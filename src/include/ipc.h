#ifndef LUCYK_IPC_H
#define LUCYK_IPC_H

#include <cap.h>
#include <object.h>
#include <task.h>
#include <types.h>

typedef struct endpoint {
    mk_object_t header;
    thread_t   *send_queue;
    thread_t   *send_tail;
    thread_t   *recv_queue;
    thread_t   *recv_tail;
    thread_t   *reply_queue;
    thread_t   *reply_tail;
    bool        destroyed;
} endpoint_t;

typedef struct notification {
    mk_object_t header;
    uint64_t    word;
    thread_t   *waiter;
    bool        destroyed;
} notification_t;

endpoint_t *endpoint_create(void);
void endpoint_destroy(endpoint_t *endpoint);

/* Unlinks a dying thread from all IPC/notification wait queues, wakes any
 * paired peer with SYS_ERR_DEAD, and releases its retained wait object. */
void ipc_thread_detach(thread_t *thread);

notification_t *notification_create(void);
void notification_destroy(notification_t *notification);

sys_status_t ipc_call(task_t *caller_task, cap_t ep_cap, const uint64_t msg[4],
                      uint64_t reply_out[4], uint64_t *badge_out);
sys_status_t ipc_recv(task_t *task, cap_t ep_cap, uint64_t msg_out[4],
                      uint64_t *badge_out, thread_t **sender_out);
sys_status_t ipc_reply(thread_t *caller, const uint64_t reply[4]);

void notify_signal(notification_t *notification, uint64_t bits);
sys_status_t notify_wait(task_t *task, cap_t ntf_cap, uint64_t *bits_out);

/* Binds an external interrupt vector (32..254, excluding the LAPIC timer) to
 * a notification bit. The binding retains the notification. */
sys_status_t irq_bind_notification(uint8_t vector, notification_t *notification,
                                   uint64_t bit);
sys_status_t irq_unbind_notification(uint8_t vector);

/* Capability object representing an IRQ binding. Releasing it unbinds the
 * vector, so a cap table can own the binding's lifetime. */
typedef struct cap_irq_object {
    mk_object_t header;
    uint8_t     vector;
} cap_irq_object_t;

mk_object_t *cap_irq_object_create(uint8_t vector);

#endif /* LUCYK_IPC_H */
