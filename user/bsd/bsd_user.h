#ifndef LUCYK_BSD_USER_H
#define LUCYK_BSD_USER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* BSD syscall numbers. */
#define BSD_SYS_EXIT     1
#define BSD_SYS_FORK     2
#define BSD_SYS_READ     3
#define BSD_SYS_WRITE    4
#define BSD_SYS_OPEN     5
#define BSD_SYS_CLOSE    6
#define BSD_SYS_WAIT4    7
#define BSD_SYS_CHDIR    12
#define BSD_SYS_GETPID   20
#define BSD_SYS_GETUID   24
#define BSD_SYS_KILL     37
#define BSD_SYS_GETPPID  39
#define BSD_SYS_DUP      41
#define BSD_SYS_PIPE     42
#define BSD_SYS_SIGACTION  46
#define BSD_SYS_GETGID   47
#define BSD_SYS_SIGPROCMASK 48
#define BSD_SYS_IOCTL    54
#define BSD_SYS_EXECVE   59
#define BSD_SYS_MUNMAP   73
#define BSD_SYS_MPROTECT 74
#define BSD_SYS_SETPGID  82
#define BSD_SYS_MKDIR    83
#define BSD_SYS_RMDIR    84
#define BSD_SYS_UNLINK   87
#define BSD_SYS_DUP2     90
#define BSD_SYS_SIGRETURN 103
#define BSD_SYS_ARCH_PRCTL 158
#define BSD_SYS_FSTAT    189
#define BSD_SYS_STAT     190
#define BSD_SYS_GETDENTS 196
#define BSD_SYS_MMAP     197
#define BSD_SYS_LSEEK    199
#define BSD_SYS_GETPGID  207
#define BSD_SYS_BRK      214
#define BSD_SYS_GETCWD   326

/* Native microkernel sleep, used for shell/idle delays. */
#define SYS_THREAD_SLEEP_MS 0x8000000000000003ULL

#define BSD_WNOHANG 1
#define BSD_PATH_MAX 256

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

#define BSD_SIG_DFL  ((uint64_t)0)
#define BSD_SIG_IGN  ((uint64_t)1)

#define BSD_SA_RESTORER 0x04000000U

#define BSD_SIG_BLOCK   1
#define BSD_SIG_UNBLOCK 2
#define BSD_SIG_SETMASK 3

#define BSD_ARCH_SET_FS 0x1002
#define BSD_ARCH_GET_FS 0x1003

#define BSD_PROT_NONE  0x0U
#define BSD_PROT_READ  0x1U
#define BSD_PROT_WRITE 0x2U
#define BSD_PROT_EXEC  0x4U

#define BSD_MAP_SHARED  0x01U
#define BSD_MAP_PRIVATE 0x02U
#define BSD_MAP_FIXED   0x10U
#define BSD_MAP_ANON    0x20U

#define BSD_TIOCSPGRP 0x5410U
#define BSD_TIOCGPGRP 0x540FU

#define BSD_ESRCH    3
#define BSD_ENOENT   2
#define BSD_EINTR    4
#define BSD_EIO      5
#define BSD_E2BIG    7
#define BSD_ENOEXEC  8
#define BSD_EBADF    9
#define BSD_ECHILD   10
#define BSD_ENOMEM   12
#define BSD_EACCES   13
#define BSD_EFAULT   14
#define BSD_EEXIST   17
#define BSD_EINVAL   22
#define BSD_ENOTTY   25
#define BSD_ENOSPC   28
#define BSD_ESPIPE   29
#define BSD_EPIPE    32
#define BSD_ERANGE   34
#define BSD_ENOTEMPTY 39
#define BSD_ENAMETOOLONG 63

#define BSD_O_RDONLY 0x0000U
#define BSD_O_WRONLY 0x0001U
#define BSD_O_RDWR   0x0002U
#define BSD_O_APPEND 0x0008U
#define BSD_O_CREAT  0x0200U
#define BSD_O_TRUNC  0x0400U

