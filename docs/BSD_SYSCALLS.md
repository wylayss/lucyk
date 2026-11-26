# BSD_SYSCALLS.md — BSD Personality Syscall & Binary ABI

## 1. Register & Error Return Convention

All BSD syscalls enter the kernel via `SYSCALL` with **bit 63 of `rax` cleared (`0`)**, routing directly to `bsd_syscall_dispatch()` (`src/bsd/syscall_bsd.c`). Numbers are defined in `src/include/bsd/syscall_bsd.h` and mirrored in `user/bsd/bsd_user.h`.

| Register | Role | Notes |
| :--- | :--- | :--- |
| `rax` | Syscall Number (In) / Return Value (Out) | Values `0` .. `0x7FFFFFFFFFFFFFFF` |
| `rdi` | Argument 1 | System V AMD64 ABI |
| `rsi` | Argument 2 | System V AMD64 ABI |
| `rdx` | Argument 3 | System V AMD64 ABI |
| `r10` | Argument 4 | Replaces `rcx` (clobbered by `SYSCALL`) |
| `r8` | Argument 5 | System V AMD64 ABI |
| `r9` | Argument 6 | System V AMD64 ABI |

### Error Return Encoding
- **Success:** `rax >= 0` (e.g., byte count, file descriptor, `pid`, or mapped virtual address).
- **Failure:** `rax` returns a negative errno value in the range `[-4095, -1]` (e.g., `(uint64_t)-BSD_ENOENT`). The freestanding wrappers in `user/bsd/bsd_user.h` return the raw signed value; callers treat `rax < 0` as `-errno` (see `write_all` in `user/bsd/lib.c`).

---

## 2. BSD Errno Table (`src/include/bsd/errno.h`)

| Constant | Value | Meaning |
| :--- | :--- | :--- |
| `BSD_EPERM` | `1` | Operation not permitted |
| `BSD_ENOENT` | `2` | No such file or directory |
| `BSD_ESRCH` | `3` | No such process |
| `BSD_EINTR` | `4` | Interrupted system call |
| `BSD_EIO` | `5` | Input/output error |
| `BSD_E2BIG` | `7` | Argument list too long |
| `BSD_ENOEXEC` | `8` | Exec format error |
| `BSD_EBADF` | `9` | Bad file descriptor |
| `BSD_ECHILD` | `10` | No child processes |
| `BSD_EAGAIN` | `11` | Resource temporarily unavailable |
| `BSD_ENOMEM` | `12` | Cannot allocate memory |
| `BSD_EACCES` | `13` | Permission denied |
| `BSD_EFAULT` | `14` | Bad user-space address |
| `BSD_EBUSY` | `16` | Device or resource busy |
| `BSD_EEXIST` | `17` | File exists |
| `BSD_ENOTDIR` | `20` | Not a directory |
| `BSD_EISDIR` | `21` | Is a directory |
| `BSD_EINVAL` | `22` | Invalid argument |
| `BSD_EMFILE` | `24` | Too many open files in process |
| `BSD_ENOTTY` | `25` | Inappropriate ioctl for device |
| `BSD_EFBIG` | `27` | File too large |
| `BSD_ENOSPC` | `28` | No space left on device |
| `BSD_ESPIPE` | `29` | Illegal seek (e.g., on `/dev/console`) |
| `BSD_EROFS` | `30` | Read-only filesystem |
| `BSD_EPIPE` | `32` | Broken pipe |
| `BSD_ERANGE` | `34` | Result too large |
| `BSD_ENOTEMPTY` | `39` | Directory not empty |
| `BSD_ENAMETOOLONG` | `63` | File name too long |
| `BSD_ENOSYS` | `78` | Function not implemented |

`BSD_ERR(e)` (in `src/include/bsd/bsd.h`) expands to `(-(int64_t)(e))`, the negative return
value carried in `rax`.

---

## 3. BSD Syscall Number Table

Numbers are from `src/include/bsd/syscall_bsd.h` / `user/bsd/bsd_user.h`; handlers are in
`src/bsd/syscall_bsd.c` (dispatcher) and the corresponding subsystem files.

