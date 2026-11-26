#include <bsd/bsd.h>
#include <bsd/errno.h>
#include <bsd/proc.h>
#include <bsd/syscall_bsd.h>
#include <bsd/vfs.h>
#include <cap.h>
#include <elf.h>
#include <io.h>
#include <kalloc.h>
#include <klog.h>
#include <object.h>
#include <pmm.h>
#include <sched.h>
#include <string.h>
#include <syscall.h>
#include <task.h>
#include <types.h>
#include <vspace.h>

static bsd_proc_t proc_table[BSD_MAX_PROCS];

/* Caller must hold the process-table lock. */
static bsd_proc_t *lookup_locked(int32_t pid)
{
    if (pid <= 0 || pid >= BSD_MAX_PROCS) {
        return NULL;
    }
    if (proc_table[pid].state == PROC_UNUSED) {
        return NULL;
    }
    return &proc_table[pid];
}

/* Caller must hold the process-table lock. PID 1 is reserved. */
static int32_t alloc_pid_locked(void)
{
    for (int32_t pid = 2; pid < BSD_MAX_PROCS; pid++) {
        if (proc_table[pid].state == PROC_UNUSED) {
            return pid;
        }
    }
    return 0;
}

/* Caller must hold the process-table lock. SIGKILL and SIGSTOP cannot be
 * blocked. SIG_IGN signals (except SIGKILL/SIGSTOP) are silently dropped.
 * SIGCHLD's default action is ignored. */
static uint64_t deliverable_signals(const bsd_proc_t *proc)
{
    uint64_t pending = proc->pending_signals;
    uint64_t blocked = proc->signal_mask;

    /* SIGKILL and SIGSTOP are always deliverable. */
    uint64_t unblocked = (pending & ~blocked) |
                         (pending & BSD_SIG_UNMASKABLE);

    /* Drop signals whose disposition is SIG_IGN (they will be cleared by the
     * delivery path). SIGKILL and SIGSTOP cannot be ignored. */
    uint64_t deliverable = 0;
    for (uint32_t s = 1; s < BSD_NSIG; s++) {
        if ((unblocked & BSD_SIGBIT(s)) == 0) {
            continue;
        }
        uint64_t handler = proc->actions[s].sa_handler;
        if (handler == BSD_SIG_IGN && s != BSD_SIGKILL && s != BSD_SIGSTOP) {
            continue;
        }
        /* SIG_DFL with default-ignore action (SIGCHLD, SIGURG). */
        if (handler == BSD_SIG_DFL &&
            (s == BSD_SIGCHLD || s == BSD_SIGURG)) {
            continue;
        }
        deliverable |= BSD_SIGBIT(s);
    }

    return deliverable;
}

/* Caller must hold the process-table lock. Returns true if at least one child
 * was reparented to PID 1, so the caller can wake a sleeping PID 1. */
static bool reparent_children_locked(bsd_proc_t *proc)
{
    bsd_proc_t *child = proc->first_child;
    bsd_proc_t *init = NULL;
    bool adopted = false;

    proc->first_child = NULL;

    if (child == NULL) {
        return false;
    }

    if (proc->pid != 1) {
        init = lookup_locked(1);
    }

    while (child != NULL) {
        bsd_proc_t *next = child->next_sibling;

        child->next_sibling = NULL;

        if (init != NULL && init != proc) {
            child->ppid = 1;
            child->parent = init;
            child->next_sibling = init->first_child;
            init->first_child = child;
            /* Its fork baseline predates this parent's resource release. */
            child->fork_leak_check = false;
            adopted = true;
        } else {
            child->ppid = 0;
            child->parent = NULL;
        }

        child = next;
    }

    return adopted;
}

/* Caller holds no locks. The parent is notified only after the last thread
 * exits (bd_on_task_exit), never while the task still owns a kernel stack. */
static bool proc_enter_zombie(bsd_proc_t *proc, int32_t encoded_status,
                              bool *adopted_out)
{
    uint64_t flags = irq_save();

    if (proc->state != PROC_RUNNING) {
        irq_restore(flags);
        return false;
    }

    proc->state = PROC_ZOMBIE;
    proc->exit_status = encoded_status;
    *adopted_out = reparent_children_locked(proc);

    /* Descendants still own frames beyond this process's fork baseline. */
    if (*adopted_out) {
        proc->fork_leak_check = false;
    }

    irq_restore(flags);
    return true;
}

static void proc_release_resources(bsd_proc_t *proc)
{
    bsd_fd_close_all(proc);

    if (proc->cwd != NULL) {
        vnode_t *cwd = proc->cwd;
        proc->cwd = NULL;
        vfs_vnode_unref(cwd);
    }
}

/* Unlink any thread parked in a pipe queue before descriptors (and therefore
 * the pipes themselves) are torn down. */
static void proc_teardown(bsd_proc_t *proc)
{
    if (proc->mk_task != NULL) {
        vfs_pipe_abort_task(proc->mk_task);
    }
    proc_release_resources(proc);
}

void bsd_proc_init(void)
{
    memset(proc_table, 0, sizeof(proc_table));
}

bsd_proc_t *bsd_proc_create(task_t *task, int32_t pid, int32_t ppid,
                            const char *comm)
{
    if (task == NULL) {
        return NULL;
    }

    uint64_t flags = irq_save();

    int32_t assigned = pid;
    if (assigned == 0) {
        assigned = alloc_pid_locked();
        if (assigned == 0) {
            irq_restore(flags);
            return NULL;
        }
    }

    if (assigned <= 0 || assigned >= BSD_MAX_PROCS) {
        irq_restore(flags);
        return NULL;
    }

    /* The BSD personality serialises per-process state (fd_table, mmap_regions,
     * brk, signal masks) with irq_save(), which only excludes the local CPU.
     * A task marked TASK_AFFINITY_ANY can run threads on arbitrary cores, and
     * those threads would race the irq_save-guarded sections from two CPUs
     * simultaneously. Reject such tasks here so the invariant is enforced at
     * the boundary rather than relying on callers. */
    if (task->affinity_cpu == TASK_AFFINITY_ANY) {
        irq_restore(flags);
        return NULL;
    }

    bsd_proc_t *proc = &proc_table[assigned];

    if (proc->state != PROC_UNUSED) {
        irq_restore(flags);
        return NULL;
    }

    memset(proc, 0, sizeof(*proc));

    proc->pid = assigned;
    proc->ppid = ppid;
    proc->pgid = assigned; /* Default: each process starts its own group */
    proc->uid = 0;
    proc->gid = 0;
    proc->state = PROC_RUNNING;
    proc->exit_status = -1;
    proc->mk_task = task;
    proc->mmap_next = BSD_MMAP_BASE;

    if (comm != NULL) {
        size_t i = 0;

        while (i + 1 < BSD_COMM_LEN && comm[i] != '\0') {
            proc->comm[i] = comm[i];
            i++;
        }
        proc->comm[i] = '\0';
    }

    if (ppid > 0) {
        bsd_proc_t *parent = lookup_locked(ppid);

        if (parent != NULL && parent->state == PROC_RUNNING) {
            proc->parent = parent;
            proc->next_sibling = parent->first_child;
            parent->first_child = proc;
        }
    }

    vnode_t *root = vfs_root();
    proc->cwd = root != NULL ? vfs_vnode_ref(root) : NULL;
    task->personality_data = proc;

    irq_restore(flags);

    return proc;
}