#define BSD_S_IFMT  0170000U
#define BSD_S_IFDIR 0040000U
#define BSD_S_IFCHR 0020000U
#define BSD_S_IFREG 0100000U

#define BSD_TIOCGWINSZ 0x5413U

typedef struct bsd_sigaction {
    uint64_t sa_handler;
    uint64_t sa_mask;
    uint32_t sa_flags;
    uint32_t _pad;
    uint64_t sa_restorer;
} bsd_sigaction_t;

typedef struct bsd_sigframe {
    uint64_t restorer;
    uint32_t signo;
    uint32_t _pad0;
    uint64_t saved_mask;
    uint64_t rax;
    uint64_t rdi;
    uint64_t rsi;
    uint64_t rdx;
    uint64_t r10;
    uint64_t r8;
    uint64_t r9;
    uint64_t rcx;
    uint64_t r11;
    uint64_t rsp;
    uint64_t r15;
    uint64_t r14;
    uint64_t r13;
    uint64_t r12;
    uint64_t rbx;
    uint64_t rbp;
    uint64_t _pad1;
} bsd_sigframe_t;

typedef struct bsd_stat {
    uint32_t st_mode;
    uint32_t st_nlink;
    uint32_t st_uid;
    uint32_t st_gid;
    uint64_t st_ino;
    uint64_t st_size;
    uint64_t st_blksize;
    uint64_t st_blocks;
} bsd_stat_t;

typedef struct bsd_dirent {
    uint64_t d_ino;
    uint16_t d_reclen;
    uint8_t  d_type;
    uint8_t  d_namlen;
    char     d_name[256];
} bsd_dirent_t;

typedef struct bsd_winsize {
    uint16_t ws_row;
    uint16_t ws_col;
    uint16_t ws_xpixel;
    uint16_t ws_ypixel;
} bsd_winsize_t;

_Static_assert(sizeof(bsd_stat_t) == 48, "bsd_stat_t ABI size");
_Static_assert(sizeof(bsd_dirent_t) == 272, "bsd_dirent_t ABI size");
_Static_assert(sizeof(bsd_winsize_t) == 8, "bsd_winsize_t ABI size");

static inline int64_t bsd_syscall(uint64_t number, uint64_t arg1,
                                  uint64_t arg2, uint64_t arg3)
{
    register uint64_t rdi __asm__("rdi") = arg1;
    register uint64_t rsi __asm__("rsi") = arg2;
    register uint64_t rdx __asm__("rdx") = arg3;
    register uint64_t r10 __asm__("r10") = 0;
    register uint64_t r8 __asm__("r8") = 0;
    register uint64_t r9 __asm__("r9") = 0;
    uint64_t result;

    __asm__ volatile("syscall"
                     : "=a"(result)
                     : "a"(number), "r"(rdi), "r"(rsi), "r"(rdx),
                       "r"(r10), "r"(r8), "r"(r9)
                     : "rcx", "r11", "cc", "memory");

    return (int64_t)result;
}

static inline int64_t bsd_syscall6(uint64_t number, uint64_t arg1,
                                   uint64_t arg2, uint64_t arg3,
                                   uint64_t arg4, uint64_t arg5,
                                   uint64_t arg6)
{
    register uint64_t rdi __asm__("rdi") = arg1;
    register uint64_t rsi __asm__("rsi") = arg2;
    register uint64_t rdx __asm__("rdx") = arg3;
    register uint64_t r10 __asm__("r10") = arg4;
    register uint64_t r8 __asm__("r8") = arg5;
    register uint64_t r9 __asm__("r9") = arg6;
    uint64_t result;

    __asm__ volatile("syscall"
                     : "=a"(result)
                     : "a"(number), "r"(rdi), "r"(rsi), "r"(rdx),
                       "r"(r10), "r"(r8), "r"(r9)
                     : "rcx", "r11", "cc", "memory");

    return (int64_t)result;
}

