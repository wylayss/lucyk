#ifndef LUCYK_BSD_PROC_H
#define LUCYK_BSD_PROC_H

#include <bsd/bsd.h>
#include <bsd/vfs.h>
#include <mk_hooks.h>
#include <task.h>
#include <types.h>

/* Signal numbers follow the BSD convention. */
#define BSD_SIGHUP   1
#define BSD_SIGINT   2
#define BSD_SIGQUIT  3
#define BSD_SIGILL   4
#define BSD_SIGTRAP  5
#define BSD_SIGABRT  6
#define BSD_SIGFPE   8
#define BSD_SIGKILL  9
#define BSD_SIGBUS   10
#define BSD_SIGSEGV  11
#define BSD_SIGPIPE  13
#define BSD_SIGTERM  15
#define BSD_SIGSTOP  17
#define BSD_SIGCHLD  20
#define BSD_SIGURG   23
#define BSD_SIGUSR1  30
#define BSD_SIGUSR2  31

#define BSD_SIGBIT(signo) (1ULL << ((signo) - 1U))

/* Signals that cannot be caught, blocked, or ignored. */
#define BSD_SIG_UNMASKABLE (BSD_SIGBIT(BSD_SIGKILL) | BSD_SIGBIT(BSD_SIGSTOP))

/* Special handler values. */
#define BSD_SIG_DFL  ((uint64_t)0)
#define BSD_SIG_IGN  ((uint64_t)1)

/* sa_flags bits. */
#define BSD_SA_RESTORER 0x04000000U

/* Per-signal action registered via BSD_SYS_SIGACTION. */
typedef struct bsd_sigaction {
    uint64_t sa_handler;  /* BSD_SIG_DFL, BSD_SIG_IGN, or Ring 3 handler addr */
    uint64_t sa_mask;     /* Additional signals to block during handler */
    uint32_t sa_flags;
    uint32_t _pad;
    uint64_t sa_restorer; /* Ring 3 trampoline that calls BSD_SYS_SIGRETURN */
} bsd_sigaction_t;

/* Signal frame pushed onto the user stack before redirecting to a handler.
 * Layout is 16-byte aligned so the handler enters with a valid ABI stack. */
typedef struct bsd_sigframe {
    uint64_t restorer;       /* Return address: sa_restorer trampoline */
    uint32_t signo;          /* Signal number passed in RDI */
    uint32_t _pad0;
    uint64_t saved_mask;     /* Previous signal_mask to restore on sigreturn */
    /* Saved user GPRs */
    uint64_t rax;
    uint64_t rdi;
    uint64_t rsi;
    uint64_t rdx;
    uint64_t r10;
    uint64_t r8;
    uint64_t r9;
    uint64_t rcx;            /* User RIP saved by SYSCALL */
    uint64_t r11;            /* User RFLAGS saved by SYSCALL */
    uint64_t rsp;            /* User RSP */
    uint64_t r15;
    uint64_t r14;
    uint64_t r13;
    uint64_t r12;
    uint64_t rbx;
    uint64_t rbp;
    uint64_t _pad1;          /* Pad to 16-byte alignment */
} bsd_sigframe_t;

_Static_assert(sizeof(bsd_sigframe_t) % 16 == 0,
               "bsd_sigframe_t must be 16-byte aligned");

/* sigprocmask `how` argument. */
#define BSD_SIG_BLOCK   1
#define BSD_SIG_UNBLOCK 2
#define BSD_SIG_SETMASK 3

/* arch_prctl sub-commands. */
#define BSD_ARCH_SET_FS 0x1002
#define BSD_ARCH_GET_FS 0x1003

typedef enum bsd_proc_state {
    PROC_UNUSED = 0,
    PROC_RUNNING,
    PROC_ZOMBIE
} bsd_proc_state_t;

/* Anonymous mmap allocations tracked per process. length == 0 means free. */
typedef struct bsd_mmap_region {
    vaddr_t base;
    size_t  length;
} bsd_mmap_region_t;

typedef struct bsd_proc {
    int32_t           pid;
    int32_t           ppid;
    int32_t           pgid;
    uint32_t          uid;
    uint32_t          gid;
    bsd_proc_state_t  state;
    int32_t           exit_status;   /* (status << 8) or 128 + signo */
    uint64_t          pending_signals;
    uint64_t          signal_mask;
    bsd_sigaction_t   actions[BSD_NSIG]; /* Per-signal dispositions */
    task_t           *mk_task;
    vnode_t          *cwd;
    file_t           *fd_table[BSD_MAX_FDS];
    vaddr_t           brk_base;
    vaddr_t           brk_current;
    bsd_mmap_region_t mmap_regions[BSD_MAX_MMAPS];
    vaddr_t           mmap_next;
    struct bsd_proc  *parent;
    struct bsd_proc  *first_child;
    struct bsd_proc  *next_sibling;
    thread_t         *waiter_thread;
    thread_t         *signal_waiter;  /* Thread parked in a pipe wait */
    size_t            fork_frames;    /* Free-frame count at fork() */
    uint64_t          fork_objects;   /* mk_object count at fork() */
    bool              fork_leak_check;
    char              comm[BSD_COMM_LEN];
} bsd_proc_t;

void bsd_proc_init(void);