bsd_proc_t *bsd_proc_from_task(task_t *task)
{
    if (task == NULL || task->personality_data == NULL) {
        return NULL;
    }

    bsd_proc_t *proc = (bsd_proc_t *)task->personality_data;

    if (proc->mk_task != task) {
        return NULL;
    }

    return proc;
}

bsd_proc_t *bsd_proc_lookup(int32_t pid)
{
    uint64_t flags = irq_save();
    bsd_proc_t *proc = lookup_locked(pid);
    irq_restore(flags);

    return proc;
}

bool bsd_proc_snapshot(int32_t pid, bool want_vspace, bsd_proc_info_t *out)
{
    if (out == NULL) {
        return false;
    }

    memset(out, 0, sizeof(*out));

    uint64_t flags = irq_save();
    bsd_proc_t *proc = lookup_locked(pid);

    if (proc == NULL) {
        irq_restore(flags);
        return false;
    }

    out->pid = proc->pid;
    out->ppid = proc->ppid;
    out->pgid = proc->pgid;
    out->uid = proc->uid;
    out->gid = proc->gid;
    out->state = proc->state;
    out->pending_signals = proc->pending_signals;
    out->signal_mask = proc->signal_mask;
    out->brk_base = proc->brk_base;
    out->brk_current = proc->brk_current;
    memcpy(out->mmap_regions, proc->mmap_regions, sizeof(out->mmap_regions));
    memcpy(out->comm, proc->comm, sizeof(out->comm));
    out->comm[BSD_COMM_LEN - 1] = '\0';

    task_t *task = proc->mk_task;

    if (task != NULL) {
        out->task_id = task->id;
        out->threads = task->thread_count;

        thread_t *first = task->threads;
        if (first != NULL) {
            out->base_qos = first->base_qos;
            out->effective_qos = first->effective_qos;
        }

        /* execve() and teardown swap or clear task->vspace under this same
         * interrupt-masked section on the process's CPU, so the reference is
         * taken on a live address space. */
        if (want_vspace && proc->state == PROC_RUNNING &&
            task->vspace != NULL) {
            vspace_ref(task->vspace);
            out->vspace = task->vspace;
        }
    }

    irq_restore(flags);
    return true;
}

int32_t bsd_proc_next_pid(int32_t after)
{
    int32_t found = 0;
    uint64_t flags = irq_save();

    for (int32_t pid = (after < 0 ? 0 : after) + 1; pid < BSD_MAX_PROCS;
         pid++) {
        if (proc_table[pid].state != PROC_UNUSED) {
            found = pid;
            break;
        }
    }

    irq_restore(flags);
    return found;
}

sys_status_t bsd_proc_install_boot_caps(bsd_proc_t *proc)
{
    if (proc == NULL || proc->mk_task == NULL || proc->mk_task->caps == NULL) {
        return BSD_ERR(BSD_EINVAL);
    }

    uint32_t installed = 0;
    sys_status_t status = cap_boot_install(proc->mk_task->caps, &installed);

    if (status != SYS_OK) {
        return BSD_ERR(BSD_EBUSY);
    }

    klog(KLOG_INFO, "bsd: PID %d received %lu boot capabilit%s",
         (int)proc->pid, (uint64_t)installed, installed == 1 ? "y" : "ies");
    return SYS_OK;
}

bsd_proc_t *bsd_proc_current(void)
{
    thread_t *thread = sched_current_thread();

    return thread != NULL ? bsd_proc_from_task(thread->owner_task) : NULL;
}

int32_t bsd_pid_alloc(void)
{
    uint64_t flags = irq_save();
    int32_t pid = alloc_pid_locked();
    irq_restore(flags);

    return pid != 0 ? pid : (int32_t)BSD_ERR(BSD_EAGAIN);
}

void bsd_pid_release(int32_t pid)
{
    if (pid <= 0 || pid >= BSD_MAX_PROCS) {
        return;
    }

    uint64_t flags = irq_save();

    if (proc_table[pid].state != PROC_RUNNING) {
        memset(&proc_table[pid], 0, sizeof(proc_table[pid]));
    }

    irq_restore(flags);
}

sys_status_t bsd_proc_exit(bsd_proc_t *proc, int32_t status)
{
    if (proc == NULL) {
        return BSD_ERR(BSD_ESRCH);
    }

    int32_t encoded = (int32_t)(((uint32_t)status & 0xFFU) << 8);
    bool adopted = false;

    if (!proc_enter_zombie(proc, encoded, &adopted)) {
        return SYS_OK;
    }

    proc_teardown(proc);

    /* A zombie grandchild reparented to PID 1 must wake a PID 1 that is
     * blocked in wait4(-1) even though this process's own parent is someone
     * else. */
    if (adopted) {
        (void)bsd_signal_send(1, BSD_SIGCHLD);
    }

    return SYS_OK;
}

void bsd_proc_signal_exit(bsd_proc_t *proc, uint32_t signo)
{
    if (proc == NULL) {
        return;
    }

    int32_t encoded = (int32_t)(128U + (signo & 0x7FU));
    bool adopted = false;

    if (!proc_enter_zombie(proc, encoded, &adopted)) {
        return;
    }

    proc_teardown(proc);

    if (adopted) {
        (void)bsd_signal_send(1, BSD_SIGCHLD);
    }
}

void bsd_proc_reap(bsd_proc_t *proc)
{
    if (proc == NULL) {
        return;
    }

    uint64_t flags = irq_save();

    if (proc->state != PROC_ZOMBIE) {
        irq_restore(flags);
        return;
    }

    bsd_proc_t *parent = proc->parent;

    if (parent != NULL) {
        bsd_proc_t **link = &parent->first_child;

        while (*link != NULL && *link != proc) {
            link = &(*link)->next_sibling;
        }
        if (*link == proc) {
            *link = proc->next_sibling;
        }
    }

    memset(proc, 0, sizeof(*proc));
    irq_restore(flags);
}

sys_status_t bsd_signal_send(int32_t pid, uint32_t signo)
{
    if (signo == 0) {
        bsd_proc_t *proc = bsd_proc_lookup(pid);
        return proc != NULL ? SYS_OK : BSD_ERR(BSD_ESRCH);
    }
    if (signo >= BSD_NSIG) {
        return BSD_ERR(BSD_EINVAL);
    }

    uint64_t flags = irq_save();
    bsd_proc_t *proc = lookup_locked(pid);

    if (proc == NULL || proc->state != PROC_RUNNING) {
        irq_restore(flags);
        return BSD_ERR(BSD_ESRCH);
    }

    proc->pending_signals |= BSD_SIGBIT(signo);
    thread_t *signal_waiter = proc->signal_waiter;
    thread_t *wait_waiter = proc->waiter_thread;
    irq_restore(flags);

    if (signal_waiter != NULL) {
        sched_wake(signal_waiter);
    }
    if (wait_waiter != NULL) {
        sched_wake(wait_waiter);
    }

    return SYS_OK;
}

