#include <cpu.h>
#include <io.h>
#include <ipc.h>
#include <kalloc.h>
#include <klog.h>
#include <lapic.h>
#include <mk_hooks.h>
#include <sched.h>
#include <smp.h>
#include <spinlock.h>
#include <string.h>
#include <syscall.h>
#include <task.h>
#include <types.h>
#include <vspace.h>

/* Each CPU owns one ring of threads plus its own idle context. A thread is
 * linked into exactly one ring and is only ever dispatched by that CPU, so two
 * CPUs can never run the same thread_t: a save/restore of the same context on
 * two cores is structurally impossible.
 *
 * Dispatch order is QoS first: the runnable thread with the highest
 * effective_qos wins, and threads of one class rotate round-robin. Every time a
 * runnable thread loses a dispatch to a higher class its qos_age grows; once
 * it reaches its class limit it is dispatched for one quantum regardless of
 * class, so DEFAULT and BACKGROUND work keeps a guaranteed share (at least one
 * quantum every QOS_AGE_LIMIT_* + 1 dispatches) under interactive load. */
typedef struct cpu_runqueue {
    spinlock_t lock;
    thread_t  *ring; /* Circular list of every thread owned by this CPU. */
    thread_t  *idle; /* This CPU's idle context; never a worker. */
    uint32_t   count;
    uint64_t   dispatch; /* Diagnostics: switches onto this CPU. */
} cpu_runqueue_t;

static cpu_runqueue_t runqueues[MAX_CPUS];

static task_t boot_task;
static thread_t boot_thread;

static thread_t *reap_list;
static spinlock_t reap_lock;

static uint64_t sched_ticks;
static uint64_t reaped_count;
static spinlock_t clock_lock;

static volatile uint32_t reschedule_pending[MAX_CPUS];

/* Dispatches a runnable thread may lose to higher classes before it is run
 * regardless of class. INTERACTIVE is the top class and never ages. */
#define QOS_AGE_LIMIT_DEFAULT    8U
#define QOS_AGE_LIMIT_BACKGROUND 16U

/* Leaf lock serialising base/inherited/effective QoS updates. */
static spinlock_t qos_lock;

/* Rotating start index for placement tie-breaks. Successive threads therefore
 * land on different CPUs even when every run queue is equally loaded. */
static uint32_t placement_cursor;

/* Threads that have left the run queue but are still executing on their own
 * kernel stacks. thread_exit() pushes onto this per-CPU list and reaping
 * happens only after another context has taken over the CPU, so a stack is
 * never freed while it is still in use. A list (not a single slot) is required
 * because several threads can exit before any of them is reclaimed. */
static thread_t *zombie_thread[MAX_CPUS];

extern void thread_entry_start(void);

thread_t *sched_boot_thread(void)
{
    return &boot_thread;
}

static cpu_runqueue_t *queue_for(uint32_t cpu)
{
    return cpu < MAX_CPUS ? &runqueues[cpu] : NULL;
}

static thread_t *current_of(uint32_t cpu)
{
    cpu_local_t *local = cpu_local_for(cpu);

    if (local == NULL) {
        return NULL;
    }

    return (thread_t *)(uintptr_t)__atomic_load_n(&local->current_thread,
                                                  __ATOMIC_ACQUIRE);
}

static void set_current_of(uint32_t cpu, thread_t *thread)
{
    cpu_local_t *local = cpu_local_for(cpu);

    if (local != NULL) {
        __atomic_store_n(&local->current_thread, (uint64_t)(uintptr_t)thread,
                         __ATOMIC_RELEASE);
    }
}

thread_t *sched_current_thread(void)
{
    return current_of(this_cpu_index());
}

static bool is_idle_thread(const thread_t *thread)
{
    return thread != NULL && thread->tid == 0;
}

/* Caller must hold rq->lock. */
static void ring_insert(cpu_runqueue_t *rq, thread_t *thread)
{
    thread->sched_next = thread;

    if (rq->ring == NULL) {
        rq->ring = thread;
    } else {
        thread->sched_next = rq->ring->sched_next;
        rq->ring->sched_next = thread;
        rq->ring = thread;
    }

    rq->count++;
}

/* Caller must hold rq->lock. Returns false when the thread is not linked. */
static bool ring_remove(cpu_runqueue_t *rq, thread_t *thread)
{
    if (rq->ring == NULL) {
        return false;
    }

    thread_t *prev = rq->ring;
    thread_t *cursor = rq->ring->sched_next;

    while (cursor != rq->ring && cursor != thread) {
        prev = cursor;
        cursor = cursor->sched_next;
    }

    if (cursor != thread) {
        return false;
    }

    if (cursor->sched_next == cursor) {
        rq->ring = NULL;
    } else {
        prev->sched_next = cursor->sched_next;
        if (rq->ring == thread) {
            rq->ring = prev;
        }
    }

    thread->sched_next = NULL;
    if (rq->count > 0) {
        rq->count--;
    }
    return true;
}