| Syscall Constant | Number (`rax`) | Signature | Implemented in |
| :--- | :--- | :--- | :--- |
| `BSD_SYS_EXIT` | `1` | `void exit(int status)` | `syscall_bsd.c` / `proc.c` |
| `BSD_SYS_FORK` | `2` | `int32_t fork(void)` | `proc.c` |
| `BSD_SYS_READ` | `3` | `ssize_t read(int fd, void *buf, size_t count)` | `syscall_bsd.c` / `vfs.c` |
| `BSD_SYS_WRITE` | `4` | `ssize_t write(int fd, const void *buf, size_t count)` | `syscall_bsd.c` / `vfs.c` |
| `BSD_SYS_OPEN` | `5` | `int open(const char *path, int flags, uint32_t mode)` | `syscall_bsd.c` / `vfs.c` |
| `BSD_SYS_CLOSE` | `6` | `int close(int fd)` | `syscall_bsd.c` / `vfs.c` |
| `BSD_SYS_WAIT4` | `7` | `int32_t wait4(int32_t pid, int *status, int options, void *rusage)` | `proc.c` |
| `BSD_SYS_CHDIR` | `12` | `int chdir(const char *path)` | `vfs.c` |
| `BSD_SYS_GETPID` | `20` | `int32_t getpid(void)` | `proc.c` |
| `BSD_SYS_GETUID` | `24` | `uint32_t getuid(void)` | `proc.c` |
| `BSD_SYS_KILL` | `37` | `int kill(int32_t pid, int sig)` | `proc.c` |
| `BSD_SYS_GETPPID` | `39` | `int32_t getppid(void)` | `proc.c` |
| `BSD_SYS_DUP` | `41` | `int dup(int oldfd)` | `vfs.c` |
| `BSD_SYS_PIPE` | `42` | `int pipe(int fds[2])` | `vfs.c` |
| `BSD_SYS_SIGACTION` | `46` | `int sigaction(int sig, const bsd_sigaction_t *act, bsd_sigaction_t *oldact)` | `proc.c` (Stage B/C) |
| `BSD_SYS_GETGID` | `47` | `uint32_t getgid(void)` | `proc.c` |
| `BSD_SYS_SIGPROCMASK` | `48` | `int sigprocmask(int how, const uint64_t *set, uint64_t *oldset)` | `proc.c` (Stage B/C) |
| `BSD_SYS_IOCTL` | `54` | `int ioctl(int fd, uint64_t req, void *arg)` | `syscall_bsd.c` / `console.c` |
| `BSD_SYS_EXECVE` | `59` | `int execve(const char *path, char *const argv[], char *const envp[])` | `proc.c` |
| `BSD_SYS_MUNMAP` | `73` | `int munmap(void *addr, size_t len)` | `mmap.c` |
| `BSD_SYS_MPROTECT` | `74` | `int mprotect(void *addr, size_t len, int prot)` | `mmap.c` (Stage C) |
| `BSD_SYS_SETPGID` | `82` | `int setpgid(int32_t pid, int32_t pgid)` | `proc.c` (Stage B) |
| `BSD_SYS_MKDIR` | `83` | `int mkdir(const char *path, uint32_t mode)` | `vfs.c` / `ext2.c` (Stage D) |
| `BSD_SYS_RMDIR` | `84` | `int rmdir(const char *path)` | `vfs.c` / `ext2.c` (Stage D) |
| `BSD_SYS_UNLINK` | `87` | `int unlink(const char *path)` | `vfs.c` / `ext2.c` (Stage D) |
| `BSD_SYS_DUP2` | `90` | `int dup2(int oldfd, int newfd)` | `vfs.c` |
| `BSD_SYS_SIGRETURN` | `103` | `int sigreturn(bsd_sigframe_t *frame)` | `proc.c` (Stage B/C) |
| `BSD_SYS_ARCH_PRCTL` | `158` | `int arch_prctl(int code, uint64_t addr)` | `proc.c` (Stage C) |
| `BSD_SYS_FSTAT` | `189` | `int fstat(int fd, bsd_stat_t *st)` | `syscall_bsd.c` / `vfs.c` |
| `BSD_SYS_STAT` | `190` | `int stat(const char *path, bsd_stat_t *st)` | `vfs.c` |
| `BSD_SYS_GETDENTS` | `196` | `ssize_t getdents(int fd, void *buf, size_t count)` | `syscall_bsd.c` / `vfs.c` |
| `BSD_SYS_MMAP` | `197` | `void *mmap(void *addr, size_t len, int prot, int flags, int fd, int64_t off)` | `mmap.c` |
| `BSD_SYS_LSEEK` | `199` | `int64_t lseek(int fd, int64_t offset, int whence)` | `vfs.c` |
| `BSD_SYS_GETPGID` | `207` | `int32_t getpgid(int32_t pid)` | `proc.c` (Stage B) |
| `BSD_SYS_BRK` | `214` | `vaddr_t brk(vaddr_t new_brk)` | `mmap.c` |
| `BSD_SYS_GETCWD` | `326` | `ssize_t getcwd(char *buf, size_t size)` | `vfs.c` |

