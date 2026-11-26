#include <cap.h>
#include <idt.h>
#include <io.h>
#include <ipc.h>
#include <isr.h>
#include <kalloc.h>
#include <klog.h>
#include <lapic.h>
#include <object.h>
#include <sched.h>
#include <string.h>
#include <task.h>
#include <types.h>

#define IRQ_VECTOR_MIN 32U
#define IRQ_VECTOR_MAX 254U

typedef struct irq_binding {
    notification_t *notification;
    uint64_t        bit;
    bool            active;
} irq_binding_t;

static irq_binding_t irq_bindings[256];

static void enqueue(thread_t **head, thread_t **tail, thread_t *thread)
{
    thread->wait_next = NULL;
    if (*tail != NULL) {
        (*tail)->wait_next = thread;
    } else {
        *head = thread;
    }
    *tail = thread;
}

static thread_t *dequeue(thread_t **head, thread_t **tail)
{
    thread_t *thread = *head;

    if (thread == NULL) {
        return NULL;
    }

    *head = thread->wait_next;
    if (*head == NULL) {
        *tail = NULL;
    }
    thread->wait_next = NULL;
    return thread;
}

static bool queue_remove(thread_t **head, thread_t **tail, thread_t *thread)
{
    thread_t *previous = NULL;
    thread_t *current = *head;

    while (current != NULL) {
        if (current == thread) {
            if (previous != NULL) {
                previous->wait_next = current->wait_next;
            } else {
                *head = current->wait_next;
            }
            if (*tail == current) {
                *tail = previous;
            }
            current->wait_next = NULL;
            return true;
        }
        previous = current;
        current = current->wait_next;
    }

    return false;
}

static void endpoint_mark_destroyed(endpoint_t *endpoint)
{
    uint64_t flags = irq_save();

    if (endpoint->destroyed) {
        irq_restore(flags);
        return;
    }

    endpoint->destroyed = true;

    thread_t *thread;
    while ((thread = dequeue(&endpoint->send_queue, &endpoint->send_tail)) != NULL) {
        thread->ipc_status = SYS_ERR_DEAD;
        sched_wake(thread);
    }
    while ((thread = dequeue(&endpoint->recv_queue, &endpoint->recv_tail)) != NULL) {
        thread->ipc_status = SYS_ERR_DEAD;
        sched_wake(thread);
    }
    while ((thread = dequeue(&endpoint->reply_queue, &endpoint->reply_tail)) != NULL) {
        if (thread->reply_target != NULL &&
            thread->reply_target->ipc_sender == thread) {
            thread->reply_target->ipc_sender = NULL;
        }
        thread->reply_target = NULL;
        thread->ipc_status = SYS_ERR_DEAD;
        sched_wake(thread);
    }

    irq_restore(flags);
}

static void endpoint_release(mk_object_t *object)
{
    endpoint_t *endpoint = (endpoint_t *)object;

    endpoint_mark_destroyed(endpoint);
    kfree(endpoint);
}

endpoint_t *endpoint_create(void)
{
    endpoint_t *endpoint = (endpoint_t *)kalloc(sizeof(endpoint_t));

    if (endpoint == NULL) {
        return NULL;
    }

    memset(endpoint, 0, sizeof(*endpoint));
    mk_object_init(&endpoint->header, CAP_OBJ_ENDPOINT, endpoint_release);
    return endpoint;
}

void endpoint_destroy(endpoint_t *endpoint)
{
    if (endpoint == NULL) {
        return;
    }

    endpoint_mark_destroyed(endpoint);
    mk_object_release(&endpoint->header);
}

static void notification_mark_destroyed(notification_t *notification)
{
    uint64_t flags = irq_save();

    if (notification->destroyed) {
        irq_restore(flags);
        return;
    }

    notification->destroyed = true;

    thread_t *waiter = notification->waiter;
    notification->waiter = NULL;

    if (waiter != NULL) {
        waiter->ipc_status = SYS_ERR_DEAD;
        sched_wake(waiter);
    }

    irq_restore(flags);
}