sys_status_t bsd_signal_send_group(int32_t pgid, uint32_t signo)
{
    if (pgid <= 0) {
        return BSD_ERR(BSD_ESRCH);
    }
    if (signo >= BSD_NSIG && signo != 0) {
        return BSD_ERR(BSD_EINVAL);
    }

    /* Collect wake targets while holding the lock, then wake after releasing.
     * This avoids dropping and re-acquiring irq_save mid-iteration, which
     * would create a TOCTOU race on proc_table on SMP. */
    thread_t *wake_signal[BSD_MAX_PROCS];
    thread_t *wake_waiter[BSD_MAX_PROCS];
    size_t wake_count = 0;
    bool found = false;

    uint64_t flags = irq_save();

    for (int32_t i = 1; i < BSD_MAX_PROCS; i++) {
        bsd_proc_t *candidate = &proc_table[i];
        if (candidate->state != PROC_RUNNING || candidate->pgid != pgid) {
            continue;
        }
        found = true;
        if (signo != 0) {
            candidate->pending_signals |= BSD_SIGBIT(signo);
            wake_signal[wake_count] = candidate->signal_waiter;
            wake_waiter[wake_count] = candidate->waiter_thread;
            wake_count++;
        }
    }

    irq_restore(flags);

    /* Wake collected targets outside the lock. */
    for (size_t i = 0; i < wake_count; i++) {
        if (wake_signal[i] != NULL) {
            sched_wake(wake_signal[i]);
        }
        if (wake_waiter[i] != NULL) {
            sched_wake(wake_waiter[i]);
        }
    }

    return found ? SYS_OK : BSD_ERR(BSD_ESRCH);
}

sys_status_t bsd_proc_kill(bsd_proc_t *caller, int32_t pid, uint32_t signo)
{
    /* pid == 0: send to caller's process group. */
    if (pid == 0) {
        if (caller == NULL) {
            return BSD_ERR(BSD_ESRCH);
        }
        return bsd_signal_send_group(caller->pgid, signo);
    }

    /* pid < -1: send to process group abs(pid). */
    if (pid < -1) {
        return bsd_signal_send_group(-pid, signo);
    }

    /* pid == -1: not supported (all processes). */
    if (pid == -1) {
        return BSD_ERR(BSD_ESRCH);
    }

    /* pid > 0: send to specific process. */
    return bsd_signal_send(pid, signo);
}

bool bsd_proc_has_pending(bsd_proc_t *proc)
{
    if (proc == NULL) {
        return false;
    }

    uint64_t flags = irq_save();
    bool pending = deliverable_signals(proc) != 0;
    irq_restore(flags);

    return pending;
}

/* Returns true when signo's default action is process termination. */
static bool default_action_is_fatal(uint32_t signo)
{
    switch (signo) {
    case BSD_SIGHUP:
    case BSD_SIGINT:
    case BSD_SIGQUIT:
    case BSD_SIGILL:
    case BSD_SIGABRT:
    case BSD_SIGTRAP:
    case BSD_SIGFPE:
    case BSD_SIGKILL:
    case BSD_SIGBUS:
    case BSD_SIGSEGV:
    case BSD_SIGPIPE:
    case BSD_SIGTERM:
    case BSD_SIGUSR1:
    case BSD_SIGUSR2:
        return true;
    default:
        return false;
    }
}

/* Builds a bsd_sigframe_t on the user stack and redirects the syscall_frame_t
 * to the custom Ring 3 handler. Returns true on success, false if the frame
 * cannot be delivered (e.g. user stack is not writable). */
static bool deliver_to_user_handler(bsd_proc_t *proc, syscall_frame_t *frame,
                                    uint32_t signo)
{
    if (signo == 0 || signo >= BSD_NSIG || frame == NULL) {
        return false;
    }

    bsd_sigaction_t *act = &proc->actions[signo];
    uint64_t handler = act->sa_handler;

    if (handler <= BSD_SIG_IGN) {
        return false;
    }

    /* Validate handler address is in canonical user space above null page. */
    if (handler < 0x1000 || handler > 0x00007FFFFFFFFFFFULL) {
        return false;
    }

    uint64_t restorer = act->sa_restorer;
    if (restorer < 0x1000 || restorer > 0x00007FFFFFFFFFFFULL) {
        return false;
    }

    /* Build the signal frame. */
    bsd_sigframe_t sigframe;
    memset(&sigframe, 0, sizeof(sigframe));

    sigframe.restorer = restorer;
    sigframe.signo = signo;
    sigframe.saved_mask = proc->signal_mask;

    /* Save all user GPRs from the syscall frame. */
    sigframe.rax = frame->rax;
    sigframe.rdi = frame->rdi;
    sigframe.rsi = frame->rsi;
    sigframe.rdx = frame->rdx;
    sigframe.r10 = frame->r10;
    sigframe.r8 = frame->r8;
    sigframe.r9 = frame->r9;
    sigframe.rcx = frame->rcx;
    sigframe.r11 = frame->r11;
    sigframe.rsp = frame->rsp;
    sigframe.r15 = frame->r15;
    sigframe.r14 = frame->r14;
    sigframe.r13 = frame->r13;
    sigframe.r12 = frame->r12;
    sigframe.rbx = frame->rbx;
    sigframe.rbp = frame->rbp;

    /* 16-byte align the user stack, then push the sigframe. The restorer
     * address is at the top of the frame and acts as the return address when
     * the handler returns via `ret`. */
    uint64_t user_rsp = frame->rsp;
    user_rsp = (user_rsp - sizeof(bsd_sigframe_t)) & ~15ULL;

    /* Validate new RSP is in canonical user space. */
    if (user_rsp > 0x00007FFFFFFFFFFFULL || user_rsp < 0x1000) {
        return false;
    }

    /* Copy the sigframe to user stack. */
    if (copy_to_user((void *)(uintptr_t)user_rsp, &sigframe,
                     sizeof(sigframe)) != SYS_OK) {
        return false;
    }

    /* Update signal mask: block sa_mask and the signal itself during handler. */
    proc->signal_mask |= act->sa_mask | BSD_SIGBIT(signo);
    /* SIGKILL and SIGSTOP can never be masked. */
    proc->signal_mask &= ~BSD_SIG_UNMASKABLE;

    /* Redirect Ring 3 execution to the signal handler.
     * RDI = signo, RSP points to sigframe (restorer is return address),
     * RIP = handler. */
    frame->rcx = handler;          /* user RIP */
    frame->rsp = user_rsp;         /* user RSP (sigframe on stack) */
    frame->rdi = (uint64_t)signo;  /* first argument */

    return true;
}

void bsd_proc_deliver_signals(bsd_proc_t *proc, syscall_frame_t *frame)
{
    if (proc == NULL) {
        return;
    }

    uint64_t flags = irq_save();

    /* Clear any SIG_IGN signals from pending. */
    for (uint32_t s = 1; s < BSD_NSIG; s++) {
        if ((proc->pending_signals & BSD_SIGBIT(s)) == 0) {
            continue;
        }
        if (proc->actions[s].sa_handler == BSD_SIG_IGN &&
            s != BSD_SIGKILL && s != BSD_SIGSTOP) {
            proc->pending_signals &= ~BSD_SIGBIT(s);
        }
        /* Clear default-ignore signals (SIGCHLD, SIGURG) when SIG_DFL. */
        if (proc->actions[s].sa_handler == BSD_SIG_DFL &&
            (s == BSD_SIGCHLD || s == BSD_SIGURG)) {
            proc->pending_signals &= ~BSD_SIGBIT(s);
        }
    }

    uint64_t deliverable = deliverable_signals(proc);

    if (deliverable == 0) {
        irq_restore(flags);
        return;
    }

    /* Find the lowest pending deliverable signal. */
    uint32_t signo = 1;
    while (signo < BSD_NSIG && (deliverable & BSD_SIGBIT(signo)) == 0) {
        signo++;
    }

    if (signo >= BSD_NSIG) {
        irq_restore(flags);
        return;
    }

    proc->pending_signals &= ~BSD_SIGBIT(signo);

    uint64_t handler = proc->actions[signo].sa_handler;
    irq_restore(flags);

    /* Custom Ring 3 handler? */
    if (handler > BSD_SIG_IGN && frame != NULL) {
        if (deliver_to_user_handler(proc, frame, signo)) {
            return;
        }
        /* Fall through to default if delivery fails. */
    }

    /* SIG_DFL or delivery failure: execute default action. */
    if (default_action_is_fatal(signo)) {
        klog(KLOG_WARN, "bsd: PID %d terminating on signal %u",
             (int)proc->pid, signo);
        bsd_proc_signal_exit(proc, signo);
        task_terminate();
    }
    /* Non-fatal default: ignore (SIGCHLD, SIGURG handled above). */
}