static uint32_t qos_age_limit(uint8_t qos)
{
    switch (qos) {
    case THREAD_QOS_BACKGROUND:
        return QOS_AGE_LIMIT_BACKGROUND;
    case THREAD_QOS_DEFAULT:
        return QOS_AGE_LIMIT_DEFAULT;
    default:
        return 0;
    }
}

static uint8_t qos_of(const thread_t *thread)
{
    return __atomic_load_n(&thread->effective_qos, __ATOMIC_RELAXED);
}

/* Caller must hold rq->lock. `current` competes only when current_eligible
 * (a preemption check), and then only while it is still RUNNING. */
static bool pick_candidate(const cpu_runqueue_t *rq, const thread_t *thread,
                           const thread_t *current, bool current_eligible)
{
    if (is_idle_thread(thread) || thread == rq->idle) {
        return false;
    }
    if (thread == current) {
        return current_eligible && thread->state == THREAD_RUNNING;
    }
    return thread->state == THREAD_READY;
}

/* Caller must hold rq->lock. Returns the thread that should own the CPU next:
 * the first aged thread, otherwise the first highest-QoS candidate in ring
 * order starting after `after` (so equal classes rotate and `current`, which
 * the walk visits last, only keeps the CPU when nothing better or equal is
 * runnable). Updates the aging credits of every candidate that lost. */
static thread_t *ring_pick_next_qos(cpu_runqueue_t *rq, thread_t *after,
                                    thread_t *current, bool current_eligible)
{
    if (rq->ring == NULL || rq->count == 0) {
        return NULL;
    }

    /* `after` may already have been unlinked (a thread selecting its own
     * successor while exiting), in which case its sched_next is NULL and the
     * walk must begin at the ring head instead. */
    thread_t *start = rq->ring->sched_next;

    if (after != NULL && after->sched_next != NULL) {
        start = after->sched_next;
    }

    thread_t *best = NULL;
    thread_t *aged = NULL;
    thread_t *cursor = start;

    do {
        if (pick_candidate(rq, cursor, current, current_eligible)) {
            uint8_t qos = qos_of(cursor);
            uint32_t limit = qos_age_limit(qos);

            if (aged == NULL && limit != 0 && cursor->qos_age >= limit) {
                aged = cursor;
            }
            if (best == NULL || qos > qos_of(best)) {
                best = cursor;
            }
        }
        cursor = cursor->sched_next;
    } while (cursor != start);

    if (best == NULL) {
        return NULL;
    }

    thread_t *chosen = (aged != NULL) ? aged : best;
    uint8_t best_qos = qos_of(best);

    cursor = start;
    do {
        if (cursor != chosen &&
            pick_candidate(rq, cursor, current, current_eligible) &&
            qos_of(cursor) < best_qos && cursor->qos_age < 0xFFFFFFFFU) {
            cursor->qos_age++;
        }
        cursor = cursor->sched_next;
    } while (cursor != start);

    chosen->qos_age = 0;
    return chosen;
}

/* Caller must hold rq->lock. Next runnable thread other than the caller. */
static thread_t *ring_pick_next(cpu_runqueue_t *rq, thread_t *after)
{
    return ring_pick_next_qos(rq, after, after, false);
}

static void qos_recompute_locked(thread_t *thread)
{
    uint8_t effective = thread->base_qos;

    if (thread->inherited_qos != THREAD_QOS_NONE &&
        thread->inherited_qos > effective) {
        effective = thread->inherited_qos;
    }
    __atomic_store_n(&thread->effective_qos, effective, __ATOMIC_RELAXED);
}

void sched_qos_set_base(thread_t *thread, uint8_t qos)
{
    if (thread == NULL || qos >= THREAD_QOS_COUNT) {
        return;
    }

    uint64_t flags = spin_lock_irqsave(&qos_lock);
    thread->base_qos = qos;
    qos_recompute_locked(thread);
    spin_unlock_irqrestore(&qos_lock, flags);
}

void sched_qos_inherit(thread_t *server, const thread_t *client)
{
    if (server == NULL || client == NULL || server == client) {
        return;
    }

    uint8_t lent = qos_of(client);
    uint64_t flags = spin_lock_irqsave(&qos_lock);

    if (server->inherited_qos == THREAD_QOS_NONE ||
        lent > server->inherited_qos) {
        server->inherited_qos = lent;
    }
    qos_recompute_locked(server);

    spin_unlock_irqrestore(&qos_lock, flags);
}

