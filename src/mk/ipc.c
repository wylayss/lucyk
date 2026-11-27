#include <cap.h>
#include <cpu.h>
#include <idt.h>
#include <ioapic.h>
#include <io/pci.h>
#include <io.h>
#include <ipc.h>
#include <isr.h>
#include <kalloc.h>
#include <klog.h>
#include <lapic.h>
#include <object.h>
#include <sched.h>
#include <spinlock.h>
#include <string.h>
#include <task.h>
#include <types.h>

#define IRQ_VECTOR_MIN 32U
#define IRQ_VECTOR_MAX 254U

/* Vectors handed to GSI-backed IRQ capabilities. They sit above the kernel's
 * own device vectors (VirtIO-Blk uses 0x40) and below the IPI range. */
#define IRQ_CAP_VECTOR_FIRST 0x50U
#define IRQ_CAP_VECTOR_LAST  0x6FU

typedef struct irq_binding {
    notification_t   *notification;
    uint64_t          bit;
    bool              active;
    cap_irq_object_t *gsi_owner; /* GSI-backed capability, or NULL */
} irq_binding_t;

static irq_binding_t irq_bindings[256];

/* Guards irq_bindings[] and the capability vector pool. The interrupt handler
 * takes it on whichever CPU the IOAPIC delivers to, so interrupt masking alone
 * would not exclude a concurrent unbind on another core. Lock order:
 * irq binding -> ioapic. */
static spinlock_t irq_binding_lock;
static uint32_t irq_cap_vectors_used;

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
            sched_qos_restore(thread->reply_target);
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
            /* The boost was lent by this caller; it ends with the caller. */
            sched_qos_restore(receiver);
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

        /* Priority inheritance: the receiver now owes this caller a reply. */
        sched_qos_inherit(self, sender);

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

    /* The reply ends the inherited boost before the caller can run again. */
    sched_qos_restore(receiver);
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

    uint64_t flags = spin_lock_irqsave(&irq_binding_lock);
    irq_binding_t *binding = &irq_bindings[vector];
    notification_t *notification = binding->active ? binding->notification : NULL;
    uint64_t bit = binding->bit;

    if (notification != NULL) {
        cap_irq_object_t *owner = binding->gsi_owner;

        /* Hold the line off until the driver acknowledges; the LAPIC EOI that
         * isr_dispatch() sends after this handler then cannot re-trigger a
         * level-asserted source. */
        if (owner != NULL) {
            ioapic_mask_irq(owner->gsi);
            owner->awaiting_ack = true;
            owner->fire_count++;
        }
        mk_object_retain(&notification->header);
    }
    spin_unlock_irqrestore(&irq_binding_lock, flags);

    if (notification != NULL) {
        notify_signal(notification, bit);
        mk_object_release(&notification->header);
    }
}

static sys_status_t bind_vector_locked(uint8_t vector,
                                       notification_t *notification,
                                       uint64_t bit, cap_irq_object_t *owner)
{
    irq_binding_t *binding = &irq_bindings[vector];

    if (binding->active) {
        return SYS_ERR_BUSY;
    }

    mk_object_retain(&notification->header);

    binding->notification = notification;
    binding->bit = bit;
    binding->gsi_owner = owner;
    binding->active = true;

    idt_register_handler(vector, irq_forward_handler);
    return SYS_OK;
}

sys_status_t irq_bind_notification(uint8_t vector, notification_t *notification,
                                   uint64_t bit)
{
    if (!irq_vector_valid(vector) || notification == NULL ||
        notification->destroyed) {
        return SYS_ERR_INVAL;
    }

    uint64_t flags = spin_lock_irqsave(&irq_binding_lock);
    sys_status_t status = bind_vector_locked(vector, notification, bit, NULL);
    spin_unlock_irqrestore(&irq_binding_lock, flags);

    return status;
}

sys_status_t irq_unbind_notification(uint8_t vector)
{
    if (!irq_vector_valid(vector)) {
        return SYS_ERR_INVAL;
    }

    uint64_t flags = spin_lock_irqsave(&irq_binding_lock);
    irq_binding_t *binding = &irq_bindings[vector];

    if (!binding->active) {
        spin_unlock_irqrestore(&irq_binding_lock, flags);
        return SYS_ERR_INVAL;
    }

    notification_t *notification = binding->notification;
    cap_irq_object_t *owner = binding->gsi_owner;

    if (owner != NULL) {
        ioapic_mask_irq(owner->gsi);
        owner->bound = false;
        owner->awaiting_ack = false;
    }

    binding->notification = NULL;
    binding->bit = 0;
    binding->gsi_owner = NULL;
    binding->active = false;

    idt_register_handler(vector, NULL);

    spin_unlock_irqrestore(&irq_binding_lock, flags);

    mk_object_release(&notification->header);
    return SYS_OK;
}