An unrecognized number returns `BSD_ERR(BSD_ENOSYS)` (`-78`). `read`/`write`/`getdents`
transfer at most `BSD_IO_CHUNK` (`4096`) bytes per call.

### 3.1 `kill` Process-Group Semantics (`BSD_SYS_KILL`, `src/bsd/proc.c`)
- `pid > 0`: signal the single process `pid` (`BSD_ESRCH` if absent).
- `pid == 0`: signal **the caller's own process group** (`caller->pgid`).
- `pid < -1`: signal **the process group `-pid`**.
- `pid == -1`: not supported; returns `BSD_ESRCH`.
- `sig == 0`: existence probe only (no signal queued).
- Signals become `pending_signals` on the target and are delivered on that target's next
  syscall return. `SIGKILL` and `SIGSTOP` cannot be blocked, caught, or ignored.

### 3.2 `ioctl` Requests (`BSD_SYS_IOCTL`, `console.c`)
Valid only on the `/dev/console` vnode; any other vnode returns `BSD_ENOTTY`.

| Request | Value | Direction | Behavior |
| :--- | :--- | :--- | :--- |
| `BSD_TIOCGWINSZ` | `0x5413` | out | Writes a `bsd_winsize_t` (25 rows x 80 cols) |
| `BSD_TIOCSPGRP` | `0x5410` | in | Sets the console foreground process group from an `int32_t` |
| `BSD_TIOCGPGRP` | `0x540F` | out | Reads the console foreground process group as an `int32_t` |

### 3.3 `arch_prctl` Commands (`BSD_SYS_ARCH_PRCTL`)
| Command | Value | Behavior |
| :--- | :--- | :--- |
| `BSD_ARCH_SET_FS` | `0x1002` | Sets the calling thread's TLS base and writes `IA32_FS_BASE` |
| `BSD_ARCH_GET_FS` | `0x1003` | Returns the calling thread's TLS base in `rax` |

---

## 4. `open` Flags (`BSD_SYS_OPEN`, `src/bsd/vfs.c`)

```c
#define BSD_O_RDONLY    0x0000
#define BSD_O_WRONLY    0x0001
#define BSD_O_RDWR      0x0002
#define BSD_O_ACCMODE   0x0003
#define BSD_O_APPEND    0x0008
#define BSD_O_CREAT     0x0200
#define BSD_O_TRUNC     0x0400
#define BSD_O_DIRECTORY 0x00200000

#define BSD_SEEK_SET    0
#define BSD_SEEK_CUR    1
#define BSD_SEEK_END    2
```

- **`O_CREAT`:** if the path does not exist, the parent vnode's `create` op is invoked
  with `mode`. A create race that loses to another CPU (`BSD_EEXIST`) re-opens the winner.
- **`O_TRUNC`:** for a non-directory opened writable, the vnode's `truncate` op is called
  before the file description is installed.
