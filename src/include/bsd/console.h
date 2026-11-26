#ifndef LUCYK_BSD_CONSOLE_H
#define LUCYK_BSD_CONSOLE_H

#include <bsd/vfs.h>

/* Initializes the COM1-backed /dev/console character device. */
void console_init(void);

/* Permanent console vnode bound at /dev/console by bsd_init(). */
vnode_t *console_vnode(void);

/* Foreground process group that receives SIGINT for Ctrl+C. */
void console_set_foreground_pgid(int32_t pgid);
int32_t console_foreground_pgid(void);

/* Legacy single-PID foreground (deprecated; delegates to pgid). */
void console_set_foreground(int32_t pid);
int32_t console_foreground(void);

/* TIOCSPGRP / TIOCGPGRP ioctl request codes. */
#define BSD_TIOCSPGRP 0x5410U
#define BSD_TIOCGPGRP 0x540FU

#endif /* LUCYK_BSD_CONSOLE_H */
