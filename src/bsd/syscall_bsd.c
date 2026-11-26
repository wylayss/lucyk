#include <bsd/bsd.h>
#include <bsd/console.h>
#include <bsd/errno.h>
#include <bsd/mmap.h>
#include <bsd/proc.h>
#include <bsd/syscall_bsd.h>
#include <bsd/vfs.h>
#include <sched.h>
#include <string.h>
#include <syscall.h>
#include <task.h>
#include <types.h>
#include <vspace.h>

static sys_status_t copy_user_path(uint64_t user_ptr, char *out)
{
    if (user_ptr == 0) {
        return BSD_ERR(BSD_EFAULT);
    }

    for (size_t i = 0; i < BSD_PATH_MAX; i++) {
        sys_status_t status =
            copy_from_user(&out[i],
                           (const void *)(uintptr_t)(user_ptr + i), 1);

        if (status != SYS_OK) {
            return BSD_ERR(BSD_EFAULT);
        }
        if (out[i] == '\0') {
            return i == 0 ? BSD_ERR(BSD_ENOENT) : SYS_OK;
        }
    }

    return BSD_ERR(BSD_ENAMETOOLONG);
}

static sys_status_t copy_user_in(void *kernel_dst, uint64_t user_src,
                                 size_t len)
{
    if (len == 0) {
        return SYS_OK;
    }
    if (copy_from_user(kernel_dst, (const void *)(uintptr_t)user_src, len) !=
        SYS_OK) {
        return BSD_ERR(BSD_EFAULT);
    }
    return SYS_OK;
}

static sys_status_t copy_user_out(uint64_t user_dst, const void *kernel_src,
                                  size_t len)
{
    if (len == 0) {
        return SYS_OK;
    }
    if (copy_to_user((void *)(uintptr_t)user_dst, kernel_src, len) != SYS_OK) {
        return BSD_ERR(BSD_EFAULT);
    }
    return SYS_OK;
}

static void sys_exit(bsd_proc_t *proc, syscall_frame_t *frame)
{
    int32_t status = (int32_t)(frame->rdi & 0xFFU);

    (void)bsd_proc_exit(proc, status);
    thread_exit();
}

static void sys_read(bsd_proc_t *proc, syscall_frame_t *frame)
{
    int32_t fd = (int32_t)frame->rdi;
    size_t count = (size_t)frame->rdx;

    if (count > BSD_IO_CHUNK) {
        count = BSD_IO_CHUNK;
    }
    if (count == 0) {
        frame->rax = 0;
        return;
    }

    file_t *file = NULL;
    sys_status_t status = bsd_fd_get(proc, fd, &file);

    if (status != SYS_OK) {
        frame->rax = (uint64_t)BSD_ERR(BSD_EBADF);
        return;
    }

    uint8_t buffer[BSD_IO_CHUNK];
    size_t done = 0;
    uint64_t saved_offset = file->offset;

    status = vfs_read(file, buffer, count, &done);

    if (status != SYS_OK) {
        vfs_file_unref(file);
        frame->rax = (uint64_t)status;
        return;
    }

    status = copy_user_out(frame->rsi, buffer, done);
    if (status != SYS_OK) {
        if (file->vnode != NULL && file->vnode->type == VNODE_REG) {
            file->offset = saved_offset;
        }
        vfs_file_unref(file);
        frame->rax = (uint64_t)status;
        return;
    }

    vfs_file_unref(file);
    frame->rax = (uint64_t)done;
}

static void sys_write(bsd_proc_t *proc, syscall_frame_t *frame)
{
    int32_t fd = (int32_t)frame->rdi;
    size_t count = (size_t)frame->rdx;

    if (count > BSD_IO_CHUNK) {
        count = BSD_IO_CHUNK;
    }
    if (count == 0) {
        frame->rax = 0;
        return;
    }

    file_t *file = NULL;
    sys_status_t status = bsd_fd_get(proc, fd, &file);

    if (status != SYS_OK) {
        frame->rax = (uint64_t)BSD_ERR(BSD_EBADF);
        return;
    }

    uint8_t buffer[BSD_IO_CHUNK];

    status = copy_user_in(buffer, frame->rsi, count);
    if (status == SYS_OK) {
        size_t done = 0;

        status = vfs_write(file, buffer, count, &done);
        if (status == SYS_OK) {
            frame->rax = (uint64_t)done;
        } else {
            frame->rax = (uint64_t)status;
        }
    } else {
        frame->rax = (uint64_t)status;
    }

    vfs_file_unref(file);
}