static inline void raw_sleep_ms(uint64_t ms)
{
    (void)bsd_syscall(SYS_THREAD_SLEEP_MS, ms, 0, 0);
}

static inline int64_t bsd_fork(void)
{
    return bsd_syscall(BSD_SYS_FORK, 0, 0, 0);
}

static inline int64_t bsd_execve(const char *path, char *const argv[],
                                 char *const envp[])
{
    return bsd_syscall(BSD_SYS_EXECVE, (uint64_t)(uintptr_t)path,
                       (uint64_t)(uintptr_t)argv,
                       (uint64_t)(uintptr_t)envp);
}

static inline int64_t bsd_wait4(int32_t pid, int32_t *status, int32_t options)
{
    return bsd_syscall(BSD_SYS_WAIT4, (uint64_t)(int64_t)pid,
                       (uint64_t)(uintptr_t)status,
                       (uint64_t)(int64_t)options);
}

static inline int64_t bsd_read(int32_t fd, void *buf, size_t count)
{
    return bsd_syscall(BSD_SYS_READ, (uint64_t)(int64_t)fd,
                       (uint64_t)(uintptr_t)buf, count);
}

static inline int64_t bsd_write(int32_t fd, const void *buf, size_t count)
{
    return bsd_syscall(BSD_SYS_WRITE, (uint64_t)(int64_t)fd,
                       (uint64_t)(uintptr_t)buf, count);
}

static inline int64_t bsd_open(const char *path, uint32_t flags, uint32_t mode)
{
    return bsd_syscall(BSD_SYS_OPEN, (uint64_t)(uintptr_t)path, flags, mode);
}

static inline int64_t bsd_close(int32_t fd)
{
    return bsd_syscall(BSD_SYS_CLOSE, (uint64_t)(int64_t)fd, 0, 0);
}

static inline int64_t bsd_mkdir(const char *path, uint32_t mode)
{
    return bsd_syscall(BSD_SYS_MKDIR, (uint64_t)(uintptr_t)path, mode, 0);
}

static inline int64_t bsd_rmdir(const char *path)
{
    return bsd_syscall(BSD_SYS_RMDIR, (uint64_t)(uintptr_t)path, 0, 0);
}

static inline int64_t bsd_unlink(const char *path)
{
    return bsd_syscall(BSD_SYS_UNLINK, (uint64_t)(uintptr_t)path, 0, 0);
}

static inline int64_t bsd_pipe(int32_t fds[2])
{
    return bsd_syscall(BSD_SYS_PIPE, (uint64_t)(uintptr_t)fds, 0, 0);
}

static inline int64_t bsd_chdir(const char *path)
{
    return bsd_syscall(BSD_SYS_CHDIR, (uint64_t)(uintptr_t)path, 0, 0);
}

static inline int64_t bsd_getcwd(char *buf, size_t size)
{
    return bsd_syscall(BSD_SYS_GETCWD, (uint64_t)(uintptr_t)buf, size, 0);
}

static inline int64_t bsd_getdents(int32_t fd, void *buf, size_t count)
{
    return bsd_syscall(BSD_SYS_GETDENTS, (uint64_t)(int64_t)fd,
                       (uint64_t)(uintptr_t)buf, count);
}

static inline int64_t bsd_getpid(void)
{
    return bsd_syscall(BSD_SYS_GETPID, 0, 0, 0);
}

static inline int64_t bsd_getppid(void)
{
    return bsd_syscall(BSD_SYS_GETPPID, 0, 0, 0);
}

static inline int64_t bsd_getuid(void)
{
    return bsd_syscall(BSD_SYS_GETUID, 0, 0, 0);
}

static inline int64_t bsd_getgid(void)
{
    return bsd_syscall(BSD_SYS_GETGID, 0, 0, 0);
}

static inline int64_t bsd_kill(int32_t pid, uint32_t signo)
{
    return bsd_syscall(BSD_SYS_KILL, (uint64_t)(int64_t)pid, signo, 0);
}

