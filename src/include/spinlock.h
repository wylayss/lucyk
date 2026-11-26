#ifndef LUCYK_SPINLOCK_H
#define LUCYK_SPINLOCK_H

#include <io.h>
#include <types.h>

/* Ticket spinlock: strictly FIFO, which bounds the worst-case wait and keeps
 * two CPUs from starving each other under heavy contention. All storage is
 * zero-initialised by the static allocator, and spinlock_init() re-arms it. */
typedef struct spinlock {
    uint32_t next_ticket;
    uint32_t now_serving;
    uint32_t owner_cpu; /* Diagnostics only; never used for exclusion. */
    uint32_t locked;    /* Diagnostics only; never used for exclusion. */
} spinlock_t;

void spinlock_init(spinlock_t *lock);

/* Acquires the lock with interrupts disabled and returns the previous RFLAGS
 * so the caller can restore the exact interrupt state on release. */
uint64_t spin_lock_irqsave(spinlock_t *lock);

/* Releases the lock, then restores the interrupt state captured on acquire. */
void spin_unlock_irqrestore(spinlock_t *lock, uint64_t flags);

/* Acquires without touching the interrupt flag. */
void spin_lock(spinlock_t *lock);
void spin_unlock(spinlock_t *lock);

/* Non-blocking attempt used by diagnostic paths that must never wait. */
bool spin_trylock(spinlock_t *lock);

#endif /* LUCYK_SPINLOCK_H */
