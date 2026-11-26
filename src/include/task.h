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

NORETURN void thread_exit(void);
void thread_yield(void);
void thread_sleep_ms(uint64_t ms);

/* Internal trampoline target; called from switch.asm for a new thread. */
NORETURN void thread_entry_c(thread_t *thread);

#endif /* LUCYK_TASK_H */