/* ------------------------------------------------------------------------- */
/* Stage C: sigaction, sigprocmask, sigreturn, setpgid, getpgid, arch_prctl  */
/* ------------------------------------------------------------------------- */

sys_status_t bsd_proc_sigaction(bsd_proc_t *proc, uint32_t signo,
                                uint64_t unewact, uint64_t uoldact)
{
    if (signo == 0 || signo >= BSD_NSIG) {
        return BSD_ERR(BSD_EINVAL);
    }
    /* SIGKILL and SIGSTOP cannot have their action changed. */
    if (signo == BSD_SIGKILL || signo == BSD_SIGSTOP) {
        return BSD_ERR(BSD_EINVAL);
    }

    bsd_sigaction_t old_act;
    uint64_t flags = irq_save();
    old_act = proc->actions[signo];
    irq_restore(flags);

    /* Copy old action to user if requested. */
    if (uoldact != 0) {
        if (copy_to_user((void *)(uintptr_t)uoldact, &old_act,
                         sizeof(old_act)) != SYS_OK) {
            return BSD_ERR(BSD_EFAULT);
        }
    }

    /* Install new action if provided. */
    if (unewact != 0) {
        bsd_sigaction_t new_act;
        if (copy_from_user(&new_act, (const void *)(uintptr_t)unewact,
                           sizeof(new_act)) != SYS_OK) {
            return BSD_ERR(BSD_EFAULT);
        }

        /* Validate handler address if it is a custom handler: must be in
         * canonical user space above the null-pointer guard page. */
        if (new_act.sa_handler > BSD_SIG_IGN &&
            (new_act.sa_handler < 0x1000 ||
             new_act.sa_handler > 0x00007FFFFFFFFFFFULL)) {
            return BSD_ERR(BSD_EINVAL);
        }

        /* Cannot mask SIGKILL or SIGSTOP in sa_mask. */
        new_act.sa_mask &= ~BSD_SIG_UNMASKABLE;

        flags = irq_save();
        proc->actions[signo] = new_act;
        irq_restore(flags);
    }

    return SYS_OK;
}

sys_status_t bsd_proc_sigprocmask(bsd_proc_t *proc, int32_t how,
                                  uint64_t uset, uint64_t uoldset)
{
    uint64_t flags = irq_save();
    uint64_t old_mask = proc->signal_mask;
    irq_restore(flags);

    if (uoldset != 0) {
        if (copy_to_user((void *)(uintptr_t)uoldset, &old_mask,
                         sizeof(old_mask)) != SYS_OK) {
            return BSD_ERR(BSD_EFAULT);
        }
    }

    if (uset != 0) {
        uint64_t set;
        if (copy_from_user(&set, (const void *)(uintptr_t)uset,
                           sizeof(set)) != SYS_OK) {
            return BSD_ERR(BSD_EFAULT);
        }

        flags = irq_save();
        switch (how) {
        case BSD_SIG_BLOCK:
            proc->signal_mask |= set;
            break;
        case BSD_SIG_UNBLOCK:
            proc->signal_mask &= ~set;
            break;
        case BSD_SIG_SETMASK:
            proc->signal_mask = set;
            break;
        default:
            irq_restore(flags);
            return BSD_ERR(BSD_EINVAL);
        }
        /* SIGKILL and SIGSTOP can never be masked. */
        proc->signal_mask &= ~BSD_SIG_UNMASKABLE;
        irq_restore(flags);
    }

    return SYS_OK;
}

sys_status_t bsd_proc_sigreturn(bsd_proc_t *proc, syscall_frame_t *frame)
{
    if (proc == NULL || frame == NULL) {
        return BSD_ERR(BSD_EINVAL);
    }

    /* The sigframe is on the user stack at the current RSP. The syscall entry
     * consumed the return address (restorer), so the frame pointer is in RDI
     * which the user trampoline passes as the first argument. If the user
     * convention is that RSP points directly at the sigframe, use frame->rsp.
     * Our convention: the trampoline calls sigreturn(rsp) where rsp points at
     * the sigframe. The user passes the frame address in RDI. */
    uint64_t frame_addr = frame->rdi;

    if (frame_addr == 0 || frame_addr > 0x00007FFFFFFFFFFFULL) {
        return BSD_ERR(BSD_EFAULT);
    }

    bsd_sigframe_t sigframe;
    if (copy_from_user(&sigframe, (const void *)(uintptr_t)frame_addr,
                       sizeof(sigframe)) != SYS_OK) {
        return BSD_ERR(BSD_EFAULT);
    }

    /* Validate canonical user RIP and RSP. */
    if (sigframe.rcx > 0x00007FFFFFFFFFFFULL ||
        sigframe.rsp > 0x00007FFFFFFFFFFFULL) {
        return BSD_ERR(BSD_EINVAL);
    }

    /* Restore saved GPRs. */
    frame->rax = sigframe.rax;
    frame->rdi = sigframe.rdi;
    frame->rsi = sigframe.rsi;
    frame->rdx = sigframe.rdx;
    frame->r10 = sigframe.r10;
    frame->r8 = sigframe.r8;
    frame->r9 = sigframe.r9;
    frame->rcx = sigframe.rcx;
    frame->rsp = sigframe.rsp;
    frame->r15 = sigframe.r15;
    frame->r14 = sigframe.r14;
    frame->r13 = sigframe.r13;
    frame->r12 = sigframe.r12;
    frame->rbx = sigframe.rbx;
    frame->rbp = sigframe.rbp;

    /* Sanitize RFLAGS: force IF=1, clear AC, mask privileged bits. */
    uint64_t rflags = sigframe.r11;
    rflags &= ~0x0000000000077500ULL;
    rflags |= 0x202ULL;
    frame->r11 = rflags;

    /* Restore signal mask. SIGKILL and SIGSTOP stay unmaskable. */
    uint64_t flags = irq_save();
    proc->signal_mask = sigframe.saved_mask & ~BSD_SIG_UNMASKABLE;
    irq_restore(flags);

    return SYS_OK;
}

sys_status_t bsd_proc_setpgid(bsd_proc_t *caller, int32_t pid, int32_t pgid)
{
    if (caller == NULL) {
        return BSD_ERR(BSD_ESRCH);
    }

    /* pid == 0 means self. */
    int32_t target_pid = (pid == 0) ? caller->pid : pid;

    /* pgid == 0 means set pgid to the target's pid. */
    int32_t target_pgid = (pgid == 0) ? target_pid : pgid;

    if (target_pgid < 0) {
        return BSD_ERR(BSD_EINVAL);
    }

    uint64_t flags = irq_save();
    bsd_proc_t *target = lookup_locked(target_pid);

    if (target == NULL || target->state != PROC_RUNNING) {
        irq_restore(flags);
        return BSD_ERR(BSD_ESRCH);
    }

    /* Only the process itself or its parent may change its pgid. */
    if (target != caller && target->ppid != caller->pid) {
        irq_restore(flags);
        return BSD_ERR(BSD_EPERM);
    }

    target->pgid = target_pgid;
    irq_restore(flags);

    return SYS_OK;
}

