#ifndef LUCYK_BSD_ERRNO_H
#define LUCYK_BSD_ERRNO_H

/* POSIX/BSD error numbers used by the BSD personality layer. They never leave
 * src/bsd/ and src/include/bsd/; microkernel code uses sys_status_t. */
#define BSD_EPERM    1
#define BSD_ENOENT   2
#define BSD_ESRCH    3
#define BSD_EINTR    4
#define BSD_EIO      5
#define BSD_E2BIG    7
#define BSD_ENOEXEC  8
#define BSD_EBADF    9
#define BSD_ECHILD   10
#define BSD_EAGAIN   11
#define BSD_ENOMEM   12
#define BSD_EACCES   13
#define BSD_EFAULT   14
#define BSD_EBUSY    16
#define BSD_EEXIST   17
#define BSD_ENOTDIR  20
#define BSD_EISDIR   21
#define BSD_EINVAL   22
#define BSD_EMFILE   24
#define BSD_ENOTTY   25
#define BSD_EFBIG    27
#define BSD_ENOSPC   28
#define BSD_ESPIPE   29
#define BSD_EROFS    30
#define BSD_EPIPE    32
#define BSD_ERANGE   34
#define BSD_ENOTEMPTY 39
#define BSD_ENAMETOOLONG 63
#define BSD_ENOSYS   78

#endif /* LUCYK_BSD_ERRNO_H */