void sched_qos_restore(thread_t *server)
{
    if (server == NULL) {
        return;
    }

    uint64_t flags = spin_lock_irqsave(&qos_lock);
    server->inherited_qos = THREAD_QOS_NONE;
    qos_recompute_locked(server);
    spin_unlock_irqrestore(&qos_lock, flags);
}

static void reap_dead_threads(void)
{
    for (;;) {
        uint64_t flags = spin_lock_irqsave(&reap_lock);
        thread_t *dead = reap_list;

        if (dead == NULL) {
            spin_unlock_irqrestore(&reap_lock, flags);
            return;
        }

        reap_list = dead->sched_next;
        dead->sched_next = NULL;
        spin_unlock_irqrestore(&reap_lock, flags);

        /* Page-table and heap work happens with no lock held: the hierarchy is
         * heap -> vspace -> PMM, and reaping touches all three. */
        uint8_t *stack = dead->kstack_base;
        dead->kstack_base = NULL;

        if (stack != NULL) {
            kstack_free((vaddr_t)(uintptr_t)stack);
        }
        kfree(dead);

        __atomic_add_fetch(&reaped_count, 1ULL, __ATOMIC_ACQ_REL);
    }
}

static void reap_enqueue(thread_t *thread)
{
    uint64_t flags = spin_lock_irqsave(&reap_lock);

    thread->sched_next = reap_list;
    reap_list = thread;

    spin_unlock_irqrestore(&reap_lock, flags);
}

/* Publishes the CPU's deferred zombie and drains the reap list. Must be called
 * only from a context that is not running on the zombie's stack. */
static void reap_publish_zombie(uint32_t cpu)
{
    thread_t *zombie = zombie_thread[cpu];

    zombie_thread[cpu] = NULL;

    while (zombie != NULL) {
        thread_t *next = zombie->sched_next;

        zombie->sched_next = NULL;
        reap_enqueue(zombie);
        zombie = next;
    }

    reap_dead_threads();
}

/* IA32_FS_BASE MSR address for thread-local storage. */
#define IA32_FS_BASE_MSR 0xC0000100U

static uint64_t read_msr(uint32_t msr)
{
    uint32_t low;
    uint32_t high;

    __asm__ volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
    return ((uint64_t)high << 32) | low;
}

static void write_msr(uint32_t msr, uint64_t value)
{
    __asm__ volatile("wrmsr"
                     :
                     : "c"(msr), "a"((uint32_t)value), "d"((uint32_t)(value >> 32))
                     : "memory");
}

/* Switches the calling CPU to next. next must already be linked in this CPU's
 * ring (workers) or be this CPU's idle context. */
static void switch_to(thread_t *next)
{
    uint32_t cpu = this_cpu_index();
    thread_t *prev = sched_current_thread();

    if (prev == next) {
        return;
    }
    if (next == NULL || next->kstack_ctx == NULL) {
        kpanic("sched: target thread has no saved context");
    }

    next->cpu_index = cpu;

    if (prev != NULL && prev->state == THREAD_RUNNING) {
        prev->state = THREAD_READY;
    }
    next->state = THREAD_RUNNING;

    paddr_t next_cr3 = next->owner_task->vspace->pml4_phys;
    vspace_t *prev_vspace = (prev != NULL) ? prev->owner_task->vspace : NULL;
    vspace_t *next_vspace = next->owner_task->vspace;
    paddr_t prev_cr3 = (prev_vspace != NULL) ? prev_vspace->pml4_phys : 0;

    cpu_runqueue_t *rq = queue_for(cpu);
    if (rq != NULL) {
        uint64_t flags = spin_lock_irqsave(&rq->lock);
        rq->dispatch++;
        spin_unlock_irqrestore(&rq->lock, flags);
    }

    set_current_of(cpu, next);

    /* Register this CPU as a TLB user of the incoming address space before it
     * becomes loadable, and retire it from the outgoing space afterwards. */
    vspace_note_cpu_enter(next_vspace, cpu);

    /* Save the outgoing thread's IA32_FS_BASE and load the incoming one so
     * each user thread observes its own TLS segment across context switches. */
    if (prev != NULL) {
        prev->fs_base = read_msr(IA32_FS_BASE_MSR);
    }
    write_msr(IA32_FS_BASE_MSR, next->fs_base);

    /* FXSAVE64/FXRSTOR64 run inside switch_context, around the stack swap,
     * so every thread resumes with exactly the FPU/SSE state it left with.
     * A NULL `prev` (a CPU with no current thread yet) saves its outgoing
     * context into a scratch slot instead of dereferencing NULL; `old_fx` is
     * already NULL in that case. */
    static cpu_context_t *abandoned_ctx[MAX_CPUS];
    cpu_context_t **save_ctx =
        (prev != NULL) ? &prev->kstack_ctx : &abandoned_ctx[cpu];

    switch_context(save_ctx, next->kstack_ctx, next->kstack_top,
                   (next_cr3 != prev_cr3) ? next_cr3 : 0,
                   (prev != NULL) ? prev->fxsave_area : NULL,
                   next->fxsave_area);

    /* `prev` may have been reclaimed while switched out, so the outgoing
     * address space is taken from the snapshot above, never from prev. */
    if (prev_vspace != NULL && prev_vspace != next_vspace) {
        vspace_note_cpu_exit(prev_vspace, cpu);
    }

    /* Back on a different stack: the context just left can no longer be
     * running, so it and other dead threads can be reclaimed. */
    reap_publish_zombie(cpu);
}