int32_t bsd_proc_getpgid(int32_t pid)
{
    if (pid < 0) {
        return (int32_t)BSD_ERR(BSD_EINVAL);
    }

    /* pid == 0 means self. */
    if (pid == 0) {
        bsd_proc_t *self = bsd_proc_current();
        if (self == NULL) {
            return (int32_t)BSD_ERR(BSD_ESRCH);
        }
        return self->pgid;
    }

    uint64_t flags = irq_save();
    bsd_proc_t *proc = lookup_locked(pid);

    if (proc == NULL || proc->state == PROC_UNUSED) {
        irq_restore(flags);
        return (int32_t)BSD_ERR(BSD_ESRCH);
    }

    int32_t pgid = proc->pgid;
    irq_restore(flags);

    return pgid;
}

sys_status_t bsd_proc_arch_prctl(int32_t code, uint64_t addr,
                                 uint64_t *result)
{
    thread_t *self = sched_current_thread();
    if (self == NULL) {
        return BSD_ERR(BSD_EINVAL);
    }

    switch (code) {
    case BSD_ARCH_SET_FS:
        /* Validate canonical user address. */
        if (addr > 0x00007FFFFFFFFFFFULL) {
            return BSD_ERR(BSD_EINVAL);
        }
        self->fs_base = addr;
        /* Write the MSR immediately so it takes effect for this thread. */
        __asm__ volatile("wrmsr"
                         :
                         : "c"((uint32_t)0xC0000100U),
                           "a"((uint32_t)addr),
                           "d"((uint32_t)(addr >> 32))
                         : "memory");
        return SYS_OK;

    case BSD_ARCH_GET_FS:
        if (result == NULL) {
            return BSD_ERR(BSD_EINVAL);
        }
        *result = self->fs_base;
        return SYS_OK;

    default:
        return BSD_ERR(BSD_EINVAL);
    }
}

/* ------------------------------------------------------------------------- */
/* Phase 3: fork, execve and wait4                                           */
/* ------------------------------------------------------------------------- */

#define BSD_EXEC_MAX_IMAGE (16ULL * 1024ULL * 1024ULL)
#define BSD_EXEC_STACK_LOW (ELF_USER_STACK_TOP - ELF_USER_STACK_SIZE)

/* Heap-allocated (about 8.5 KiB): far too large for a 16 KiB kernel stack. */
typedef struct bsd_exec_args {
    char path[BSD_PATH_MAX];
    char strings[BSD_MAX_EXEC_ARGS][BSD_PATH_MAX];
    char env[BSD_MAX_EXEC_ARGS][BSD_PATH_MAX];
    int argc;
    int envc;
} bsd_exec_args_t;

typedef struct bsd_fork_start {
    syscall_frame_t frame;
} bsd_fork_start_t;

static sys_status_t copy_user_string(uint64_t user_addr, char *dst,
                                     size_t cap, int32_t length_error)
{
    if (user_addr == 0 || dst == NULL || cap == 0) {
        return BSD_ERR(BSD_EFAULT);
    }

    for (size_t i = 0; i < cap; i++) {
        if (copy_from_user(&dst[i],
                           (const void *)(uintptr_t)(user_addr + i), 1) !=
            SYS_OK) {
            return BSD_ERR(BSD_EFAULT);
        }
        if (dst[i] == '\0') {
            return SYS_OK;
        }
    }

    return BSD_ERR(length_error);
}

/* Copies a NULL-terminated user vector of at most BSD_MAX_EXEC_ARGS strings
 * into out[], charging every string (with its NUL) against *payload, which
 * is bounded by BSD_ARG_MAX across argv and envp together. A NULL vector is
 * empty. */
static sys_status_t copy_exec_vector(uint64_t uvec,
                                     char out[BSD_MAX_EXEC_ARGS][BSD_PATH_MAX],
                                     int *count_out, size_t *payload)
{
    *count_out = 0;

    if (uvec == 0) {
        return SYS_OK;
    }

    int index = 0;

    for (; index < BSD_MAX_EXEC_ARGS; index++) {
        uint64_t pointer = 0;

        if (copy_from_user(
                &pointer,
                (const void *)(uintptr_t)(uvec +
                    (uint64_t)index * sizeof(uint64_t)),
                sizeof(uint64_t)) != SYS_OK) {
            return BSD_ERR(BSD_EFAULT);
        }
        if (pointer == 0) {
            *count_out = index;
            return SYS_OK;
        }

        sys_status_t status =
            copy_user_string(pointer, out[index], BSD_PATH_MAX, BSD_E2BIG);
        if (status != SYS_OK) {
            return status;
        }

        size_t length = strlen(out[index]) + 1;
        if (length > BSD_ARG_MAX - *payload) {
            return BSD_ERR(BSD_E2BIG);
        }
        *payload += length;
    }

    uint64_t terminator = 0;

    if (copy_from_user(
            &terminator,
            (const void *)(uintptr_t)(uvec +
                (uint64_t)BSD_MAX_EXEC_ARGS * sizeof(uint64_t)),
            sizeof(uint64_t)) != SYS_OK) {
        return BSD_ERR(BSD_EFAULT);
    }
    if (terminator != 0) {
        return BSD_ERR(BSD_E2BIG);
    }

    *count_out = index;
    return SYS_OK;
}

/* Copies the path and the bounded argv and envp string arrays from the
 * caller's address space before that address space is replaced. */
static sys_status_t copy_exec_arguments(uint64_t upath, uint64_t uargv,
                                        uint64_t uenvp, bsd_exec_args_t *args)
{
    sys_status_t status =
        copy_user_string(upath, args->path, sizeof(args->path),
                         BSD_ENAMETOOLONG);

    if (status != SYS_OK) {
        return status;
    }
    if (args->path[0] == '\0') {
        return BSD_ERR(BSD_ENOENT);
    }

    size_t payload = 0;

    status = copy_exec_vector(uargv, args->strings, &args->argc, &payload);
    if (status != SYS_OK) {
        return status;
    }

    if (args->argc == 0) {
        size_t length = strlen(args->path);

        memcpy(args->strings[0], args->path, length + 1);
        args->argc = 1;
        payload += length + 1;
    }

    return copy_exec_vector(uenvp, args->env, &args->envc, &payload);
}

static sys_status_t write_user_vspace(vspace_t *vspace, vaddr_t dst,
                                      const void *src, size_t len);

/* Lays out the System V initial process stack in new_vspace below
 * ELF_USER_STACK_TOP:
 *
 *   base -> argc | argv[0..argc-1] | NULL | envp[0..envc-1] | NULL
 *           ... padding ... | envp strings | argv strings | (top)
 *
 * base is 16-byte aligned, which is what musl's _start and Rust's runtime
 * expect at process entry (rsp == base). */
