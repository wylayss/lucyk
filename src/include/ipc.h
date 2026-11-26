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
 * vector, so a cap table can own the binding's lifetime.
 *
 * A GSI-backed object (cap_irq_gsi_object_create) additionally owns an IOAPIC
 * line: the kernel allocates a vector for it and routes the GSI there, masked.
 * Binding a notification unmasks the line. When the interrupt fires the
 * kernel masks the GSI, sends the LAPIC EOI and ORs the bound bit into the
 * notification; the Ring 3 driver services its device and acknowledges with
 * irq_cap_ack() (SYS_IRQ_ACK), which unmasks the line again. A level-triggered
 * device therefore cannot storm the CPU while its driver is still working. */
typedef struct cap_irq_object {
    mk_object_t header;
    uint8_t     vector;
    bool        gsi_backed;
    uint8_t     gsi;
    bool        bound;        /* A notification is attached */
    bool        awaiting_ack; /* Fired and masked; waiting for irq_cap_ack */
    uint64_t    fire_count;   /* Deliveries observed (diagnostics) */
} cap_irq_object_t;

mk_object_t *cap_irq_object_create(uint8_t vector);

/* Allocates a kernel vector for gsi and routes it (masked) through the IOAPIC.
 * Returns NULL when the GSI is unavailable or no vector is free. */
mk_object_t *cap_irq_gsi_object_create(uint8_t gsi, bool active_low,
                                       bool level_triggered);

/* Attaches `notification` to an IRQ object: each interrupt ORs `bit` into it.
 * For a GSI-backed object this unmasks the line. */
sys_status_t irq_cap_bind(cap_irq_object_t *irq, notification_t *notification,
                          uint64_t bit);

/* Re-arms a GSI-backed IRQ after its driver serviced the device (unmasks the
 * line). A vector-only object has nothing to unmask and succeeds. Returns
 * SYS_ERR_INVAL when no notification is bound. */
sys_status_t irq_cap_ack(cap_irq_object_t *irq);

#endif /* LUCYK_IPC_H */
