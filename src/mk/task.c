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

/* Every task created through task_create() is linked here so a capability
 * can be granted to a task named by its id (SYS_CAP_GRANT). Lock order:
 * task registry -> cap. */
static task_t *task_registry;
static spinlock_t task_registry_lock;

_Static_assert(THREAD_FXSAVE_SIZE == 512U, "FXSAVE64 image is 512 bytes");

static void fxsave_image_default(uint8_t *image)
{
    memset(image, 0, THREAD_FXSAVE_SIZE);

    /* Legacy FXSAVE layout: FCW at byte 0 (FSW, abridged FTW and FOP stay
     * zero: empty x87 stack, no pending exceptions), MXCSR at byte 24. XMM0-15
     * at bytes 160..415 are already zero. */
    uint16_t fcw = (uint16_t)THREAD_FPU_DEFAULT_FCW;
    uint32_t mxcsr = (uint32_t)THREAD_FPU_DEFAULT_MXCSR;

    memcpy(image + 0, &fcw, sizeof(fcw));
    memcpy(image + 24, &mxcsr, sizeof(mxcsr));
}

static uint8_t *fxsave_aligned(thread_t *thread)
{
    uintptr_t raw = (uintptr_t)thread->fxsave_storage;

    return (uint8_t *)(uintptr_t)ALIGN_UP(raw, THREAD_FXSAVE_ALIGN);
}

void thread_fpu_init(thread_t *thread)
{
    if (thread == NULL) {
        return;
    }

    thread->fxsave_area = fxsave_aligned(thread);
    if (((uintptr_t)thread->fxsave_area & (THREAD_FXSAVE_ALIGN - 1U)) != 0) {
        kpanic("thread: FXSAVE area %p is not 16-byte aligned",
               thread->fxsave_area);
    }
    fxsave_image_default(thread->fxsave_area);
}

/* Captures the live FPU/SSE registers of the calling CPU into image. The
 * kernel is built without SSE code generation, so whatever the registers hold
 * here still belongs to the thread that entered the kernel. */
static void fxsave_live(uint8_t *image)
{
    __asm__ volatile("fxsave64 %0"
                     : "=m"(*(uint8_t(*)[THREAD_FXSAVE_SIZE])image)
                     :
                     : "memory");
}

static void fxrstor_live(const uint8_t *image)
{
    __asm__ volatile("fxrstor64 %0"
                     :
                     : "m"(*(const uint8_t(*)[THREAD_FXSAVE_SIZE])image)
                     : "memory");
}

void thread_fpu_reset_current(void)
{
    uint64_t flags = irq_save();
    thread_t *self = sched_current_thread();

    if (self != NULL && self->fxsave_area != NULL) {
        /* Interrupts stay off between the two steps: a context switch in the
         * middle would save the stale live registers over the clean image. */
        fxsave_image_default(self->fxsave_area);
        fxrstor_live(self->fxsave_area);
    }

    irq_restore(flags);
}

sys_status_t thread_set_qos(thread_t *thread, uint32_t qos)
{
    if (thread == NULL || qos >= THREAD_QOS_COUNT) {
        return SYS_ERR_INVAL;
    }

    sched_qos_set_base(thread, (uint8_t)qos);
    return SYS_OK;
}

thread_t *task_find_thread(task_t *task, uint64_t tid)
{
    thread_t *self = sched_current_thread();

    if (task == NULL) {
        return NULL;
    }
    if (tid == 0) {
        return (self != NULL && self->owner_task == task) ? self : NULL;
    }

    thread_t *found = NULL;
    uint64_t flags = spin_lock_irqsave(&task->thread_lock);

    for (thread_t *thread = task->threads; thread != NULL;
         thread = thread->task_next) {
        if (thread->tid == tid && thread->state != THREAD_DEAD) {
            found = thread;
            break;
        }
    }

    spin_unlock_irqrestore(&task->thread_lock, flags);
    return found;
}

sys_status_t task_with_id(uint64_t task_id,
                          sys_status_t (*fn)(task_t *target, void *ctx),
                          void *ctx)
{
    if (fn == NULL || task_id == 0) {
        return SYS_ERR_INVAL;
    }

    uint64_t flags = spin_lock_irqsave(&task_registry_lock);
    task_t *target = task_registry;

    while (target != NULL && target->id != task_id) {
        target = target->registry_next;
    }

    sys_status_t status;

    if (target == NULL) {
        status = SYS_ERR_INVAL;
    } else if (target->terminating || target->caps == NULL) {
        status = SYS_ERR_DEAD;
    } else {
        status = fn(target, ctx);
    }

    spin_unlock_irqrestore(&task_registry_lock, flags);
    return status;
}

static void task_registry_insert(task_t *task)
{
    uint64_t flags = spin_lock_irqsave(&task_registry_lock);

    task->registry_next = task_registry;
    task_registry = task;

    spin_unlock_irqrestore(&task_registry_lock, flags);
}

static void task_registry_remove(task_t *task)
{
    uint64_t flags = spin_lock_irqsave(&task_registry_lock);
    task_t **link = &task_registry;

    while (*link != NULL && *link != task) {
        link = &(*link)->registry_next;
    }
    if (*link == task) {
        *link = task->registry_next;
    }
    task->registry_next = NULL;

    spin_unlock_irqrestore(&task_registry_lock, flags);
}

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

    task_registry_insert(task);
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
    irq_restore(flags);

    /* Unpublish first: once the registry no longer names this task, no
     * cross-task grant can be inserting into its capability table. */
    task_registry_remove(task);

    flags = irq_save();
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
                                        uint64_t fs_base, bool inherit_fpu)
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
    thread->base_qos = (uint8_t)THREAD_QOS_DEFAULT;
    thread->effective_qos = (uint8_t)THREAD_QOS_DEFAULT;
    thread->inherited_qos = (uint8_t)THREAD_QOS_NONE;
    thread->qos_age = 0;

    thread_fpu_init(thread);
    if (inherit_fpu) {
        thread_t *creator = sched_current_thread();

        /* Inherit the creator's live register state, and its QoS class, so
         * the duplicate resumes with exactly the context it was cloned from. */
        fxsave_live(thread->fxsave_area);
        if (creator != NULL) {
            thread->base_qos = creator->base_qos;
            thread->effective_qos = creator->base_qos;
        }
    }

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
    return thread_create_internal(task, entry, arg, 0, false);
}

thread_t *thread_create_with_fs_base(task_t *task,
                                      void (*entry)(void *arg), void *arg,
                                      uint64_t fs_base)
{
    return thread_create_internal(task, entry, arg, fs_base, false);
}

thread_t *thread_create_inheriting(task_t *task, void (*entry)(void *arg),
                                   void *arg, uint64_t fs_base)
{
    return thread_create_internal(task, entry, arg, fs_base, true);
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