static sys_status_t build_exec_stack(vspace_t *new_vspace,
                                     const bsd_exec_args_t *args,
                                     vaddr_t *base_out)
{
    uint64_t arg_addresses[BSD_MAX_EXEC_ARGS];
    uint64_t env_addresses[BSD_MAX_EXEC_ARGS];
    vaddr_t top = ELF_USER_STACK_TOP;
    int argc = args->argc;
    int envc = args->envc;

    for (int i = argc - 1; i >= 0; i--) {
        size_t length = strlen(args->strings[i]) + 1;

        if ((vaddr_t)length > top - BSD_EXEC_STACK_LOW) {
            return BSD_ERR(BSD_E2BIG);
        }
        top -= (vaddr_t)length;
        if (write_user_vspace(new_vspace, top, args->strings[i], length) !=
            SYS_OK) {
            return BSD_ERR(BSD_EFAULT);
        }
        arg_addresses[i] = (uint64_t)top;
    }

    for (int i = envc - 1; i >= 0; i--) {
        size_t length = strlen(args->env[i]) + 1;

        if ((vaddr_t)length > top - BSD_EXEC_STACK_LOW) {
            return BSD_ERR(BSD_E2BIG);
        }
        top -= (vaddr_t)length;
        if (write_user_vspace(new_vspace, top, args->env[i], length) !=
            SYS_OK) {
            return BSD_ERR(BSD_EFAULT);
        }
        env_addresses[i] = (uint64_t)top;
    }

    top &= ~15ULL;

    size_t words = 1U + (size_t)argc + 1U + (size_t)envc + 1U;
    if ((vaddr_t)(words * sizeof(uint64_t)) + 16U > top - BSD_EXEC_STACK_LOW) {
        return BSD_ERR(BSD_E2BIG);
    }

    vaddr_t base = (top - (vaddr_t)(words * sizeof(uint64_t))) & ~15ULL;
    uint64_t stack_words[1 + BSD_MAX_EXEC_ARGS + 1 + BSD_MAX_EXEC_ARGS + 1];
    size_t w = 0;

    stack_words[w++] = (uint64_t)argc;
    for (int i = 0; i < argc; i++) {
        stack_words[w++] = arg_addresses[i];
    }
    stack_words[w++] = 0;
    for (int i = 0; i < envc; i++) {
        stack_words[w++] = env_addresses[i];
    }
    stack_words[w++] = 0;

    if (write_user_vspace(new_vspace, base, stack_words,
                          words * sizeof(uint64_t)) != SYS_OK) {
        return BSD_ERR(BSD_EFAULT);
    }

    *base_out = base;
    return SYS_OK;
}

/* Writes into a not-current vspace by translating each page through the HHDM. */
static sys_status_t write_user_vspace(vspace_t *vspace, vaddr_t dst,
                                      const void *src, size_t len)
{
    const uint8_t *bytes = (const uint8_t *)src;

    while (len > 0) {
        vaddr_t page = dst & ~PAGE_MASK;
        paddr_t phys = 0;

        if (!vspace_translate(vspace, page, &phys)) {
            return BSD_ERR(BSD_EFAULT);
        }

        size_t offset = (size_t)(dst & PAGE_MASK);
        size_t chunk = (size_t)PAGE_SIZE - offset;

        if (chunk > len) {
            chunk = len;
        }

        memcpy((uint8_t *)pmm_phys_to_virt(phys) + offset, bytes, chunk);
        dst += chunk;
        bytes += chunk;
        len -= chunk;
    }

    return SYS_OK;
}

static void set_exec_comm(bsd_proc_t *proc, const char *path)
{
    const char *base = path;

    for (const char *cursor = path; *cursor != '\0'; cursor++) {
        if (*cursor == '/') {
            base = cursor + 1;
        }
    }

    size_t i = 0;

    while (i + 1 < BSD_COMM_LEN && base[i] != '\0') {
        proc->comm[i] = base[i];
        i++;
    }
    proc->comm[i] = '\0';
}

static void bsd_fork_child_entry(void *arg)
{
    bsd_fork_start_t *start = (bsd_fork_start_t *)arg;
    syscall_frame_t frame = start->frame;

    kfree(start);
    user_frame_resume(&frame);
}

static void release_fork_snapshot(file_t *files[BSD_MAX_FDS], vnode_t *cwd)
{
    for (int32_t fd = 0; fd < BSD_MAX_FDS; fd++) {
        if (files[fd] != NULL) {
            vfs_file_unref(files[fd]);
        }
    }
    if (cwd != NULL) {
        vfs_vnode_unref(cwd);
    }
}

int32_t bsd_proc_fork(syscall_frame_t *parent_frame)
{
    bsd_proc_t *parent = bsd_proc_current();


    if (parent == NULL || parent->mk_task == NULL ||
        parent->mk_task->vspace == NULL || parent_frame == NULL) {
        return (int32_t)BSD_ERR(BSD_EINVAL);
    }

    /* The child resumes through IRETQ; a non-canonical RIP or RSP would fault
     * in Ring 0. The parent's SYSCALL saved canonical user state, but reject
     * malformed frames defensively. */
    if (parent_frame->rcx > 0x00007FFFFFFFFFFFULL ||
        parent_frame->rsp > 0x00007FFFFFFFFFFFULL) {
        return (int32_t)BSD_ERR(BSD_EINVAL);
    }

    size_t fork_frames = pmm_free_frame_count();
    uint64_t fork_objects = mk_object_live_count();

    file_t *snapshot_fds[BSD_MAX_FDS];
    bsd_mmap_region_t snapshot_regions[BSD_MAX_MMAPS];
    char snapshot_comm[BSD_COMM_LEN];
    vnode_t *snapshot_cwd = NULL;
    vaddr_t snapshot_brk_base = 0;
    vaddr_t snapshot_brk_current = 0;
    vaddr_t snapshot_mmap_next = 0;
    uint64_t snapshot_signal_mask = 0;
    int32_t snapshot_pgid = 0;
    uint64_t snapshot_fs_base = 0;
    uint32_t snapshot_uid = 0;
    uint32_t snapshot_gid = 0;

    memset(snapshot_fds, 0, sizeof(snapshot_fds));

    /* Snapshot descriptor/cwd references and per-process memory metadata in
     * one interrupt-free section so a concurrent close/chdir cannot race the
     * reference increments. */
    uint64_t snapshot_flags = irq_save();

    for (int32_t fd = 0; fd < BSD_MAX_FDS; fd++) {
        if (parent->fd_table[fd] != NULL) {
            vfs_file_ref(parent->fd_table[fd]);
            snapshot_fds[fd] = parent->fd_table[fd];
        }
    }

    if (parent->cwd != NULL) {
        snapshot_cwd = vfs_vnode_ref(parent->cwd);
    }

    snapshot_brk_base = parent->brk_base;
    snapshot_brk_current = parent->brk_current;
    memcpy(snapshot_regions, parent->mmap_regions, sizeof(snapshot_regions));
    snapshot_mmap_next = parent->mmap_next;
    snapshot_signal_mask = parent->signal_mask;
    snapshot_pgid = parent->pgid;
    snapshot_uid = parent->uid;
    snapshot_gid = parent->gid;
    memcpy(snapshot_comm, parent->comm, sizeof(snapshot_comm));
    snapshot_comm[BSD_COMM_LEN - 1] = '\0';

    /* Inherit the parent thread's TLS base. */
    thread_t *parent_thread = sched_current_thread();
    if (parent_thread != NULL) {
        snapshot_fs_base = parent_thread->fs_base;
    }

    irq_restore(snapshot_flags);

    vspace_t *child_vspace = vspace_clone(parent->mk_task->vspace);
    if (child_vspace == NULL) {
        release_fork_snapshot(snapshot_fds, snapshot_cwd);
        return (int32_t)BSD_ERR(BSD_ENOMEM);
    }

    task_t *child_task = task_create();
    if (child_task == NULL) {
        vspace_destroy(child_vspace);
        release_fork_snapshot(snapshot_fds, snapshot_cwd);
        return (int32_t)BSD_ERR(BSD_ENOMEM);
    }

    vspace_destroy(child_task->vspace);
    child_task->vspace = child_vspace;

    bsd_proc_t *child =
        bsd_proc_create(child_task, 0, parent->pid, snapshot_comm);
    if (child == NULL) {
        task_destroy(child_task);
        release_fork_snapshot(snapshot_fds, snapshot_cwd);
        return (int32_t)BSD_ERR(BSD_ENOMEM);
    }

    /* Ownership of the snapshot references transfers to the child. */
    for (int32_t fd = 0; fd < BSD_MAX_FDS; fd++) {
        child->fd_table[fd] = snapshot_fds[fd];
    }

    vnode_t *previous_cwd = child->cwd;
    child->cwd = snapshot_cwd;
    if (previous_cwd != NULL) {
        vfs_vnode_unref(previous_cwd);
    }

    child->uid = snapshot_uid;
    child->gid = snapshot_gid;
    child->pgid = snapshot_pgid;
    child->signal_mask = snapshot_signal_mask;

    /* Inherit signal actions from parent. */
    memcpy(child->actions, parent->actions, sizeof(child->actions));
    child->brk_base = snapshot_brk_base;
    child->brk_current = snapshot_brk_current;
    memcpy(child->mmap_regions, snapshot_regions, sizeof(snapshot_regions));
    child->mmap_next = snapshot_mmap_next;
    child->fork_frames = fork_frames;
    child->fork_objects = fork_objects;
    child->fork_leak_check = true;

    bsd_fork_start_t *start =
        (bsd_fork_start_t *)kalloc(sizeof(bsd_fork_start_t));
    if (start == NULL) {
        goto fail;
    }

    start->frame = *parent_frame;
    start->frame.rax = 0;

    /* The microkernel publishes the inherited TLS base, and a copy of this
     * thread's live FPU/SSE registers, before the thread can be dequeued by
     * any CPU. */
    thread_t *child_thread = thread_create_inheriting(
        child_task, bsd_fork_child_entry, start, snapshot_fs_base);

    if (child_thread == NULL) {
        kfree(start);
        goto fail;
    }

    return child->pid;

fail:
    (void)bsd_proc_exit(child, 127);
    child_task->personality_data = NULL;
    child->mk_task = NULL;
    bsd_proc_reap(child);
    task_destroy(child_task);
    return (int32_t)BSD_ERR(BSD_ENOMEM);
}