static void sys_open(bsd_proc_t *proc, syscall_frame_t *frame)
{
    char path[BSD_PATH_MAX];

    sys_status_t status = copy_user_path(frame->rdi, path);
    if (status != SYS_OK) {
        frame->rax = (uint64_t)status;
        return;
    }

    file_t *file = NULL;
    status = vfs_open(proc, path, (uint32_t)frame->rsi, (uint32_t)frame->rdx,
                      &file);
    if (status != SYS_OK) {
        frame->rax = (uint64_t)status;
        return;
    }

    int32_t fd = bsd_fd_alloc(proc, file);
    if (fd < 0) {
        vfs_file_unref(file);
        frame->rax = (uint64_t)(int64_t)fd;
        return;
    }

    frame->rax = (uint64_t)(int64_t)fd;
}

static void sys_close(bsd_proc_t *proc, syscall_frame_t *frame)
{
    frame->rax = (uint64_t)bsd_fd_close(proc, (int32_t)frame->rdi);
}

static void sys_getpid(bsd_proc_t *proc, syscall_frame_t *frame)
{
    frame->rax = (uint64_t)(int64_t)proc->pid;
}

static void sys_getppid(bsd_proc_t *proc, syscall_frame_t *frame)
{
    frame->rax = (uint64_t)(int64_t)proc->ppid;
}

static void sys_getuid(bsd_proc_t *proc, syscall_frame_t *frame)
{
    frame->rax = (uint64_t)proc->uid;
}

static void sys_getgid(bsd_proc_t *proc, syscall_frame_t *frame)
{
    frame->rax = (uint64_t)proc->gid;
}

static void sys_fstat(bsd_proc_t *proc, syscall_frame_t *frame)
{
    file_t *file = NULL;
    sys_status_t status = bsd_fd_get(proc, (int32_t)frame->rdi, &file);

    if (status != SYS_OK) {
        frame->rax = (uint64_t)BSD_ERR(BSD_EBADF);
        return;
    }

    bsd_stat_t st;
    status = vfs_fstat(file, &st);
    vfs_file_unref(file);

    if (status != SYS_OK) {
        frame->rax = (uint64_t)status;
        return;
    }

    status = copy_user_out(frame->rsi, &st, sizeof(st));
    frame->rax = status == SYS_OK ? 0 : (uint64_t)status;
}

static void sys_lseek(bsd_proc_t *proc, syscall_frame_t *frame)
{
    file_t *file = NULL;
    sys_status_t status = bsd_fd_get(proc, (int32_t)frame->rdi, &file);

    if (status != SYS_OK) {
        frame->rax = (uint64_t)BSD_ERR(BSD_EBADF);
        return;
    }

    uint64_t result = 0;

    status = vfs_lseek(file, (int64_t)frame->rsi, (uint32_t)frame->rdx,
                       &result);
    vfs_file_unref(file);

    if (status != SYS_OK) {
        frame->rax = (uint64_t)status;
        return;
    }

    frame->rax = result;
}

static void sys_mmap(bsd_proc_t *proc, syscall_frame_t *frame)
{
    vaddr_t result = 0;
    sys_status_t status = bsd_mmap(proc, frame->rdi, frame->rsi,
                                   (uint32_t)frame->rdx, (uint32_t)frame->r10,
                                   (int32_t)frame->r8, (int64_t)frame->r9,
                                   &result);

    frame->rax = status == SYS_OK ? (uint64_t)result : (uint64_t)status;
}

static void sys_munmap(bsd_proc_t *proc, syscall_frame_t *frame)
{
    frame->rax = (uint64_t)bsd_munmap(proc, frame->rdi, frame->rsi);
}

static void sys_brk(bsd_proc_t *proc, syscall_frame_t *frame)
{
    vaddr_t result = 0;
    sys_status_t status = bsd_brk(proc, frame->rdi, &result);

    frame->rax = status == SYS_OK ? (uint64_t)result : (uint64_t)status;
}

static void sys_stat(bsd_proc_t *proc, syscall_frame_t *frame)
{
    char path[BSD_PATH_MAX];

    sys_status_t status = copy_user_path(frame->rdi, path);
    if (status != SYS_OK) {
        frame->rax = (uint64_t)status;
        return;
    }

    bsd_stat_t st;
    status = vfs_stat_path(proc, path, &st);
    if (status != SYS_OK) {
        frame->rax = (uint64_t)status;
        return;
    }

    status = copy_user_out(frame->rsi, &st, sizeof(st));
    frame->rax = status == SYS_OK ? 0 : (uint64_t)status;
}