void sched_init(void)
{
    for (uint32_t cpu = 0; cpu < MAX_CPUS; cpu++) {
        spinlock_init(&runqueues[cpu].lock);
        runqueues[cpu].ring = NULL;
        runqueues[cpu].idle = NULL;
        runqueues[cpu].count = 0;
        runqueues[cpu].dispatch = 0;
        reschedule_pending[cpu] = 0;
    }

    spinlock_init(&reap_lock);
    spinlock_init(&clock_lock);
    spinlock_init(&qos_lock);

    memset(&boot_task, 0, sizeof(boot_task));
    memset(&boot_thread, 0, sizeof(boot_thread));

    boot_task.id = 0;
    boot_task.vspace = vspace_kernel();
    boot_task.threads = &boot_thread;
    boot_task.thread_count = 1;
    boot_task.affinity_cpu = 0;

    /* boot_task is a static rather than a task_create() result, so its thread
     * lock is armed explicitly here. */
    spinlock_init(&boot_task.thread_lock);

    boot_thread.tid = 0;
    boot_thread.owner_task = &boot_task;
    boot_thread.cpu_index = 0;
    boot_thread.base_qos = (uint8_t)THREAD_QOS_DEFAULT;
    boot_thread.effective_qos = (uint8_t)THREAD_QOS_DEFAULT;
    boot_thread.inherited_qos = (uint8_t)THREAD_QOS_NONE;
    thread_fpu_init(&boot_thread);

    /* The idle context gets the same guarded five-page slot as every worker.
     * kernel_main switches onto it immediately after sched_init(). */
    vaddr_t idle_stack = kstack_alloc();
    if (idle_stack == 0) {
        kpanic("sched: cannot allocate the idle kernel stack");
    }
    boot_thread.kstack_base = (uint8_t *)(uintptr_t)idle_stack;
    boot_thread.kstack_top = idle_stack + KSTACK_SLOT_SIZE;
    boot_thread.state = THREAD_RUNNING;
    boot_thread.sched_next = &boot_thread;

    runqueues[0].ring = &boot_thread;
    runqueues[0].idle = &boot_thread;
    runqueues[0].count = 0;

    cpu_local_for(0)->idle_thread = (uint64_t)(uintptr_t)&boot_thread;
    set_current_of(0, &boot_thread);

    reap_list = NULL;
    sched_ticks = 0;
    reaped_count = 0;
}

/* AP idle body. Never linked as a worker; it exists only as this CPU's
 * fallback context when no worker is runnable. */
static void sched_idle_task(void *arg)
{
    UNUSED(arg);

    /* Running on this CPU's idle stack now. Publishing here is what releases
     * the BSP onto the next AP: the bootstrap stack this context abandoned can
     * only be recycled afterwards. The BSP reclaims it, not this CPU, because
     * the BSP is the one that can synchronise the flush with its own bring-up
     * loop. */
    smp_cpu_park_ready(this_cpu_index());

    for (;;) {
        __asm__ volatile("sti; hlt; cli" : : : "memory");
        sched_tick();
    }
}

/* Registers an AP in the scheduler. The BSP-prepared guarded stack the AP is
 * currently executing on belongs to the bootstrap path, so the idle context
 * gets a slot of its own: adopting the live stack would overwrite the frames
 * this very code is using. */