static void notification_release(mk_object_t *object)
{
    notification_t *notification = (notification_t *)object;

    notification_mark_destroyed(notification);
    kfree(notification);
}

notification_t *notification_create(void)
{
    notification_t *notification =
        (notification_t *)kalloc(sizeof(notification_t));

    if (notification == NULL) {
        return NULL;
    }

    memset(notification, 0, sizeof(*notification));
    mk_object_init(&notification->header, CAP_OBJ_NOTIFICATION,
                   notification_release);
    return notification;
}

void notification_destroy(notification_t *notification)
{
    if (notification == NULL) {
        return;
    }

    notification_mark_destroyed(notification);
    mk_object_release(&notification->header);
}

static void ipc_wait_begin(thread_t *thread, void *object, uint32_t kind)
{
    thread->ipc_wait_object = object;
    thread->ipc_wait_kind = kind;
}

static void ipc_wait_finish(thread_t *thread, mk_object_t *object)
{
    uint64_t flags = irq_save();

    thread->ipc_wait_object = NULL;
    thread->ipc_wait_kind = IPC_WAIT_NONE;
    mk_object_release(object);

    irq_restore(flags);
}

void ipc_thread_detach(thread_t *thread)
{
    if (thread == NULL) {
        return;
    }

    uint64_t flags = irq_save();

    if (thread->ipc_sender != NULL) {
        thread_t *caller = thread->ipc_sender;

        if (caller->ipc_wait_kind == IPC_WAIT_REPLY &&
            caller->ipc_wait_object != NULL) {
            endpoint_t *caller_endpoint = (endpoint_t *)caller->ipc_wait_object;
            (void)queue_remove(&caller_endpoint->reply_queue,
                               &caller_endpoint->reply_tail, caller);
        }

        caller->reply_target = NULL;
        caller->ipc_status = SYS_ERR_DEAD;
        sched_wake(caller);
        thread->ipc_sender = NULL;
    }

    if (thread->reply_target != NULL) {
        thread_t *receiver = thread->reply_target;

        if (receiver->ipc_sender == thread) {
            receiver->ipc_sender = NULL;
        }
        thread->reply_target = NULL;
    }

    void *object = thread->ipc_wait_object;
    uint32_t kind = thread->ipc_wait_kind;

    thread->ipc_wait_object = NULL;
    thread->ipc_wait_kind = IPC_WAIT_NONE;

    if (object != NULL) {
        if (kind == IPC_WAIT_SEND) {
            endpoint_t *endpoint = (endpoint_t *)object;
            (void)queue_remove(&endpoint->send_queue, &endpoint->send_tail,
                               thread);
        } else if (kind == IPC_WAIT_RECV) {
            endpoint_t *endpoint = (endpoint_t *)object;
            (void)queue_remove(&endpoint->recv_queue, &endpoint->recv_tail,
                               thread);
        } else if (kind == IPC_WAIT_REPLY) {
            endpoint_t *endpoint = (endpoint_t *)object;
            (void)queue_remove(&endpoint->reply_queue, &endpoint->reply_tail,
                               thread);
        } else if (kind == IPC_WAIT_NOTIFY) {
            notification_t *notification = (notification_t *)object;
            if (notification->waiter == thread) {
                notification->waiter = NULL;
            }
        }

        mk_object_release((mk_object_t *)object);
    }

    irq_restore(flags);
}

static sys_status_t caller_for_task(task_t *task, thread_t **thread_out)
{
    if (task == NULL || thread_out == NULL) {
        return SYS_ERR_INVAL;
    }

    thread_t *current = sched_current_thread();
    if (current == NULL || current->owner_task != task) {
        return SYS_ERR_INVAL;
    }

    *thread_out = current;
    return SYS_OK;
}