sys_status_t irq_cap_bind(cap_irq_object_t *irq, notification_t *notification,
                          uint64_t bit)
{
    if (irq == NULL || notification == NULL || notification->destroyed ||
        bit == 0) {
        return SYS_ERR_INVAL;
    }
    if (!irq->gsi_backed) {
        sys_status_t status =
            irq_bind_notification(irq->vector, notification, bit);

        if (status == SYS_OK) {
            irq->bound = true;
        }
        return status;
    }

    uint64_t flags = spin_lock_irqsave(&irq_binding_lock);
    sys_status_t status =
        bind_vector_locked(irq->vector, notification, bit, irq);

    if (status == SYS_OK) {
        irq->bound = true;
        irq->awaiting_ack = false;
        ioapic_unmask_irq(irq->gsi);
    }
    spin_unlock_irqrestore(&irq_binding_lock, flags);

    return status;
}

sys_status_t irq_cap_ack(cap_irq_object_t *irq)
{
    if (irq == NULL) {
        return SYS_ERR_INVAL;
    }

    uint64_t flags = spin_lock_irqsave(&irq_binding_lock);

    if (!irq->bound) {
        spin_unlock_irqrestore(&irq_binding_lock, flags);
        return SYS_ERR_INVAL;
    }
    if (irq->gsi_backed && irq->awaiting_ack) {
        irq->awaiting_ack = false;
        ioapic_unmask_irq(irq->gsi);
    }

    spin_unlock_irqrestore(&irq_binding_lock, flags);
    return SYS_OK;
}

static void irq_object_release(mk_object_t *object)
{
    cap_irq_object_t *irq = (cap_irq_object_t *)object;

    (void)irq_unbind_notification(irq->vector);

    if (irq->gsi_backed) {
        ioapic_unbind_cap_irq(irq->gsi);

        uint64_t flags = spin_lock_irqsave(&irq_binding_lock);
        irq_cap_vectors_used &=
            ~(1U << (irq->vector - IRQ_CAP_VECTOR_FIRST));
        spin_unlock_irqrestore(&irq_binding_lock, flags);
    } else if (irq->vector != 0) {
        /* MSI/MSI-X (non-GSI) vector: disable on device + release to pool. */
        if (irq->msi) {
            pci_msi_disable((uint8_t)((irq->msi_bdf >> 16) & 0xFFU),
                            (uint8_t)((irq->msi_bdf >> 8) & 0xFFU),
                            (uint8_t)(irq->msi_bdf & 0xFFU));
        }
        ioapic_free_msi_vector(irq->vector);
    }
    kfree(irq);
}

mk_object_t *cap_irq_object_create(uint8_t vector)
{
    /* vector 0 mints a fresh unbound object: no interrupt vector is claimed
     * and nothing is bound until e.g. SYS_PCI_ALLOC_MSI assigns one. */
    if (vector != 0 && !irq_vector_valid(vector)) {
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

mk_object_t *cap_irq_gsi_object_create(uint8_t gsi, bool active_low,
                                       bool level_triggered)
{
    _Static_assert(IRQ_CAP_VECTOR_LAST - IRQ_CAP_VECTOR_FIRST < 32U,
                   "capability vector pool must fit the bitmap");

    cap_irq_object_t *irq = (cap_irq_object_t *)kalloc(sizeof(cap_irq_object_t));
    if (irq == NULL) {
        return NULL;
    }
    memset(irq, 0, sizeof(*irq));

    uint8_t vector = 0;
    uint64_t flags = spin_lock_irqsave(&irq_binding_lock);

    for (uint32_t v = IRQ_CAP_VECTOR_FIRST; v <= IRQ_CAP_VECTOR_LAST; v++) {
        uint32_t bit = 1U << (v - IRQ_CAP_VECTOR_FIRST);

        if ((irq_cap_vectors_used & bit) == 0 && !irq_bindings[v].active) {
            irq_cap_vectors_used |= bit;
            vector = (uint8_t)v;
            break;
        }
    }
    spin_unlock_irqrestore(&irq_binding_lock, flags);

    if (vector == 0) {
        kfree(irq);
        return NULL;
    }

    /* External interrupts are delivered to the BSP; the notification wake path
     * reaches the driver thread on whatever CPU owns it. */
    cpu_local_t *bsp = cpu_local_for(0);
    sys_status_t status = ioapic_bind_cap_irq(gsi, vector, bsp->lapic_id,
                                              active_low, level_triggered);
    if (status != SYS_OK) {
        flags = spin_lock_irqsave(&irq_binding_lock);
        irq_cap_vectors_used &= ~(1U << (vector - IRQ_CAP_VECTOR_FIRST));
        spin_unlock_irqrestore(&irq_binding_lock, flags);
        kfree(irq);
        return NULL;
    }

    irq->vector = vector;
    irq->gsi_backed = true;
    irq->gsi = gsi;
    mk_object_init(&irq->header, CAP_OBJ_IRQ, irq_object_release);

    return &irq->header;
}