void sched_register_cpu(uint32_t cpu_index)
{
    if (cpu_index == 0 || cpu_index >= MAX_CPUS) {
        return;
    }

    cpu_runqueue_t *rq = queue_for(cpu_index);
    cpu_local_t *local = cpu_local_for(cpu_index);

    vaddr_t idle_stack = kstack_alloc();
    if (idle_stack == 0) {
        kpanic("sched: cannot allocate an idle stack for CPU %u", cpu_index);
    }

    thread_t *idle = (thread_t *)kalloc(sizeof(thread_t));
    if (idle == NULL) {
        kpanic("sched: cannot allocate an idle thread for CPU %u", cpu_index);
    }
    memset(idle, 0, sizeof(*idle));

    idle->tid = 0;
    idle->owner_task = &boot_task;
    idle->cpu_index = cpu_index;
    idle->kstack_base = (uint8_t *)(uintptr_t)idle_stack;
    idle->kstack_top = idle_stack + KSTACK_SLOT_SIZE;
    idle->state = THREAD_READY;
    idle->sched_next = idle;
    idle->entry = sched_idle_task;
    idle->arg = NULL;
    idle->base_qos = (uint8_t)THREAD_QOS_DEFAULT;
    idle->effective_qos = (uint8_t)THREAD_QOS_DEFAULT;
    idle->inherited_qos = (uint8_t)THREAD_QOS_NONE;
    thread_fpu_init(idle);

    cpu_context_t *ctx =
        (cpu_context_t *)(uintptr_t)(idle->kstack_top - sizeof(cpu_context_t));
    memset(ctx, 0, sizeof(*ctx));
    ctx->r12 = (uint64_t)(uintptr_t)idle;
    ctx->rip = (uint64_t)(uintptr_t)thread_entry_start;
    idle->kstack_ctx = ctx;

    uint64_t flags = spin_lock_irqsave(&rq->lock);

    rq->idle = idle;
    rq->ring = idle;
    rq->count = 0;

    spin_unlock_irqrestore(&rq->lock, flags);

    local->idle_thread = (uint64_t)(uintptr_t)idle;
    set_current_of(cpu_index, idle);
}

void sched_idle_wait(void)
{
    __asm__ volatile("sti; hlt; cli" : : : "memory");
    sched_tick();
}

/* Enters an AP's idle context for the first time by switching onto its saved
 * context. Calling the idle body directly would run it on the bootstrap stack,
 * which the AP must be able to abandon; the save area is a dead local because
 * control never returns here. */
NORETURN void sched_enter_idle(void)
{
    uint32_t cpu = this_cpu_index();
    cpu_runqueue_t *rq = queue_for(cpu);

    if (rq == NULL || rq->idle == NULL) {
        cpu_halt();
    }

    thread_t *idle = rq->idle;
    cpu_context_t *abandoned = NULL;

    idle->state = THREAD_RUNNING;
    switch_context(&abandoned, idle->kstack_ctx, idle->kstack_top, 0, NULL,
                   idle->fxsave_area);

    cpu_halt();
    __builtin_unreachable();
}

static thread_t *select_next_local(cpu_runqueue_t *rq, thread_t *current)
{
    thread_t *next = ring_pick_next(rq, current);

    if (next == NULL) {
        next = rq->idle;
    }

    return next;
}

/* Preemption variant: the running worker competes with the ready threads, so
 * it keeps the CPU against lower classes and rotates within its own. */
static thread_t *select_next_preempt(cpu_runqueue_t *rq, thread_t *current)
{
    bool eligible = current != NULL && !is_idle_thread(current) &&
                    current->state == THREAD_RUNNING;
    thread_t *next = ring_pick_next_qos(rq, current, current, eligible);

    if (next == NULL) {
        next = rq->idle;
    }

    return next;
}

/* Picks the run queue that should own a newly created thread: the least loaded
 * CPU among those that are online and have an idle context. A thread is only
 * ever dispatched by the CPU whose ring holds it, so this placement is what
 * makes work reach every core instead of piling onto the creating CPU. */
static uint32_t pick_target_cpu(void)
{
    if (!smp_multicore_active()) {
        return this_cpu_index();
    }

    uint32_t best = this_cpu_index();
    uint32_t best_load = 0xFFFFFFFFU;
    uint32_t start = 0;

    {
        uint64_t flags = spin_lock_irqsave(&clock_lock);
        start = placement_cursor++ % MAX_CPUS;
        spin_unlock_irqrestore(&clock_lock, flags);
    }

    for (uint32_t i = 0; i < MAX_CPUS; i++) {
        uint32_t cpu = (start + i) % MAX_CPUS;
        cpu_runqueue_t *rq = queue_for(cpu);
        cpu_local_t *local = cpu_local_for(cpu);

        if (rq == NULL || local == NULL || rq->idle == NULL ||
            !__atomic_load_n(&local->online, __ATOMIC_ACQUIRE)) {
            continue;
        }

        uint64_t flags = spin_lock_irqsave(&rq->lock);
        uint32_t load = rq->count;
        spin_unlock_irqrestore(&rq->lock, flags);

        if (load < best_load) {
            best = cpu;
            best_load = load;
        }
    }

    return best;
}