- **`O_APPEND`:** each `write` on a regular file re-derives the offset from the file size
  so data is appended atomically (`vfs_write` under `file->lock`).
- Opening a directory with non-`O_RDONLY` access returns `BSD_EISDIR`; `O_DIRECTORY` on a
  non-directory returns `BSD_ENOTDIR`.

---

## 5. User-Space ABI Structures & Flags

### 5.1 `bsd_stat_t` & File Modes
```c
#define BSD_S_IFMT      0170000U
#define BSD_S_IFDIR     0040000U
#define BSD_S_IFCHR     0020000U
#define BSD_S_IFREG     0100000U

typedef struct bsd_stat {
    uint32_t st_mode;
    uint32_t st_nlink;
    uint32_t st_uid;
    uint32_t st_gid;
    uint64_t st_ino;
    uint64_t st_size;
    uint64_t st_blksize;
    uint64_t st_blocks;
} bsd_stat_t;               /* sizeof == 48 */
```

### 5.2 `bsd_dirent_t` (`BSD_SYS_GETDENTS`)
```c
typedef struct bsd_dirent {
    uint64_t d_ino;
    uint16_t d_reclen;
    uint8_t  d_type;    /* 2 = DT_CHR, 4 = DT_DIR, 8 = DT_REG */
    uint8_t  d_namlen;
    char     d_name[256];
} bsd_dirent_t;             /* sizeof == 272 */
```

### 5.3 `mmap` Protection & Flags (`W^X` Enforced)
```c
#define BSD_PROT_NONE   0x0
#define BSD_PROT_READ   0x1
#define BSD_PROT_WRITE  0x2
#define BSD_PROT_EXEC   0x4  /* PROT_WRITE | PROT_EXEC is rejected with BSD_EINVAL */

#define BSD_MAP_SHARED  0x01
#define BSD_MAP_PRIVATE 0x02
#define BSD_MAP_FIXED   0x10
#define BSD_MAP_ANON    0x20
#define BSD_MAP_FAILED  ((void *)-1)
```
Anonymous private mappings live in `[BSD_MMAP_BASE, BSD_MMAP_LIMIT)`; the data break is
bounded by `BSD_BRK_LIMIT`. `mmap` requires `MAP_ANON | MAP_PRIVATE` and rejects
`MAP_SHARED`, `PROT_WRITE | PROT_EXEC`, and inconsistent flag combinations.

### 5.4 Signal ABI (`BSD_SYS_SIGACTION` / `SIGPROCMASK` / `SIGRETURN`)
```c
#define BSD_SIG_DFL  ((uint64_t)0)
#define BSD_SIG_IGN  ((uint64_t)1)
#define BSD_SA_RESTORER 0x04000000U

#define BSD_SIG_BLOCK   1
#define BSD_SIG_UNBLOCK 2
#define BSD_SIG_SETMASK 3

typedef struct bsd_sigaction {
    uint64_t sa_handler;  /* SIG_DFL, SIG_IGN, or Ring 3 handler address */
    uint64_t sa_mask;     /* Additional signals blocked during the handler */
    uint32_t sa_flags;
    uint32_t _pad;
    uint64_t sa_restorer; /* Ring 3 trampoline that calls BSD_SYS_SIGRETURN */
} bsd_sigaction_t;
```
The user-stack `bsd_sigframe_t` layout is documented in
[`BSD_PERSONALITY.md`](BSD_PERSONALITY.md) §5.

---

## 6. User Pointer Marshalling Rules

Every syscall accepting a user-space pointer (`buf`, `path`, `st`, `argv`, `envp`) MUST:
1. Validate user range bounds using microkernel `copy_from_user()` / `copy_to_user()` (never dereference user virtual addresses directly in Ring 0).
2. Convert microkernel `SYS_ERR_INVAL` on pointer copy failures into `-BSD_EFAULT`.
3. Bound string lengths (`BSD_PATH_MAX = 256`, `BSD_ARG_MAX = 4096`) and transfer chunks (`BSD_IO_CHUNK = 4096` bytes per iteration) so Ring 0 stack usage stays bounded and predictable.