/* Creates a process for task. pid == 0 allocates the next free PID (PID 1 is
 * reserved for /bin/init and is never handed out by the allocator). */
bsd_proc_t *bsd_proc_create(task_t *task, int32_t pid, int32_t ppid,
                            const char *comm);
bsd_proc_t *bsd_proc_from_task(task_t *task);
bsd_proc_t *bsd_proc_lookup(int32_t pid);
bsd_proc_t *bsd_proc_current(void);

/* Advisory scan for the next free PID; it does not reserve the slot.
 * bsd_proc_create(task, 0, ...) allocates and claims a PID atomically. */
int32_t bsd_pid_alloc(void);
void bsd_pid_release(int32_t pid);

/* Marks the process a zombie, records the wait4-encoded status, reparents its
 * children to PID 1, closes descriptors and drops the cwd reference. */
sys_status_t bsd_proc_exit(bsd_proc_t *proc, int32_t status);

/* Signal-caused termination; exit_status becomes 128 + signo. */
void bsd_proc_signal_exit(bsd_proc_t *proc, uint32_t signo);

/* Releases a zombie's table slot and unlinks it from its parent. */
void bsd_proc_reap(bsd_proc_t *proc);

sys_status_t bsd_signal_send(int32_t pid, uint32_t signo);

/* Sends a signal to every PROC_RUNNING process whose pgid matches. */
sys_status_t bsd_signal_send_group(int32_t pgid, uint32_t signo);

/* BSD_SYS_KILL entry point: signo 0 probes existence, signals are queued as
 * pending and delivered on the target's next syscall return.
 * pid == 0 -> caller's process group, pid < -1 -> group abs(pid). */
sys_status_t bsd_proc_kill(bsd_proc_t *caller, int32_t pid, uint32_t signo);

/* BSD_SYS_SIGACTION: installs a signal disposition and returns the old one. */
sys_status_t bsd_proc_sigaction(bsd_proc_t *proc, uint32_t signo,
                                uint64_t unewact, uint64_t uoldact);

/* BSD_SYS_SIGPROCMASK: manipulates the blocked signal mask. */
sys_status_t bsd_proc_sigprocmask(bsd_proc_t *proc, int32_t how,
                                  uint64_t uset, uint64_t uoldset);

/* BSD_SYS_SIGRETURN: restores the saved context from the user sigframe. */
sys_status_t bsd_proc_sigreturn(bsd_proc_t *proc, syscall_frame_t *frame);

/* BSD_SYS_SETPGID / BSD_SYS_GETPGID. */
sys_status_t bsd_proc_setpgid(bsd_proc_t *caller, int32_t pid, int32_t pgid);
int32_t bsd_proc_getpgid(int32_t pid);

/* BSD_SYS_ARCH_PRCTL: ARCH_SET_FS / ARCH_GET_FS for TLS. */
sys_status_t bsd_proc_arch_prctl(int32_t code, uint64_t addr,
                                 uint64_t *result);

/* BSD Phase 3 process creation, program execution and child reaping. */
int32_t bsd_proc_fork(syscall_frame_t *parent_frame);
sys_status_t bsd_proc_execve(syscall_frame_t *frame, const char *upath,
                             char *const uargv[], char *const uenvp[]);
int32_t bsd_proc_wait4(int32_t target_pid, uint64_t uwstatus, int32_t options);

/* True when an unblocked, non-ignored signal is pending. */
bool bsd_proc_has_pending(bsd_proc_t *proc);

/* Delivers the lowest pending unblocked signal. If a custom handler is
 * installed, builds a sigframe on the user stack and redirects the frame to
 * the handler. Fatal default signals terminate the calling task. */
void bsd_proc_deliver_signals(bsd_proc_t *proc, syscall_frame_t *frame);

/* Introspection snapshot used by /proc. Every field is copied under the
 * process-table lock; vspace (when requested) carries one reference that the
 * caller drops with vspace_destroy(). */
typedef struct bsd_proc_info {
    int32_t           pid;
    int32_t           ppid;
    int32_t           pgid;
    uint32_t          uid;
    uint32_t          gid;
    bsd_proc_state_t  state;
    uint32_t          threads;
    uint64_t          task_id;
    uint8_t           base_qos;      /* Of the task's first thread */
    uint8_t           effective_qos;
    uint64_t          pending_signals;
    uint64_t          signal_mask;
    vaddr_t           brk_base;
    vaddr_t           brk_current;
    bsd_mmap_region_t mmap_regions[BSD_MAX_MMAPS];
    char              comm[BSD_COMM_LEN];
    vspace_t         *vspace;
} bsd_proc_info_t;

/* Fills *out for a live (running or zombie) pid. want_vspace retains the
 * address space of a running process. */
bool bsd_proc_snapshot(int32_t pid, bool want_vspace, bsd_proc_info_t *out);

/* Smallest live pid greater than `after`, or 0 when there is none. */
int32_t bsd_proc_next_pid(int32_t after);

/* Installs the kernel's boot capabilities (framebuffer and display-info
 * frames) into a freshly created PID 1, before its first thread runs. */
sys_status_t bsd_proc_install_boot_caps(bsd_proc_t *proc);

/* mk_hooks_t callbacks for mk_register_hooks(). */
const mk_hooks_t *bsd_proc_get_hooks(void);

#endif /* LUCYK_BSD_PROC_H */