/* Resolves the CPU that must own `thread`.
 *
 * A task's threads stay on one core: the rendezvous fast path hands a receiver
 * control directly (sched_handoff), which is only sound inside a single run
 * queue, and per-task state (capability table, thread list, descriptor tables)
 * is protected by interrupt masking that excludes one core only. A task marked
 * TASK_AFFINITY_ANY is instead spread over the least loaded online CPU, which
 * is how the SMP stress test reaches every core. */
static uint32_t placement_cpu_for(thread_t *thread)
{
    task_t *task = thread->owner_task;
    uint32_t local = this_cpu_index();

    if (task == NULL || task->affinity_cpu == TASK_AFFINITY_ANY) {
        return pick_target_cpu();
    }

    uint32_t cpu = task->affinity_cpu;

    if (cpu >= MAX_CPUS) {
        return local;
    }
    if (cpu == local) {
        return cpu;
    }

    cpu_local_t *target = cpu_local_for(cpu);

    /* The owning core must have an idle context before it can accept a
     * thread; otherwise the creator keeps it. */
    if (target == NULL || !__atomic_load_n(&target->online, __ATOMIC_ACQUIRE)) {
        return local;
    }

    return cpu;
}

void sched_add(thread_t *thread)
{
    if (thread == NULL) {
        return;
    }

    /* Placement happens before any lock is taken: reading every ring's load
     * requires each lock in turn, and holding one while acquiring another
     * would allow a two-CPU deadlock. */
    uint32_t cpu = placement_cpu_for(thread);
    cpu_runqueue_t *rq = queue_for(cpu);
    cpu_local_t *local = cpu_local_for(cpu);

    uint64_t flags = spin_lock_irqsave(&rq->lock);

    thread->state = THREAD_READY;
    thread->cpu_index = cpu;
    ring_insert(rq, thread);

    spin_unlock_irqrestore(&rq->lock, flags);

    /* A peer parked in its halt loop must re-evaluate its ring; the local CPU
     * just takes the deferred-reschedule path. */
    if (cpu != this_cpu_index() && local != NULL &&
        __atomic_load_n(&local->online, __ATOMIC_ACQUIRE)) {
        lapic_send_ipi(local->lapic_id, IPI_RESCHEDULE_VECTOR);
    }

    sched_request_reschedule();
}

/* Hands the CPU to `next`. Only sound when `next` belongs to this CPU's ring:
 * the caller keeps control until the peer blocks or exits, which a thread owned
 * by another run queue cannot honour. A remote receiver is left READY and woken
 * with a reschedule IPI instead, so IPC across cores degrades to the normal
 * blocking path rather than corrupting two rings. */
void sched_handoff(thread_t *next)
{
    if (next == NULL) {
        return;
    }

    uint64_t flags = irq_save();
    thread_t *self = sched_current_thread();

    /* Priority inheritance: a caller parked waiting for next's reply lends
     * next its QoS until the reply (or the caller's teardown) restores it. */
    if (self != NULL && self->state == THREAD_BLOCKED_REPLY &&
        self->reply_target == next) {
        sched_qos_inherit(next, self);
    }

    if (next->cpu_index == this_cpu_index()) {
        switch_to(next);
        irq_restore(flags);
        return;
    }

    /* A peer on another CPU cannot be switched to from here. Signal it and let
     * this thread block; its own run queue will dispatch it. */
    cpu_local_t *peer = cpu_local_for(next->cpu_index);

    next->state = THREAD_READY;
    __atomic_store_n(&reschedule_pending[next->cpu_index], 1U,
                     __ATOMIC_RELEASE);

    if (peer != NULL && __atomic_load_n(&peer->online, __ATOMIC_ACQUIRE)) {
        lapic_send_ipi(peer->lapic_id, IPI_RESCHEDULE_VECTOR);
    }

    irq_restore(flags);
}

void sched_block_current(void)
{
    uint32_t cpu = this_cpu_index();
    cpu_runqueue_t *rq = queue_for(cpu);
    uint64_t flags = irq_save();

    uint64_t rq_flags = spin_lock_irqsave(&rq->lock);
    thread_t *current = sched_current_thread();
    thread_t *next = select_next_local(rq, current);
    spin_unlock_irqrestore(&rq->lock, rq_flags);

    if (next != NULL && next != current) {
        switch_to(next);
    }

    /* A thread may have exited since this one last ran; reclaim it here too so
     * reaping does not depend on a switch that already happened. */
    reap_publish_zombie(cpu);

    irq_restore(flags);
}

