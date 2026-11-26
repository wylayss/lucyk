#include <cpu.h>
#include <spinlock.h>
#include <types.h>

static inline void cpu_relax(void)
{
    __builtin_ia32_pause();
}

void spinlock_init(spinlock_t *lock)
{
    if (lock == NULL) {
        return;
    }

    __atomic_store_n(&lock->next_ticket, 0U, __ATOMIC_RELAXED);
    __atomic_store_n(&lock->now_serving, 0U, __ATOMIC_RELAXED);
    __atomic_store_n(&lock->owner_cpu, CPU_INDEX_NONE, __ATOMIC_RELAXED);
    __atomic_store_n(&lock->locked, 0U, __ATOMIC_RELAXED);
}

static void spin_acquire(spinlock_t *lock)
{
    uint32_t ticket =
        __atomic_fetch_add(&lock->next_ticket, 1U, __ATOMIC_ACQUIRE);

    while (__atomic_load_n(&lock->now_serving, __ATOMIC_ACQUIRE) != ticket) {
        cpu_relax();
    }

    /* The acquire fence pairs with the release store below, so every write
     * made by the previous holder is visible before the critical section. */
    __atomic_thread_fence(__ATOMIC_ACQUIRE);

    __atomic_store_n(&lock->owner_cpu, this_cpu_index(), __ATOMIC_RELAXED);
    __atomic_store_n(&lock->locked, 1U, __ATOMIC_RELAXED);
}

static void spin_release(spinlock_t *lock)
{
    __atomic_store_n(&lock->locked, 0U, __ATOMIC_RELAXED);
    __atomic_store_n(&lock->owner_cpu, CPU_INDEX_NONE, __ATOMIC_RELAXED);

    uint32_t next = __atomic_load_n(&lock->now_serving, __ATOMIC_RELAXED) + 1U;
    __atomic_store_n(&lock->now_serving, next, __ATOMIC_RELEASE);
}

uint64_t spin_lock_irqsave(spinlock_t *lock)
{
    uint64_t flags = irq_save();

    spin_acquire(lock);
    return flags;
}

void spin_unlock_irqrestore(spinlock_t *lock, uint64_t flags)
{
    spin_release(lock);
    irq_restore(flags);
}

void spin_lock(spinlock_t *lock)
{
    spin_acquire(lock);
}

void spin_unlock(spinlock_t *lock)
{
    spin_release(lock);
}

bool spin_trylock(spinlock_t *lock)
{
    uint32_t serving = __atomic_load_n(&lock->now_serving, __ATOMIC_ACQUIRE);
    uint32_t expected = serving;

    if (!__atomic_compare_exchange_n(&lock->next_ticket, &expected,
                                     serving + 1U, false, __ATOMIC_ACQ_REL,
                                     __ATOMIC_RELAXED)) {
        return false;
    }

    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    __atomic_store_n(&lock->owner_cpu, this_cpu_index(), __ATOMIC_RELAXED);
    __atomic_store_n(&lock->locked, 1U, __ATOMIC_RELAXED);
    return true;
}