static sys_status_t exec_image(bsd_proc_t *proc, syscall_frame_t *frame,
                               const bsd_exec_args_t *args);

sys_status_t bsd_proc_execve(syscall_frame_t *frame, const char *upath,
                             char *const uargv[], char *const uenvp[])
{
    bsd_proc_t *proc = bsd_proc_current();

    if (proc == NULL || proc->mk_task == NULL || frame == NULL) {
        return BSD_ERR(BSD_EINVAL);
    }

    bsd_exec_args_t *args = (bsd_exec_args_t *)kalloc(sizeof(*args));
    if (args == NULL) {
        return BSD_ERR(BSD_ENOMEM);
    }
    memset(args, 0, sizeof(*args));

    sys_status_t status =
        copy_exec_arguments((uint64_t)(uintptr_t)upath,
                            (uint64_t)(uintptr_t)uargv,
                            (uint64_t)(uintptr_t)uenvp, args);
    if (status == SYS_OK) {
        status = exec_image(proc, frame, args);
    }

    kfree(args);
    return status;
}

/* Loads args->path over the calling process. On failure the caller's address
 * space is untouched. */
static sys_status_t exec_image(bsd_proc_t *proc, syscall_frame_t *frame,
                               const bsd_exec_args_t *args)
{
    file_t *file = NULL;
    sys_status_t status = vfs_open(proc, args->path, BSD_O_RDONLY, 0, &file);
    if (status != SYS_OK) {
        return status;
    }

    vnode_t *vnode = file->vnode;
    if (vnode == NULL || vnode->type != VNODE_REG) {
        bool is_dir = vnode != NULL && vnode->type == VNODE_DIR;
        vfs_file_unref(file);
        return BSD_ERR(is_dir ? BSD_EISDIR : BSD_EACCES);
    }
    if ((vnode->mode & 0111U) == 0U) {
        vfs_file_unref(file);
        return BSD_ERR(BSD_EACCES);
    }
    if (vnode->size == 0 || vnode->size > BSD_EXEC_MAX_IMAGE) {
        vfs_file_unref(file);
        return BSD_ERR(BSD_ENOEXEC);
    }

    size_t image_size = (size_t)vnode->size;
    uint8_t *image = (uint8_t *)kalloc(image_size);

    if (image == NULL) {
        vfs_file_unref(file);
        return BSD_ERR(BSD_ENOMEM);
    }

    size_t total = 0;
    while (total < image_size) {
        size_t done = 0;

        status = vfs_read(file, image + total, image_size - total, &done);
        if (status != SYS_OK || done == 0) {
            break;
        }
        total += done;
    }
    vfs_file_unref(file);

    if (total != image_size) {
        kfree(image);
        return status != SYS_OK ? status : BSD_ERR(BSD_EIO);
    }

    vspace_t *new_vspace = vspace_create();
    if (new_vspace == NULL) {
        kfree(image);
        return BSD_ERR(BSD_ENOMEM);
    }

    uint64_t entry = 0;
    status = elf64_load(new_vspace, image, image_size, &entry);
    uint64_t brk_base =
        status == SYS_OK ? bsd_elf_image_break(image, image_size) : 0;
    kfree(image);

    if (status != SYS_OK) {
        vspace_destroy(new_vspace);
        return status == SYS_ERR_NOMEM ? BSD_ERR(BSD_ENOMEM)
                                       : BSD_ERR(BSD_ENOEXEC);
    }
    if (brk_base == 0) {
        vspace_destroy(new_vspace);
        return BSD_ERR(BSD_ENOEXEC);
    }

    vaddr_t base = 0;

    status = build_exec_stack(new_vspace, args, &base);
    if (status != SYS_OK) {
        goto fail_vspace;
    }

    int argc = args->argc;
    vaddr_t argv_user = base + sizeof(uint64_t);
    vaddr_t envp_user =
        base + (vaddr_t)((1U + (size_t)(argc + 1)) * sizeof(uint64_t));

    task_t *task = proc->mk_task;
    uint64_t flags = irq_save();
    vspace_t *old_vspace = task->vspace;
    task->vspace = new_vspace;
    irq_restore(flags);

    vspace_switch(new_vspace);
    vspace_destroy(old_vspace);

    uint64_t reset_flags = irq_save();
    proc->brk_base = brk_base;
    proc->brk_current = brk_base;
    memset(proc->mmap_regions, 0, sizeof(proc->mmap_regions));
    proc->mmap_next = BSD_MMAP_BASE;
    proc->pending_signals = 0;
    proc->signal_waiter = NULL;
    /* On execve: reset custom handlers to SIG_DFL, preserve SIG_IGN and
     * signal_mask per POSIX. */
    for (uint32_t s = 1; s < BSD_NSIG; s++) {
        if (proc->actions[s].sa_handler != BSD_SIG_IGN) {
            memset(&proc->actions[s], 0, sizeof(proc->actions[s]));
        }
    }
    irq_restore(reset_flags);

    /* Clear TLS base on execve. */
    thread_t *exec_thread = sched_current_thread();
    if (exec_thread != NULL) {
        exec_thread->fs_base = 0;
        __asm__ volatile("wrmsr"
                         :
                         : "c"((uint32_t)0xC0000100U),
                           "a"((uint32_t)0),
                           "d"((uint32_t)0)
                         : "memory");
    }

    /* The new image starts from the clean FPU/SSE state (FCW 0x037F, MXCSR
     * 0x1F80, XMM0-15 zero) rather than inheriting the old program's. */
    thread_fpu_reset_current();

    set_exec_comm(proc, args->path);

    frame->rcx = entry;
    frame->rsp = (uint64_t)base;
    frame->rax = 0;
    frame->rdi = (uint64_t)argc;
    frame->rsi = (uint64_t)argv_user;
    frame->rdx = (uint64_t)envp_user;
    return SYS_OK;

fail_vspace:
    vspace_destroy(new_vspace);
    return status;
}