void sched_wake(thread_t *thread)
{
    if (thread == NULL) {
        return;
    }

    uint64_t flags = spin_lock_irqsave(&clock_lock);
    bool woken = false;

    switch (thread->state) {
    case THREAD_BLOCKED_SEND:
    case THREAD_BLOCKED_RECV:
    case THREAD_BLOCKED_REPLY:
    case THREAD_BLOCKED_NOTIFY:
    case THREAD_SLEEPING:
        thread->state = THREAD_READY;
        woken = true;
        break;
    default:
        break;
    }

    spin_unlock_irqrestore(&clock_lock, flags);

    if (!woken) {
        return;
    }

    /* On a multicore machine, nudge the CPU that owns the thread so it
     * re-evaluates even when parked in its halt loop. */
    if (smp_multicore_active() && thread->cpu_index != this_cpu_index()) {
        cpu_local_t *peer = cpu_local_for(thread->cpu_index);

        if (peer != NULL && __atomic_load_n(&peer->online, __ATOMIC_ACQUIRE)) {
            lapic_send_ipi(peer->lapic_id, IPI_RESCHEDULE_VECTOR);
        }
    }

    sched_request_reschedule();
}

void sched_request_reschedule(void)
{
    __atomic_store_n(&reschedule_pending[this_cpu_index()], 1U,
                     __ATOMIC_RELEASE);
}

void sched_detach_thread(thread_t *thread)
{
    if (thread == NULL || thread == sched_current_thread() ||
        thread->state == THREAD_DEAD || is_idle_thread(thread)) {
        return;
    }

    cpu_runqueue_t *rq = queue_for(thread->cpu_index);

    if (rq != NULL) {
        uint64_t flags = spin_lock_irqsave(&rq->lock);
        (void)ring_remove(rq, thread);
        spin_unlock_irqrestore(&rq->lock, flags);
    }

    thread->state = THREAD_DEAD;
    reap_enqueue(thread);
}

void sched_tick(void)
{
    uint32_t cpu = this_cpu_index();

    /* The clock is millisecond wall time, not "ticks delivered". Every CPU
     * runs a 1 kHz timer, so letting each one advance the counter would make
     * time run four times too fast on a 4-core machine and expire every
     * deadline early. The BSP alone owns the global clock; APs only read it to
     * wake their own sleepers. */
    uint64_t now;

    if (cpu == 0) {
        /* A single owner keeps the counter equal to elapsed milliseconds; the
         * atomic form lets APs read it without the clock lock. */
        now = __atomic_add_fetch(&sched_ticks, 1ULL, __ATOMIC_ACQ_REL);
    } else {
        now = __atomic_load_n(&sched_ticks, __ATOMIC_ACQUIRE);
    }

    reap_dead_threads();

    cpu_runqueue_t *rq = queue_for(cpu);
    uint64_t flags = spin_lock_irqsave(&rq->lock);

    if (rq->ring != NULL) {
        thread_t *start = rq->ring->sched_next;
        thread_t *cursor = start;

        do {
            thread_t *next = cursor->sched_next;

            if (cursor->state == THREAD_SLEEPING &&
                cursor->wakeup_tick <= now) {
                cursor->state = THREAD_READY;
            }
            cursor = next;
        } while (cursor != start);
    }

    thread_t *current = sched_current_thread();
    bool force = __atomic_exchange_n(&reschedule_pending[cpu], 0U,
                                     __ATOMIC_ACQ_REL) != 0;
    thread_t *next = NULL;

    bool needs_switch = force || current == NULL || is_idle_thread(current) ||
                        current->state != THREAD_RUNNING || (now & 1ULL) == 0;

    if (needs_switch) {
        next = select_next_preempt(rq, current);
    }

    spin_unlock_irqrestore(&rq->lock, flags);

    if (next != NULL && next != current) {
        switch_to(next);
    }
}

void sched_start(void)
{
    klog(KLOG_INFO, "scheduler: per-CPU QoS (interactive/default/background, "
         "aged), 1000 Hz preemption, FXSAVE64 FPU/SSE context switch");
    __asm__ volatile("sti" : : : "memory");
}

void thread_yield(void)
{
    uint32_t cpu = this_cpu_index();
    cpu_runqueue_t *rq = queue_for(cpu);
    uint64_t flags = irq_save();

    uint64_t rq_flags = spin_lock_irqsave(&rq->lock);
    thread_t *current = sched_current_thread();
    thread_t *next = ring_pick_next(rq, current);
    spin_unlock_irqrestore(&rq->lock, rq_flags);

    if (next != NULL && next != current) {
        switch_to(next);
    }

    irq_restore(flags);
}