static inline int64_t bsd_stat(const char *path, bsd_stat_t *st)
{
    return bsd_syscall(BSD_SYS_STAT, (uint64_t)(uintptr_t)path,
                       (uint64_t)(uintptr_t)st, 0);
}

static inline int64_t bsd_fstat(int32_t fd, bsd_stat_t *st)
{
    return bsd_syscall(BSD_SYS_FSTAT, (uint64_t)(int64_t)fd,
                       (uint64_t)(uintptr_t)st, 0);
}

static inline int64_t bsd_lseek(int32_t fd, int64_t offset, int32_t whence)
{
    return bsd_syscall(BSD_SYS_LSEEK, (uint64_t)(int64_t)fd,
                       (uint64_t)offset, (uint64_t)(int64_t)whence);
}

static inline int64_t bsd_ioctl(int32_t fd, uint64_t request, void *arg)
{
    return bsd_syscall(BSD_SYS_IOCTL, (uint64_t)(int64_t)fd, request,
                       (uint64_t)(uintptr_t)arg);
}

static inline int64_t bsd_sigaction(uint32_t signo, const bsd_sigaction_t *act,
                                    bsd_sigaction_t *oldact)
{
    return bsd_syscall(BSD_SYS_SIGACTION, signo,
                       (uint64_t)(uintptr_t)act,
                       (uint64_t)(uintptr_t)oldact);
}

static inline int64_t bsd_sigprocmask(int32_t how, const uint64_t *set,
                                      uint64_t *oldset)
{
    return bsd_syscall(BSD_SYS_SIGPROCMASK, (uint64_t)(int64_t)how,
                       (uint64_t)(uintptr_t)set,
                       (uint64_t)(uintptr_t)oldset);
}

static inline int64_t bsd_sigreturn(bsd_sigframe_t *frame)
{
    return bsd_syscall(BSD_SYS_SIGRETURN, (uint64_t)(uintptr_t)frame, 0, 0);
}

static inline int64_t bsd_setpgid(int32_t pid, int32_t pgid)
{
    return bsd_syscall(BSD_SYS_SETPGID, (uint64_t)(int64_t)pid,
                       (uint64_t)(int64_t)pgid, 0);
}

static inline int64_t bsd_getpgid(int32_t pid)
{
    return bsd_syscall(BSD_SYS_GETPGID, (uint64_t)(int64_t)pid, 0, 0);
}

static inline int64_t bsd_arch_prctl(int32_t code, uint64_t addr)
{
    return bsd_syscall(BSD_SYS_ARCH_PRCTL, (uint64_t)(int64_t)code, addr, 0);
}

static inline int64_t bsd_mprotect(void *addr, size_t len, uint32_t prot)
{
    return bsd_syscall(BSD_SYS_MPROTECT, (uint64_t)(uintptr_t)addr, len, prot);
}

static inline int64_t bsd_mmap(void *addr, size_t len, uint32_t prot,
                               uint32_t flags, int32_t fd, int64_t offset)
{
    return bsd_syscall6(BSD_SYS_MMAP, (uint64_t)(uintptr_t)addr, len, prot,
                        flags, (uint64_t)(int64_t)fd, (uint64_t)offset);
}

static inline int64_t bsd_munmap(void *addr, size_t len)
{
    return bsd_syscall(BSD_SYS_MUNMAP, (uint64_t)(uintptr_t)addr, len, 0);
}

/* Shared freestanding runtime implemented in user/bsd/lib.c. */
__attribute__((noreturn)) void user_exit(uint64_t code);
size_t text_len(const char *text);
void write_all(int32_t fd, const char *text, size_t length);
void write_text(int32_t fd, const char *text);
void write_unsigned(int32_t fd, uint64_t value);
void write_signed(int32_t fd, int64_t value);
bool str_eq(const char *a, const char *b);
bool str_eq_len(const char *a, const char *b, size_t length);
bool parse_int(const char *text, int64_t *value);

#endif /* LUCYK_BSD_USER_H */
