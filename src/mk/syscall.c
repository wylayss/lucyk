#include <cap.h>
#include <io.h>
#include <ipc.h>
#include <klog.h>
#include <mk_hooks.h>
#include <object.h>
#include <sched.h>
#include <string.h>
#include <syscall.h>
#include <syscall_numbers.h>
#include <task.h>
#include <types.h>
#include <vspace.h>

#define USER_NULL_GUARD 0x1000ULL
#define USER_DEBUG_MAX  256U

static mk_hooks_t registered_hooks;
static bool hooks_registered;

void mk_register_hooks(const mk_hooks_t *hooks)
{
    if (hooks == NULL) {
        memset(&registered_hooks, 0, sizeof(registered_hooks));
        hooks_registered = false;
        return;
    }

    registered_hooks = *hooks;
    hooks_registered = true;
}

const mk_hooks_t *mk_hooks_get(void)
{
    return hooks_registered ? &registered_hooks : NULL;
}

static void dispatch_ipc_call(task_t *task, syscall_frame_t *frame)
{
    uint64_t msg[4] = { frame->rsi, frame->rdx, frame->r10, frame->r8 };
    uint64_t reply[4] = { 0, 0, 0, 0 };
    uint64_t badge = 0;

    sys_status_t status = ipc_call(task, (cap_t)frame->rdi, msg, reply, &badge);
    frame->rax = status;

    if (status == SYS_OK) {
        frame->rdx = badge;
        frame->rsi = reply[0];
        frame->r8 = reply[1];
        frame->r9 = reply[2];
        frame->r10 = reply[3];
    }
}

static void dispatch_ipc_recv(task_t *task, thread_t *current,
                              syscall_frame_t *frame)
{
    uint64_t msg[4] = { 0, 0, 0, 0 };
    uint64_t badge = 0;
    thread_t *sender = NULL;

    sys_status_t status = ipc_recv(task, (cap_t)frame->rdi, msg, &badge, &sender);
    frame->rax = status;

    if (status == SYS_OK) {
        current->ipc_sender = sender;
        frame->rdx = badge;
        frame->rsi = msg[0];
        frame->r8 = msg[1];
        frame->r9 = msg[2];
        frame->r10 = msg[3];
    }
}

static void dispatch_ipc_reply(thread_t *current, syscall_frame_t *frame)
{
    uint64_t reply[4] = { frame->rdi, frame->rsi, frame->rdx, frame->r10 };

    frame->rax = ipc_reply(current->ipc_sender, reply);
}

static void dispatch_frame_map(task_t *task, syscall_frame_t *frame)
{
    uint64_t vaddr = frame->rsi;
    uint64_t prot = frame->rdx & (VSPACE_PROT_WRITE | VSPACE_PROT_EXEC);

    if (vaddr < USER_NULL_GUARD || (vaddr & PAGE_MASK) != 0) {
        frame->rax = SYS_ERR_INVAL;
        return;
    }

    uint64_t required = CAP_RIGHT_READ;
    if ((prot & VSPACE_PROT_WRITE) != 0) {
        required |= CAP_RIGHT_WRITE;
    }

    void *object = NULL;
    sys_status_t status = cap_lookup_retained(task->caps, (cap_t)frame->rdi,
                                              CAP_OBJ_FRAME, required,
                                              &object, NULL);
    if (status != SYS_OK) {
        frame->rax = status;
        return;
    }

    cap_frame_object_t *frame_object = (cap_frame_object_t *)object;

    /* Transfer frame ownership to the vspace. This prevents a later cap delete
     * from freeing a frame that remains mapped, and prevents a second map of
     * the same frame capability from claiming the same physical frame twice. */
    uint64_t map_flags = irq_save();
    if (frame_object->transferred) {
        irq_restore(map_flags);
        mk_object_release(&frame_object->header);
        frame->rax = SYS_ERR_BUSY;
        return;
    }
    frame_object->transferred = true;
    irq_restore(map_flags);

    status = vspace_map_user(task->vspace, vaddr, frame_object->phys,
                             prot | VSPACE_PROT_USER | VSPACE_PROT_OWNED);
    if (status != SYS_OK) {
        map_flags = irq_save();
        frame_object->transferred = false;
        irq_restore(map_flags);
    }

    mk_object_release(&frame_object->header);
    frame->rax = status;
}