void thread_sleep_ms(uint64_t ms)
{
    if (ms == 0) {
        thread_yield();
        return;
    }

    uint64_t now = __atomic_load_n(&sched_ticks, __ATOMIC_ACQUIRE);
    thread_t *self = sched_current_thread();

    self->wakeup_tick = now + ms;
    self->state = THREAD_SLEEPING;

    sched_block_current();
}

NORETURN void task_terminate(void)
{
    uint64_t flags = irq_save();
    thread_t *current = sched_current_thread();
    task_t *task = (current != NULL) ? current->owner_task : NULL;

    if (task != NULL) {
        task->terminating = true;

        uint64_t list_flags = spin_lock_irqsave(&task->thread_lock);
        thread_t **link = &task->threads;
        thread_t *victims = NULL;

        while (*link != NULL) {
            thread_t *thread = *link;

            if (thread == current) {
                link = &thread->task_next;
                continue;
            }

            *link = thread->task_next;
            thread->task_next = victims;
            victims = thread;

            if (task->thread_count > 0) {
                task->thread_count--;
            }
        }
        spin_unlock_irqrestore(&task->thread_lock, list_flags);

        /* Detaching takes scheduler and IPC locks, so it happens with the task
         * lock released. */
        while (victims != NULL) {
            thread_t *thread = victims;

            victims = thread->task_next;
            thread->task_next = NULL;

            ipc_thread_detach(thread);
            sched_detach_thread(thread);
        }
    }

    if (current != NULL) {
        current->exit_code = -14; /* user memory fault */
    }

    thread_exit();
    irq_restore(flags);
    __builtin_unreachable();
}

NORETURN void thread_exit(void)
{
    uint64_t flags = irq_save();

    thread_t *self = sched_current_thread();
    uint32_t cpu = this_cpu_index();
    cpu_runqueue_t *rq = queue_for(cpu);

    ipc_thread_detach(self);

    /* Unlink before selecting so this thread cannot be chosen as its own
     * successor, then pick the next runnable context for this CPU. */
    uint64_t rq_flags = spin_lock_irqsave(&rq->lock);
    (void)ring_remove(rq, self);
    thread_t *next = select_next_local(rq, self);
    spin_unlock_irqrestore(&rq->lock, rq_flags);

    task_t *owner = self->owner_task;
    if (owner != NULL) {
        /* The list is shared with task_terminate(), which can run on another
         * CPU, so it is unlinked under the task's own lock. */
        uint64_t list_flags = spin_lock_irqsave(&owner->thread_lock);
        thread_t **link = &owner->threads;
        while (*link != NULL && *link != self) {
            link = &(*link)->task_next;
        }
        if (*link == self) {
            *link = self->task_next;
            if (owner->thread_count > 0) {
                owner->thread_count--;
            }
        }
        self->task_next = NULL;
        uint32_t remaining = owner->thread_count;
        spin_unlock_irqrestore(&owner->thread_lock, list_flags);

        if (remaining == 0) {
            const mk_hooks_t *hooks = mk_hooks_get();
            if (hooks != NULL && hooks->on_task_exit != NULL) {
                hooks->on_task_exit(owner, self->exit_code);
            }
        }
    }

    self->state = THREAD_DEAD;

    /* Still executing on this thread's own stack: link it onto the CPU's
     * zombie list so a later context switch reclaims it only after control has
     * moved off this stack. */
    self->sched_next = zombie_thread[cpu];
    zombie_thread[cpu] = self;

    if (next != NULL && next != self) {
        switch_to(next);
    }

    /* Unreachable in practice: a dead thread is never resumed. Park the CPU
     * safely if a future change ever does switch back here. */
    irq_restore(flags);
    cpu_halt();
    __builtin_unreachable();
}

uint64_t sched_tick_count(void)
{
    return __atomic_load_n(&sched_ticks, __ATOMIC_ACQUIRE);
}

uint64_t sched_reaped_count(void)
{
    return __atomic_load_n(&reaped_count, __ATOMIC_ACQUIRE);
}

bool sched_reap_pending(void)
{
    uint64_t flags = spin_lock_irqsave(&reap_lock);
    bool pending = reap_list != NULL;
    spin_unlock_irqrestore(&reap_lock, flags);

    return pending;
}

task_t *sched_boot_task(void)
{
    return &boot_task;
}

uint64_t sched_boot_stack_top(void)
{
    return boot_thread.kstack_top;
}

uint64_t sched_cpu_dispatch_count(uint32_t cpu_index)
{
    if (cpu_index >= MAX_CPUS) {
        return 0;
    }

    uint64_t flags = spin_lock_irqsave(&runqueues[cpu_index].lock);
    uint64_t count = runqueues[cpu_index].dispatch;
    spin_unlock_irqrestore(&runqueues[cpu_index].lock, flags);

    return count;
}