static void sys_getdents(bsd_proc_t *proc, syscall_frame_t *frame)
{
    int32_t fd = (int32_t)frame->rdi;
    size_t count = (size_t)frame->rdx;

    if (count > BSD_IO_CHUNK) {
        count = BSD_IO_CHUNK;
    }
    if (count == 0) {
        frame->rax = 0;
        return;
    }

    file_t *file = NULL;
    sys_status_t status = bsd_fd_get(proc, fd, &file);

    if (status != SYS_OK) {
        frame->rax = (uint64_t)BSD_ERR(BSD_EBADF);
        return;
    }

    uint8_t buffer[BSD_IO_CHUNK];
    size_t done = 0;
    uint64_t saved_offset = file->offset;

    status = vfs_getdents(file, buffer, count, &done);
    if (status != SYS_OK) {
        vfs_file_unref(file);
        frame->rax = (uint64_t)status;
        return;
    }

    status = copy_user_out(frame->rsi, buffer, done);
    if (status != SYS_OK) {
        file->offset = saved_offset;
        vfs_file_unref(file);
        frame->rax = (uint64_t)status;
        return;
    }

    vfs_file_unref(file);
    frame->rax = (uint64_t)done;
}

static void sys_chdir(bsd_proc_t *proc, syscall_frame_t *frame)
{
    char path[BSD_PATH_MAX];

    sys_status_t status = copy_user_path(frame->rdi, path);
    if (status != SYS_OK) {
        frame->rax = (uint64_t)status;
        return;
    }

    frame->rax = (uint64_t)vfs_chdir(proc, path);
}

static void sys_mkdir(bsd_proc_t *proc, syscall_frame_t *frame)
{
    char path[BSD_PATH_MAX];

    sys_status_t status = copy_user_path(frame->rdi, path);
    if (status != SYS_OK) {
        frame->rax = (uint64_t)status;
        return;
    }

    frame->rax = (uint64_t)vfs_mkdir(proc, path, (uint32_t)frame->rsi);
}

static void sys_rmdir(bsd_proc_t *proc, syscall_frame_t *frame)
{
    char path[BSD_PATH_MAX];

    sys_status_t status = copy_user_path(frame->rdi, path);
    if (status != SYS_OK) {
        frame->rax = (uint64_t)status;
        return;
    }

    frame->rax = (uint64_t)vfs_rmdir(proc, path);
}

static void sys_unlink(bsd_proc_t *proc, syscall_frame_t *frame)
{
    char path[BSD_PATH_MAX];

    sys_status_t status = copy_user_path(frame->rdi, path);
    if (status != SYS_OK) {
        frame->rax = (uint64_t)status;
        return;
    }

    frame->rax = (uint64_t)vfs_unlink(proc, path);
}

static void sys_getcwd(bsd_proc_t *proc, syscall_frame_t *frame)
{
    size_t size = (size_t)frame->rsi;
    char path[BSD_PATH_MAX];
    size_t length = 0;

    sys_status_t status = vfs_getcwd(proc, path, sizeof(path), &length);
    if (status != SYS_OK) {
        frame->rax = (uint64_t)status;
        return;
    }

    if (length + 1 > size) {
        frame->rax = (uint64_t)BSD_ERR(BSD_ERANGE);
        return;
    }

    status = copy_user_out(frame->rdi, path, length + 1);
    frame->rax = status == SYS_OK ? (uint64_t)length : (uint64_t)status;
}

static void sys_dup(bsd_proc_t *proc, syscall_frame_t *frame)
{
    int32_t fd = vfs_dup(proc, (int32_t)frame->rdi);

    frame->rax = (uint64_t)(int64_t)fd;
}

static void sys_dup2(bsd_proc_t *proc, syscall_frame_t *frame)
{
    int32_t fd = vfs_dup2(proc, (int32_t)frame->rdi, (int32_t)frame->rsi);

    frame->rax = (uint64_t)(int64_t)fd;
}