sys_status_t ipc_call(task_t *caller_task, cap_t ep_cap, const uint64_t msg[4],
                      uint64_t reply_out[4], uint64_t *badge_out)
{
    if (msg == NULL || reply_out == NULL) {
        return SYS_ERR_INVAL;
    }

    thread_t *caller = NULL;
    sys_status_t status = caller_for_task(caller_task, &caller);
    if (status != SYS_OK) {
        return status;
    }

    void *object = NULL;
    uint64_t cap_badge = 0;
    status = cap_lookup_retained(caller_task->caps, ep_cap, CAP_OBJ_ENDPOINT,
                                 CAP_RIGHT_CALL, &object, &cap_badge);
    if (status != SYS_OK) {
        return status;
    }

    endpoint_t *endpoint = (endpoint_t *)object;
    uint64_t flags = irq_save();

    if (endpoint->destroyed) {
        irq_restore(flags);
        mk_object_release(&endpoint->header);
        return SYS_ERR_DEAD;
    }

    thread_t *receiver = dequeue(&endpoint->recv_queue, &endpoint->recv_tail);
    if (receiver != NULL) {
        for (size_t i = 0; i < 4; i++) {
            receiver->ipc_msg_regs[i] = msg[i];
        }
        receiver->ipc_badge = cap_badge;
        receiver->ipc_sender = caller;
        receiver->ipc_status = SYS_OK;

        caller->reply_target = receiver;
        caller->state = THREAD_BLOCKED_REPLY;
        ipc_wait_begin(caller, endpoint, IPC_WAIT_REPLY);
        enqueue(&endpoint->reply_queue, &endpoint->reply_tail, caller);

        /* sched_handoff() switches directly only when the receiver shares this
         * run queue. Otherwise it just makes the receiver runnable, so the
         * caller must park itself; either way control returns once the receiver
         * replies (or the endpoint is torn down). */
        sched_handoff(receiver);

        if (caller->state == THREAD_BLOCKED_REPLY) {
            sched_block_current();
        }
        irq_restore(flags);
        ipc_wait_finish(caller, &endpoint->header);

        if (caller->ipc_status != SYS_OK) {
            return caller->ipc_status;
        }
        for (size_t i = 0; i < 4; i++) {
            reply_out[i] = caller->ipc_msg_regs[i];
        }
        if (badge_out != NULL) {
            *badge_out = cap_badge;
        }
        return SYS_OK;
    }

    for (size_t i = 0; i < 4; i++) {
        caller->ipc_msg_regs[i] = msg[i];
    }
    caller->ipc_badge = cap_badge;
    caller->reply_target = NULL;
    caller->ipc_status = SYS_OK;
    caller->state = THREAD_BLOCKED_SEND;
    ipc_wait_begin(caller, endpoint, IPC_WAIT_SEND);

    enqueue(&endpoint->send_queue, &endpoint->send_tail, caller);

    sched_block_current();
    irq_restore(flags);
    ipc_wait_finish(caller, &endpoint->header);

    if (caller->ipc_status != SYS_OK) {
        return caller->ipc_status;
    }
    for (size_t i = 0; i < 4; i++) {
        reply_out[i] = caller->ipc_msg_regs[i];
    }
    if (badge_out != NULL) {
        *badge_out = cap_badge;
    }
    return SYS_OK;
}

