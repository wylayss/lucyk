#ifndef LUCYK_BSD_SYSCALL_BSD_H
#define LUCYK_BSD_SYSCALL_BSD_H

#include <bsd/bsd.h>
#include <bsd/proc.h>
#include <syscall.h>

/* BSD personality syscalls (bit 63 of rax cleared). */
#define BSD_SYS_EXIT      1
#define BSD_SYS_FORK      2
#define BSD_SYS_READ      3
#define BSD_SYS_WRITE     4
#define BSD_SYS_OPEN      5
#define BSD_SYS_CLOSE     6
#define BSD_SYS_WAIT4     7
#define BSD_SYS_CHDIR     12
#define BSD_SYS_GETPID    20
#define BSD_SYS_GETUID    24
#define BSD_SYS_KILL      37
#define BSD_SYS_GETPPID   39
#define BSD_SYS_DUP       41
#define BSD_SYS_PIPE      42
#define BSD_SYS_SIGACTION  46
#define BSD_SYS_GETGID    47
#define BSD_SYS_SIGPROCMASK 48
#define BSD_SYS_IOCTL     54
#define BSD_SYS_EXECVE    59
#define BSD_SYS_MUNMAP    73
#define BSD_SYS_MPROTECT  74
#define BSD_SYS_SETPGID   82
#define BSD_SYS_MKDIR     83
#define BSD_SYS_RMDIR     84
#define BSD_SYS_UNLINK    87
#define BSD_SYS_DUP2      90
#define BSD_SYS_SIGRETURN 103
#define BSD_SYS_ARCH_PRCTL 158
#define BSD_SYS_FSTAT     189
#define BSD_SYS_STAT      190
#define BSD_SYS_GETDENTS  196
#define BSD_SYS_MMAP      197
#define BSD_SYS_LSEEK     199
#define BSD_SYS_GETPGID   207
#define BSD_SYS_BRK       214
#define BSD_SYS_GETCWD    326

/* Dispatches one BSD syscall. frame->rax holds the number on entry and the
 * result on exit. */
void bsd_syscall_dispatch(bsd_proc_t *proc, syscall_frame_t *frame);

#endif /* LUCYK_BSD_SYSCALL_BSD_H */