static void dispatch_range_unmap(task_t *task, syscall_frame_t *frame)
{
    uint64_t vaddr = frame->rdi;
    uint64_t size = frame->rsi;

    if (size == 0 || (vaddr & PAGE_MASK) != 0 ||
        vaddr < USER_NULL_GUARD || vaddr > 0x00007FFFFFFFFFFFULL) {
        frame->rax = SYS_ERR_INVAL;
        return;
    }
    if (size > 0x00007FFFFFFFFFFFULL - vaddr + 1ULL) {
        frame->rax = SYS_ERR_INVAL;
        return;
    }

    sys_status_t status = SYS_OK;
    uint64_t end = vaddr + size;

    for (uint64_t addr = vaddr; addr < end; addr += PAGE_SIZE) {
        status = vspace_unmap(task->vspace, addr);
        if (status != SYS_OK) {
            break;
        }
    }

    frame->rax = status;
}

static void dispatch_debug_log(syscall_frame_t *frame)
{
    size_t len = (size_t)frame->rsi;

    if (len > USER_DEBUG_MAX) {
        frame->rax = SYS_ERR_INVAL;
        return;
    }

    char buffer[USER_DEBUG_MAX + 1];
    sys_status_t status = copy_from_user(buffer, (const void *)(uintptr_t)frame->rdi,
                                         len);
    if (status != SYS_OK) {
        frame->rax = status;
        return;
    }

    buffer[len] = '\0';
    klog(KLOG_INFO, "user log: %s", buffer);
    frame->rax = SYS_OK;
}

void syscall_dispatch(syscall_frame_t *frame)
{
    thread_t *current = sched_current_thread();

    if (current == NULL || current->owner_task == NULL) {
        frame->rax = SYS_ERR_DEAD;
        return;
    }

    task_t *task = current->owner_task;
    uint64_t number = frame->rax;

    if ((number & (1ULL << 63)) == 0) {
        const mk_hooks_t *hooks = mk_hooks_get();

        if (hooks != NULL && hooks->on_personality_syscall != NULL) {
            hooks->on_personality_syscall(current, frame);
        } else {
            frame->rax = SYS_ERR_NOSYS;
        }
        return;
    }

    switch (number) {
    case SYS_THREAD_EXIT:
        current->exit_code = (int64_t)frame->rdi;
        thread_exit();
        break;

    case SYS_THREAD_YIELD:
        thread_yield();
        frame->rax = SYS_OK;
        break;

    case SYS_THREAD_SLEEP_MS:
        thread_sleep_ms(frame->rdi);
        frame->rax = SYS_OK;
        break;

    case SYS_IPC_CALL:
        dispatch_ipc_call(task, frame);
        break;

    case SYS_IPC_RECV:
        dispatch_ipc_recv(task, current, frame);
        break;

    case SYS_IPC_REPLY:
        dispatch_ipc_reply(current, frame);
        break;

    case SYS_NOTIFY_SIGNAL: {
        void *object = NULL;
        sys_status_t status = cap_lookup_retained(task->caps, (cap_t)frame->rdi,
                                                   CAP_OBJ_NOTIFICATION,
                                                   CAP_RIGHT_WRITE, &object,
                                                   NULL);
        if (status == SYS_OK) {
            notification_t *notification = (notification_t *)object;
            notify_signal(notification, frame->rsi);
            mk_object_release(&notification->header);
        }
        frame->rax = status;
        break;
    }

    case SYS_NOTIFY_WAIT: {
        uint64_t bits = 0;
        sys_status_t status = notify_wait(task, (cap_t)frame->rdi, &bits);
        frame->rax = status;
        if (status == SYS_OK) {
            frame->rdx = bits;
        }
        break;
    }

    case SYS_CAP_DERIVE: {
        cap_t new_cap = CAP_NULL;
        sys_status_t status = cap_derive(task->caps, (cap_t)frame->rdi,
                                          frame->rsi, frame->rdx, &new_cap);
        frame->rax = status;
        if (status == SYS_OK) {
            frame->rdx = (uint64_t)new_cap;
        }
        break;
    }

    case SYS_CAP_GRANT: {
        /* User-visible grants duplicate within the calling task; a task
         * capability is not yet exposed by this ABI. */
        cap_t new_cap = CAP_NULL;
        sys_status_t status = cap_grant(task->caps, (cap_t)frame->rdi,
                                         task->caps, &new_cap);
        frame->rax = status;
        if (status == SYS_OK) {
            frame->rdx = (uint64_t)new_cap;
        }
        break;
    }

    case SYS_CAP_DELETE:
        frame->rax = cap_delete(task->caps, (cap_t)frame->rdi);
        break;

    case SYS_VSPACE_MAP:
        dispatch_frame_map(task, frame);
        break;

    case SYS_VSPACE_UNMAP:
        dispatch_range_unmap(task, frame);
        break;

    case SYS_DEBUG_LOG:
        dispatch_debug_log(frame);
        break;

    default:
        frame->rax = SYS_ERR_NOSYS;
        break;
    }
}