sys_status_t ipc_recv(task_t *task, cap_t ep_cap, uint64_t msg_out[4],
                      uint64_t *badge_out, thread_t **sender_out)
{
    if (msg_out == NULL || sender_out == NULL) {
        return SYS_ERR_INVAL;
    }

    thread_t *self = NULL;
    sys_status_t status = caller_for_task(task, &self);
    if (status != SYS_OK) {
        return status;
    }

    void *object = NULL;
    status = cap_lookup_retained(task->caps, ep_cap, CAP_OBJ_ENDPOINT,
                                 CAP_RIGHT_READ, &object, NULL);
    if (status != SYS_OK) {
        return status;
    }

    endpoint_t *endpoint = (endpoint_t *)object;
    uint64_t flags = irq_save();

    if (endpoint->destroyed) {
        irq_restore(flags);
        mk_object_release(&endpoint->header);
        return SYS_ERR_DEAD;
    }
    if (self->ipc_sender != NULL) {
        irq_restore(flags);
        mk_object_release(&endpoint->header);
        return SYS_ERR_BUSY;
    }

    thread_t *sender = dequeue(&endpoint->send_queue, &endpoint->send_tail);
    if (sender != NULL) {
        for (size_t i = 0; i < 4; i++) {
            msg_out[i] = sender->ipc_msg_regs[i];
        }
        if (badge_out != NULL) {
            *badge_out = sender->ipc_badge;
        }
        *sender_out = sender;
        self->ipc_sender = sender;

        sender->reply_target = self;
        sender->state = THREAD_BLOCKED_REPLY;
        sender->ipc_status = SYS_OK;
        sender->ipc_wait_kind = IPC_WAIT_REPLY;
        enqueue(&endpoint->reply_queue, &endpoint->reply_tail, sender);

        irq_restore(flags);
        mk_object_release(&endpoint->header);
        return SYS_OK;
    }

    self->ipc_status = SYS_OK;
    self->ipc_sender = NULL;
    self->state = THREAD_BLOCKED_RECV;
    ipc_wait_begin(self, endpoint, IPC_WAIT_RECV);
    enqueue(&endpoint->recv_queue, &endpoint->recv_tail, self);

    sched_block_current();
    irq_restore(flags);
    ipc_wait_finish(self, &endpoint->header);

    if (self->ipc_status != SYS_OK) {
        return self->ipc_status;
    }

    sender = self->ipc_sender;
    if (sender == NULL) {
        return SYS_ERR_DEAD;
    }

    for (size_t i = 0; i < 4; i++) {
        msg_out[i] = self->ipc_msg_regs[i];
    }
    if (badge_out != NULL) {
        *badge_out = self->ipc_badge;
    }
    *sender_out = sender;
    return SYS_OK;
}

sys_status_t ipc_reply(thread_t *caller, const uint64_t reply[4])
{
    if (caller == NULL || reply == NULL) {
        return SYS_ERR_INVAL;
    }

    thread_t *receiver = sched_current_thread();
    if (receiver == NULL) {
        return SYS_ERR_INVAL;
    }

    uint64_t flags = irq_save();

    /* Validate the pairing before dereferencing a caller pointer that the
     * receiver may have cached across an unrelated task termination. */
    if (caller != receiver->ipc_sender) {
        irq_restore(flags);
        return SYS_ERR_INVAL;
    }
    if (caller->state != THREAD_BLOCKED_REPLY ||
        caller->reply_target != receiver) {
        irq_restore(flags);
        return SYS_ERR_INVAL;
    }

    if (caller->ipc_wait_kind == IPC_WAIT_REPLY &&
        caller->ipc_wait_object != NULL) {
        endpoint_t *endpoint = (endpoint_t *)caller->ipc_wait_object;
        (void)queue_remove(&endpoint->reply_queue, &endpoint->reply_tail,
                           caller);
    }

    for (size_t i = 0; i < 4; i++) {
        caller->ipc_msg_regs[i] = reply[i];
    }
    caller->ipc_status = SYS_OK;
    caller->reply_target = NULL;
    receiver->ipc_sender = NULL;
    sched_wake(caller);

    irq_restore(flags);
    return SYS_OK;
}

void notify_signal(notification_t *notification, uint64_t bits)
{
    if (notification == NULL) {
        return;
    }

    uint64_t flags = irq_save();

    if (notification->destroyed) {
        irq_restore(flags);
        return;
    }

    notification->word |= bits;

    thread_t *waiter = notification->waiter;
    if (waiter != NULL) {
        notification->waiter = NULL;
        waiter->ipc_status = SYS_OK;
        sched_wake(waiter);
    }

    irq_restore(flags);
}