static void sys_ioctl(bsd_proc_t *proc, syscall_frame_t *frame)
{
    file_t *file = NULL;
    sys_status_t status = bsd_fd_get(proc, (int32_t)frame->rdi, &file);

    if (status != SYS_OK) {
        frame->rax = (uint64_t)BSD_ERR(BSD_EBADF);
        return;
    }

    uint64_t request = frame->rsi;

    if (file->vnode != console_vnode()) {
        vfs_file_unref(file);
        frame->rax = (uint64_t)BSD_ERR(BSD_ENOTTY);
        return;
    }

    switch (request) {
    case BSD_TIOCGWINSZ: {
        bsd_winsize_t winsize = {
            .ws_row = 25,
            .ws_col = 80,
            .ws_xpixel = 0,
            .ws_ypixel = 0,
        };
        status = copy_user_out(frame->rdx, &winsize, sizeof(winsize));
        vfs_file_unref(file);
        frame->rax = status == SYS_OK ? 0 : (uint64_t)status;
        return;
    }

    case BSD_TIOCSPGRP: {
        int32_t pgid = 0;
        status = copy_user_in(&pgid, frame->rdx, sizeof(pgid));
        vfs_file_unref(file);
        if (status != SYS_OK) {
            frame->rax = (uint64_t)status;
            return;
        }
        console_set_foreground_pgid(pgid);
        frame->rax = 0;
        return;
    }

    case BSD_TIOCGPGRP: {
        int32_t pgid = console_foreground_pgid();
        status = copy_user_out(frame->rdx, &pgid, sizeof(pgid));
        vfs_file_unref(file);
        frame->rax = status == SYS_OK ? 0 : (uint64_t)status;
        return;
    }

    default:
        vfs_file_unref(file);
        frame->rax = (uint64_t)BSD_ERR(BSD_ENOTTY);
        return;
    }
}

static void sys_pipe(bsd_proc_t *proc, syscall_frame_t *frame)
{
    int32_t fds[2] = { -1, -1 };

    sys_status_t status = vfs_pipe(proc, fds);
    if (status != SYS_OK) {
        frame->rax = (uint64_t)status;
        return;
    }

    status = copy_user_out(frame->rdi, fds, sizeof(fds));
    if (status != SYS_OK) {
        (void)bsd_fd_close(proc, fds[0]);
        (void)bsd_fd_close(proc, fds[1]);
        frame->rax = (uint64_t)status;
        return;
    }

    frame->rax = 0;
}

static void sys_kill(bsd_proc_t *proc, syscall_frame_t *frame)
{
    frame->rax =
        (uint64_t)bsd_proc_kill(proc, (int32_t)frame->rdi,
                                (uint32_t)frame->rsi);
}

static void sys_sigaction(bsd_proc_t *proc, syscall_frame_t *frame)
{
    frame->rax =
        (uint64_t)bsd_proc_sigaction(proc, (uint32_t)frame->rdi,
                                     frame->rsi, frame->rdx);
}

static void sys_sigprocmask(bsd_proc_t *proc, syscall_frame_t *frame)
{
    frame->rax =
        (uint64_t)bsd_proc_sigprocmask(proc, (int32_t)frame->rdi,
                                       frame->rsi, frame->rdx);
}

static void sys_sigreturn(bsd_proc_t *proc, syscall_frame_t *frame)
{
    frame->rax = (uint64_t)bsd_proc_sigreturn(proc, frame);
}

static void sys_setpgid(bsd_proc_t *proc, syscall_frame_t *frame)
{
    frame->rax =
        (uint64_t)bsd_proc_setpgid(proc, (int32_t)frame->rdi,
                                   (int32_t)frame->rsi);
}

static void sys_getpgid(bsd_proc_t *proc, syscall_frame_t *frame)
{
    UNUSED(proc);
    frame->rax = (uint64_t)(int64_t)bsd_proc_getpgid((int32_t)frame->rdi);
}

static void sys_arch_prctl(bsd_proc_t *proc, syscall_frame_t *frame)
{
    UNUSED(proc);
    uint64_t result = 0;
    sys_status_t status = bsd_proc_arch_prctl((int32_t)frame->rdi,
                                              frame->rsi, &result);
    if (status == SYS_OK && (int32_t)frame->rdi == BSD_ARCH_GET_FS) {
        frame->rax = result;
    } else {
        frame->rax = (uint64_t)status;
    }
}

static void sys_mprotect(bsd_proc_t *proc, syscall_frame_t *frame)
{
    frame->rax =
        (uint64_t)bsd_mprotect(proc, frame->rdi, frame->rsi,
                               (uint32_t)frame->rdx);
}

