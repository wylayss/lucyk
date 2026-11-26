#ifndef LUCYK_TASK_H
#define LUCYK_TASK_H

#include <cap.h>
#include <spinlock.h>
#include <types.h>
#include <vspace.h>

typedef enum thread_state {
    THREAD_READY = 0,
    THREAD_RUNNING,
    THREAD_BLOCKED_SEND,
    THREAD_BLOCKED_RECV,
    THREAD_BLOCKED_REPLY,
    THREAD_BLOCKED_NOTIFY,
    THREAD_SLEEPING,
    THREAD_DEAD
} thread_state_t;

/* Callee-saved register frame as laid out by switch_context. */
typedef struct cpu_context {
    uint64_t r15;
    uint64_t r14;
    uint64_t r13;
    uint64_t r12;
    uint64_t rbx;
    uint64_t rbp;
    uint64_t rip;
} cpu_context_t;

/* IPC wait reason, used to unlink a dying thread from wait queues. */
#define IPC_WAIT_NONE   0U
#define IPC_WAIT_SEND   1U
#define IPC_WAIT_RECV   2U
#define IPC_WAIT_REPLY  3U
#define IPC_WAIT_NOTIFY 4U

/* Quality-of-service classes. A higher value is dispatched first; the
 * scheduler ages lower classes so they keep making bounded forward progress
 * (see sched.c). THREAD_QOS_NONE marks "no inherited boost". */
#define THREAD_QOS_BACKGROUND  0U
#define THREAD_QOS_DEFAULT     1U
#define THREAD_QOS_INTERACTIVE 2U
#define THREAD_QOS_COUNT       3U
#define THREAD_QOS_NONE        0xFFU

/* x87/SSE register image saved by FXSAVE64 and restored by FXRSTOR64. Both
 * instructions fault (#GP) on an address that is not 16-byte aligned, so each
 * thread carries a little slack and fxsave_area points at the aligned part. */
#define THREAD_FXSAVE_SIZE  512U
#define THREAD_FXSAVE_ALIGN 16U

/* Power-on defaults loaded into every new FPU/SSE image. */
#define THREAD_FPU_DEFAULT_FCW   0x037FU
#define THREAD_FPU_DEFAULT_MXCSR 0x1F80U

struct task;

typedef struct thread {
    uint64_t        tid;
    struct task    *owner_task;
    cpu_context_t  *kstack_ctx;
    uint8_t        *kstack_base;
    uint64_t        kstack_top;
    thread_state_t  state;
    uint64_t        wakeup_tick;
    uint64_t        ipc_msg_regs[4];
    uint64_t        ipc_badge;
    struct thread  *reply_target;
    struct thread  *ipc_sender;
    sys_status_t    ipc_status;
    int64_t         exit_code;
    void           *ipc_wait_object;
    uint32_t        ipc_wait_kind;
    uint32_t        cpu_index;      /* CPU whose run queue owns this thread */
    uint64_t        fs_base;        /* IA32_FS_BASE for user-space TLS */
    void          (*entry)(void *arg);
    void           *arg;
    struct thread  *sched_next;
    struct thread  *wait_next;
    struct thread  *task_next;
    uint8_t         base_qos;       /* QoS class requested for the thread */
    uint8_t         effective_qos;  /* max(base_qos, inherited_qos) */
    uint8_t         inherited_qos;  /* IPC priority-inheritance boost */
    uint32_t        qos_age;        /* Dispatches lost to higher classes */
    uint8_t        *fxsave_area;    /* 16-byte aligned view into storage */
    uint8_t         fxsave_storage[THREAD_FXSAVE_SIZE + THREAD_FXSAVE_ALIGN];
} thread_t;

/* Task CPU placement. A task whose threads exchange messages must live on one
 * CPU: the rendezvous fast path hands a receiver control directly
 * (sched_handoff), which is only sound when both threads belong to the same
 * run queue. Tasks therefore default to the CPU that created them, and only a
 * task explicitly marked TASK_AFFINITY_ANY is placed on any online core. */
#define TASK_AFFINITY_ANY 0xFFFFFFFFU

typedef struct task {
    uint64_t      id;
    vspace_t     *vspace;
    cap_table_t  *caps;
    thread_t     *threads;
    uint32_t      thread_count;
    uint32_t      affinity_cpu; /* CPU owning the task's threads, or ANY */
    spinlock_t    thread_lock;  /* Guards threads[] and thread_count */
    bool          terminating;
    void         *personality_data;
    struct task  *registry_next; /* Global task registry link (task.c) */
} task_t;

task_t *task_create(void);

/* Overrides the placement of threads created afterwards for this task. */
void task_set_affinity(task_t *task, uint32_t cpu);

/* Destroys a task that owns no threads. The caller must not be executing in
 * task->vspace (the boot task is rejected). */
void task_destroy(task_t *task);

thread_t *thread_create(task_t *task, void (*entry)(void *arg), void *arg);

/* Creates a thread whose TLS base is visible before it can be scheduled on
 * any CPU. */
thread_t *thread_create_with_fs_base(task_t *task,
                                      void (*entry)(void *arg), void *arg,
                                      uint64_t fs_base);

/* Creates a thread that starts with a copy of the calling thread's live
 * FPU/SSE register state and the given TLS base, both published before the
 * thread can be scheduled on any CPU. Used when a personality duplicates the
 * calling context into a new task. */
thread_t *thread_create_inheriting(task_t *task, void (*entry)(void *arg),
                                   void *arg, uint64_t fs_base);

/* Points thread->fxsave_area at the 16-byte aligned part of its storage and
 * loads the clean default image (FCW 0x037F, MXCSR 0x1F80, XMM0-15 zeroed).
 * Every thread_t, including static and per-CPU idle contexts, goes through
 * this before its first context switch. */
void thread_fpu_init(thread_t *thread);

/* Replaces the calling thread's FPU/SSE state, both the saved image and the
 * live registers, with the clean default. Used when a personality loads a new
 * program image into the calling task. */
void thread_fpu_reset_current(void);

/* Sets the base QoS class of `thread` (THREAD_QOS_*) and recomputes its
 * effective class. Returns SYS_ERR_INVAL for an unknown class. */
sys_status_t thread_set_qos(thread_t *thread, uint32_t qos);

/* Finds a live thread of `task` by thread id; tid 0 names the caller. */
thread_t *task_find_thread(task_t *task, uint64_t tid);

/* Runs fn(target, ctx) for the live task whose id is `task_id` with the task
 * registry locked, so the target cannot be destroyed while fn runs. Returns
 * SYS_ERR_INVAL when no such task exists and SYS_ERR_DEAD when it is being
 * torn down; otherwise fn's status. fn must not block. */
sys_status_t task_with_id(uint64_t task_id,
                          sys_status_t (*fn)(task_t *target, void *ctx),
                          void *ctx);

NORETURN void thread_exit(void);
void thread_yield(void);
void thread_sleep_ms(uint64_t ms);

/* Internal trampoline target; called from switch.asm for a new thread. */
NORETURN void thread_entry_c(thread_t *thread);

#endif /* LUCYK_TASK_H */
