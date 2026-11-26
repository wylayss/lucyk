#include <io.h>
#include <kalloc.h>
#include <klog.h>
#include <sched.h>
#include <string.h>
#include <task.h>
#include <types.h>
#include <cpu.h>
#include <vspace.h>

_Static_assert(sizeof(cpu_context_t) == 7 * sizeof(uint64_t),
               "cpu_context_t must match the switch.asm frame");

extern void thread_entry_start(void);

static uint64_t next_task_id = 1;
static uint64_t next_thread_id = 1;

task_t *task_create(void)
{
    task_t *task = (task_t *)kalloc(sizeof(task_t));
    if (task == NULL) {
        return NULL;
    }
    memset(task, 0, sizeof(*task));

    task->vspace = vspace_create();
    if (task->vspace == NULL) {
        kfree(task);
        return NULL;
    }

    task->caps = cap_table_create();
    if (task->caps == NULL) {
        vspace_destroy(task->vspace);
        kfree(task);
        return NULL;
    }

    uint64_t flags = irq_save();
    task->id = next_task_id++;

    /* Threads stay with the creating core by default. The microkernel's
     * per-task state and the IPC rendezvous handoff both assume a single run
     * queue; spreading is opt-in through task_set_affinity(). */
    task->affinity_cpu = this_cpu_index();
    spinlock_init(&task->thread_lock);
    irq_restore(flags);

    return task;
}

void task_set_affinity(task_t *task, uint32_t cpu)
{
    if (task == NULL) {
        return;
    }

    if (cpu != TASK_AFFINITY_ANY && cpu >= MAX_CPUS) {
        return;
    }

    uint64_t flags = irq_save();
    task->affinity_cpu = cpu;
    irq_restore(flags);
}

void task_destroy(task_t *task)
{
    if (task == NULL || task == sched_boot_task()) {
        return;
    }

    uint64_t flags = irq_save();

    if (task->thread_count != 0 || task->threads != NULL) {
        irq_restore(flags);
        kpanic("task_destroy: task %lu still owns threads", task->id);
    }

    vspace_t *vspace = task->vspace;
    cap_table_t *caps = task->caps;

    task->vspace = NULL;
    task->caps = NULL;
    irq_restore(flags);

    if (caps != NULL) {
        cap_table_destroy(caps);
    }
    if (vspace != NULL) {
        vspace_destroy(vspace);
    }
    kfree(task);
}

static thread_t *thread_create_internal(task_t *task,
                                        void (*entry)(void *arg), void *arg,
                                        uint64_t fs_base)
{
    if (task == NULL || entry == NULL) {
        return NULL;
    }

    thread_t *thread = (thread_t *)kalloc(sizeof(thread_t));
    if (thread == NULL) {
        return NULL;
    }
    memset(thread, 0, sizeof(*thread));

    /* The lowest page of the slot is an unmapped guard page; the upper four
     * pages form a 16 KiB RW/NX Ring 0 stack. */
    vaddr_t stack = kstack_alloc();
    if (stack == 0) {
        kfree(thread);
        return NULL;
    }

    thread->owner_task = task;
    thread->kstack_base = (uint8_t *)(uintptr_t)stack;
    thread->kstack_top = stack + KSTACK_SLOT_SIZE;
    thread->state = THREAD_READY;
    thread->entry = entry;
    thread->arg = arg;
    thread->fs_base = fs_base;

    cpu_context_t *ctx =
        (cpu_context_t *)(uintptr_t)(thread->kstack_top - sizeof(cpu_context_t));
    memset(ctx, 0, sizeof(*ctx));
    ctx->r12 = (uint64_t)(uintptr_t)thread;
    ctx->rip = (uint64_t)(uintptr_t)thread_entry_start;
    thread->kstack_ctx = ctx;

    uint64_t flags = irq_save();
    thread->tid = next_thread_id++;

    /* The thread list can be mutated from another CPU (task_terminate runs
     * wherever the killer is executing), so it has its own spinlock rather than
     * relying on the local interrupt mask. */
    uint64_t list_flags = spin_lock_irqsave(&task->thread_lock);
    thread->task_next = task->threads;
    task->threads = thread;
    task->thread_count++;
    spin_unlock_irqrestore(&task->thread_lock, list_flags);

    sched_add(thread);
    irq_restore(flags);

    return thread;
}

thread_t *thread_create(task_t *task, void (*entry)(void *arg), void *arg)
{
    return thread_create_internal(task, entry, arg, 0);
}

thread_t *thread_create_with_fs_base(task_t *task,
                                      void (*entry)(void *arg), void *arg,
                                      uint64_t fs_base)
{
    return thread_create_internal(task, entry, arg, fs_base);
}

NORETURN void thread_entry_c(thread_t *thread)
{
    __asm__ volatile("sti" : : : "memory");

    if (thread->entry != NULL) {
        thread->entry(thread->arg);
    }

    thread_exit();
    __builtin_unreachable();
}