static void sys_fork(bsd_proc_t *proc, syscall_frame_t *frame)
{
    UNUSED(proc);

    frame->rax = (uint64_t)(int64_t)bsd_proc_fork(frame);
}

static void sys_execve(bsd_proc_t *proc, syscall_frame_t *frame)
{
    UNUSED(proc);

    frame->rax = (uint64_t)bsd_proc_execve(
        frame, (const char *)(uintptr_t)frame->rdi,
        (char *const *)(uintptr_t)frame->rsi,
        (char *const *)(uintptr_t)frame->rdx);
}

static void sys_wait4(bsd_proc_t *proc, syscall_frame_t *frame)
{
    UNUSED(proc);

    frame->rax = (uint64_t)(int64_t)bsd_proc_wait4(
        (int32_t)frame->rdi, frame->rsi, (int32_t)frame->rdx);
}

void bsd_syscall_dispatch(bsd_proc_t *proc, syscall_frame_t *frame)
{
    if (proc == NULL || frame == NULL) {
        return;
    }

    switch (frame->rax) {
    case BSD_SYS_EXIT:
        sys_exit(proc, frame);
        break;
    case BSD_SYS_READ:
        sys_read(proc, frame);
        break;
    case BSD_SYS_WRITE:
        sys_write(proc, frame);
        break;
    case BSD_SYS_OPEN:
        sys_open(proc, frame);
        break;
    case BSD_SYS_CLOSE:
        sys_close(proc, frame);
        break;
    case BSD_SYS_GETPID:
        sys_getpid(proc, frame);
        break;
    case BSD_SYS_GETUID:
        sys_getuid(proc, frame);
        break;
    case BSD_SYS_GETPPID:
        sys_getppid(proc, frame);
        break;
    case BSD_SYS_GETGID:
        sys_getgid(proc, frame);
        break;
    case BSD_SYS_FSTAT:
        sys_fstat(proc, frame);
        break;
    case BSD_SYS_LSEEK:
        sys_lseek(proc, frame);
        break;
    case BSD_SYS_MMAP:
        sys_mmap(proc, frame);
        break;
    case BSD_SYS_MUNMAP:
        sys_munmap(proc, frame);
        break;
    case BSD_SYS_BRK:
        sys_brk(proc, frame);
        break;
    case BSD_SYS_STAT:
        sys_stat(proc, frame);
        break;
    case BSD_SYS_GETDENTS:
        sys_getdents(proc, frame);
        break;
    case BSD_SYS_CHDIR:
        sys_chdir(proc, frame);
        break;
    case BSD_SYS_GETCWD:
        sys_getcwd(proc, frame);
        break;
    case BSD_SYS_DUP:
        sys_dup(proc, frame);
        break;
    case BSD_SYS_DUP2:
        sys_dup2(proc, frame);
        break;
    case BSD_SYS_IOCTL:
        sys_ioctl(proc, frame);
        break;
    case BSD_SYS_PIPE:
        sys_pipe(proc, frame);
        break;
    case BSD_SYS_KILL:
        sys_kill(proc, frame);
        break;
    case BSD_SYS_SIGACTION:
        sys_sigaction(proc, frame);
        break;
    case BSD_SYS_SIGPROCMASK:
        sys_sigprocmask(proc, frame);
        break;
    case BSD_SYS_SIGRETURN:
        sys_sigreturn(proc, frame);
        break;
    case BSD_SYS_SETPGID:
        sys_setpgid(proc, frame);
        break;
    case BSD_SYS_MKDIR:
        sys_mkdir(proc, frame);
        break;
    case BSD_SYS_RMDIR:
        sys_rmdir(proc, frame);
        break;
    case BSD_SYS_UNLINK:
        sys_unlink(proc, frame);
        break;
    case BSD_SYS_GETPGID:
        sys_getpgid(proc, frame);
        break;
    case BSD_SYS_ARCH_PRCTL:
        sys_arch_prctl(proc, frame);
        break;
    case BSD_SYS_MPROTECT:
        sys_mprotect(proc, frame);
        break;
    case BSD_SYS_FORK:
        sys_fork(proc, frame);
        break;
    case BSD_SYS_EXECVE:
        sys_execve(proc, frame);
        break;
    case BSD_SYS_WAIT4:
        sys_wait4(proc, frame);
        break;
    default:
        frame->rax = (uint64_t)BSD_ERR(BSD_ENOSYS);
        break;
    }
}