sys_status_t notify_wait(task_t *task, cap_t ntf_cap, uint64_t *bits_out)
{
    if (bits_out == NULL) {
        return SYS_ERR_INVAL;
    }

    thread_t *self = NULL;
    sys_status_t status = caller_for_task(task, &self);
    if (status != SYS_OK) {
        return status;
    }

    void *object = NULL;
    status = cap_lookup_retained(task->caps, ntf_cap, CAP_OBJ_NOTIFICATION,
                                 CAP_RIGHT_READ, &object, NULL);
    if (status != SYS_OK) {
        return status;
    }

    notification_t *notification = (notification_t *)object;
    uint64_t flags = irq_save();

    if (notification->destroyed) {
        irq_restore(flags);
        mk_object_release(&notification->header);
        return SYS_ERR_DEAD;
    }
    if (notification->waiter != NULL) {
        irq_restore(flags);
        mk_object_release(&notification->header);
        return SYS_ERR_BUSY;
    }
    if (notification->word != 0) {
        *bits_out = notification->word;
        notification->word = 0;
        irq_restore(flags);
        mk_object_release(&notification->header);
        return SYS_OK;
    }

    self->ipc_status = SYS_OK;
    notification->waiter = self;
    self->state = THREAD_BLOCKED_NOTIFY;
    ipc_wait_begin(self, notification, IPC_WAIT_NOTIFY);

    sched_block_current();
    irq_restore(flags);

    if (self->ipc_status != SYS_OK) {
        status = self->ipc_status;
        ipc_wait_finish(self, &notification->header);
        return status;
    }

    uint64_t word_flags = irq_save();
    uint64_t bits = notification->word;
    notification->word = 0;
    irq_restore(word_flags);

    *bits_out = bits;
    ipc_wait_finish(self, &notification->header);
    return SYS_OK;
}

static bool irq_vector_valid(uint8_t vector)
{
    if (vector < IRQ_VECTOR_MIN || vector > IRQ_VECTOR_MAX) {
        return false;
    }
    if (vector == LAPIC_TIMER_VECTOR) {
        return false;
    }
    return true;
}

static void irq_forward_handler(interrupt_frame_t *frame)
{
    uint8_t vector = (uint8_t)frame->vector;

    uint64_t flags = irq_save();
    irq_binding_t *binding = &irq_bindings[vector];
    notification_t *notification = binding->active ? binding->notification : NULL;
    uint64_t bit = binding->bit;
    irq_restore(flags);

    if (notification != NULL) {
        notify_signal(notification, bit);
    }
}

sys_status_t irq_bind_notification(uint8_t vector, notification_t *notification,
                                   uint64_t bit)
{
    if (!irq_vector_valid(vector) || notification == NULL ||
        notification->destroyed) {
        return SYS_ERR_INVAL;
    }

    uint64_t flags = irq_save();
    irq_binding_t *binding = &irq_bindings[vector];

    if (binding->active) {
        irq_restore(flags);
        return SYS_ERR_BUSY;
    }

    mk_object_retain(&notification->header);

    binding->notification = notification;
    binding->bit = bit;
    binding->active = true;

    idt_register_handler(vector, irq_forward_handler);

    irq_restore(flags);
    return SYS_OK;
}

sys_status_t irq_unbind_notification(uint8_t vector)
{
    if (!irq_vector_valid(vector)) {
        return SYS_ERR_INVAL;
    }

    uint64_t flags = irq_save();
    irq_binding_t *binding = &irq_bindings[vector];

    if (!binding->active) {
        irq_restore(flags);
        return SYS_ERR_INVAL;
    }

    notification_t *notification = binding->notification;

    binding->notification = NULL;
    binding->bit = 0;
    binding->active = false;

    idt_register_handler(vector, NULL);

    irq_restore(flags);

    mk_object_release(&notification->header);
    return SYS_OK;
}

static void irq_object_release(mk_object_t *object)
{
    cap_irq_object_t *irq = (cap_irq_object_t *)object;

    (void)irq_unbind_notification(irq->vector);
    kfree(irq);
}

mk_object_t *cap_irq_object_create(uint8_t vector)
{
    if (!irq_vector_valid(vector)) {
        return NULL;
    }

    cap_irq_object_t *irq = (cap_irq_object_t *)kalloc(sizeof(cap_irq_object_t));
    if (irq == NULL) {
        return NULL;
    }

    memset(irq, 0, sizeof(*irq));
    irq->vector = vector;
    mk_object_init(&irq->header, CAP_OBJ_IRQ, irq_object_release);

    return &irq->header;
}