int32_t bsd_proc_wait4(int32_t target_pid, uint64_t uwstatus, int32_t options)
{
    if ((options & ~BSD_WNOHANG) != 0 || target_pid == 0 || target_pid < -1) {
        return (int32_t)BSD_ERR(BSD_EINVAL);
    }

    bsd_proc_t *parent = bsd_proc_current();
    if (parent == NULL) {
        return (int32_t)BSD_ERR(BSD_EINVAL);
    }

    for (;;) {
        bsd_proc_t *child = NULL;
        bool have_child = false;

        uint64_t flags = irq_save();

        for (bsd_proc_t *candidate = parent->first_child;
             candidate != NULL; candidate = candidate->next_sibling) {
            if (target_pid != -1 && candidate->pid != target_pid) {
                continue;
            }
            if (candidate->state == PROC_UNUSED) {
                continue;
            }

            have_child = true;

            if (candidate->state == PROC_ZOMBIE &&
                (candidate->mk_task == NULL ||
                 candidate->mk_task->thread_count == 0)) {
                child = candidate;
                break;
            }
        }

        if (child == NULL) {
            if (!have_child) {
                irq_restore(flags);
                return (int32_t)BSD_ERR(BSD_ECHILD);
            }
            if ((options & BSD_WNOHANG) != 0) {
                irq_restore(flags);
                return 0;
            }
            if (deliverable_signals(parent) != 0) {
                irq_restore(flags);
                return (int32_t)BSD_ERR(BSD_EINTR);
            }

            thread_t *self = sched_current_thread();
            parent->waiter_thread = self;
            self->state = THREAD_BLOCKED_RECV;
            irq_restore(flags);

            sched_block_current();

            flags = irq_save();
            if (parent->waiter_thread == self) {
                parent->waiter_thread = NULL;
            }
            irq_restore(flags);

            if (bsd_proc_has_pending(parent)) {
                return (int32_t)BSD_ERR(BSD_EINTR);
            }
            continue;
        }

        irq_restore(flags);

        int32_t pid = child->pid;
        int32_t encoded = child->exit_status;
        int32_t status =
            (encoded >= 1 && encoded < 256) ? (encoded & 0x7F) : encoded;

        if (uwstatus != 0) {
            if (copy_to_user((void *)(uintptr_t)uwstatus, &status,
                             sizeof(status)) != SYS_OK) {
                return (int32_t)BSD_ERR(BSD_EFAULT);
            }
        }

        task_t *task = child->mk_task;
        char comm[BSD_COMM_LEN];
        memcpy(comm, child->comm, sizeof(comm));
        comm[BSD_COMM_LEN - 1] = '\0';
        size_t fork_frames = child->fork_frames;
        uint64_t fork_objects = child->fork_objects;
        bool check = child->fork_leak_check;

        child->mk_task = NULL;
        if (task != NULL) {
            task->personality_data = NULL;
            task_destroy(task);
        }
        bsd_proc_reap(child);

        size_t frames_after = pmm_free_frame_count();
        uint64_t objects_after = mk_object_live_count();

        if (check) {
            if (frames_after == fork_frames &&
                objects_after == fork_objects) {
                klog(KLOG_INFO,
                     "bsd: reaped PID %d (%s), wait status %d, "
                     "0 leaked PMM frames/objects (%lu free)",
                     (int)pid, comm, (int)status,
                     (unsigned long)frames_after);
            } else {
                klog(KLOG_WARN,
                     "bsd: reaped PID %d (%s), wait status %d, "
                     "accounting frames %lu -> %lu, objects %lu -> %lu",
                     (int)pid, comm, (int)status,
                     (unsigned long)fork_frames,
                     (unsigned long)frames_after,
                     (unsigned long)fork_objects,
                     (unsigned long)objects_after);
            }
        } else {
            klog(KLOG_INFO, "bsd: reaped PID %d (%s), wait status %d",
                 (int)pid, comm, (int)status);
        }

        return pid;
    }
}

/* ------------------------------------------------------------------------- */
/* mk_hooks_t callbacks                                                      */
/* ------------------------------------------------------------------------- */

static void bsd_on_personality_syscall(thread_t *caller,
                                       syscall_frame_t *frame)
{
    task_t *task = caller != NULL ? caller->owner_task : NULL;
    bsd_proc_t *proc = bsd_proc_from_task(task);

    if (proc == NULL || proc->state != PROC_RUNNING) {
        frame->rax = (uint64_t)BSD_ERR(BSD_ENOSYS);
        return;
    }

    bsd_syscall_dispatch(proc, frame);

    if (proc->state == PROC_RUNNING) {
        bsd_proc_deliver_signals(proc, frame);
    }
}

static uint32_t bsd_signal_for_vector(uint64_t vector)
{
    switch (vector) {
    case 0:  /* #DE divide error */
    case 16: /* #MF x87 floating point */
    case 19: /* #XM SIMD floating point */
        return BSD_SIGFPE;
    case 1:  /* #DB debug */
    case 3:  /* #BP breakpoint */
        return BSD_SIGTRAP;
    case 6:  /* #UD invalid opcode */
        return BSD_SIGILL;
    case 13: /* #GP general protection */
    case 17: /* #AC alignment check */
        return BSD_SIGBUS;
    case 14: /* #PF page fault */
    default:
        return BSD_SIGSEGV;
    }
}

static bool bsd_on_user_exception(thread_t *faulting_thread, uint64_t vector,
                                  uint64_t error_code, uint64_t fault_addr)
{
    task_t *task = faulting_thread != NULL ? faulting_thread->owner_task : NULL;
    bsd_proc_t *proc = bsd_proc_from_task(task);

    if (proc == NULL || proc->state != PROC_RUNNING) {
        return false;
    }

    uint32_t signo = bsd_signal_for_vector(vector);

    klog(KLOG_WARN,
         "bsd: PID %d user fault vector=%lu error=0x%lx addr=0x%lx -> signal %u",
         (int)proc->pid, vector, error_code, fault_addr, signo);

    bsd_proc_signal_exit(proc, signo);
    task_terminate();

    return true;
}

static void bsd_on_task_exit(task_t *task, int64_t exit_status)
{
    bsd_proc_t *proc = bsd_proc_from_task(task);

    if (proc == NULL) {
        return;
    }

    if (proc->state != PROC_ZOMBIE) {
        klog(KLOG_INFO, "bsd: PID %d (%s) exited with status %ld",
             (int)proc->pid, proc->comm, (long)exit_status);

        (void)bsd_proc_exit(proc, (int32_t)exit_status);
    }

    /* The task owns no threads now. SIGCHLD wakes a parent blocked in wait4
     * through bsd_signal_send()'s waiter wakeup. */
    if (proc->ppid > 0) {
        (void)bsd_signal_send(proc->ppid, BSD_SIGCHLD);
    }
}

static const mk_hooks_t bsd_hooks = {
    .on_personality_syscall = bsd_on_personality_syscall,
    .on_user_exception = bsd_on_user_exception,
    .on_task_exit = bsd_on_task_exit,
};

const mk_hooks_t *bsd_proc_get_hooks(void)
{
    return &bsd_hooks;
}
