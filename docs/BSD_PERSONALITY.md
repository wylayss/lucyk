# BSD_PERSONALITY.md — BSD Subsystem Architecture & Microkernel Boundary

## 1. Architectural Overview

The **BSD Personality Layer** implements a POSIX/BSD execution environment (processes, file descriptors, signals, a Virtual File System, a TTY console, and memory mapping) on top of the **LucyK x86_64 Microkernel**.

Following a **Co-located Hybrid-Ready Topology**, the BSD subsystem executes in Ring 0 (`src/bsd/`) and attaches to the microkernel core exclusively through the registered `mk_hooks_t` callback interface and the public microkernel primitive APIs (`task.h`, `vspace.h`, `cap.h`, `ipc.h`, `elf.h`). It reaches hardware and storage through the personality-neutral `src/io/` layer (`pci.h`, `blkdev.h`), never the reverse.

```text
+-------------------------------------------------------------------+
|                        User Space (Ring 3)                        |
|        /bin/init (PID 1)   |   /bin/sh   |   BSD Binaries         |
+------------------------------------+------------------------------+
                                     |  (SYSCALL with rax[63] == 0)
=====================================|============================== (Ring 0)
                                     v
+-------------------------------------------------------------------+
|               BSD Personality Layer (src/bsd/)                    |
|  - Process & Signal Manager (bsd_proc_t, pid/ppid/pgid, wait4)    |
|  - Virtual File System (vnode_t, file_t, fd_table[64])            |
|  - Root Filesystems (writable ext2 at /, read-only USTAR fallback)|
|  - /dev/console TTY (COM1, foreground process group)              |
|  - BSD Syscall Dispatcher & User Memory Marshalling               |
+------------------------------------+------------------------------+
                                     |  (mk_hooks_t & Core C API)
                                     v
+-------------------------------------------------------------------+
|                   LucyK Microkernel Core (src/mk/)                |
|   task_t  |  thread_t  |  vspace_t  |  cap_table_t  |  endpoint_t |
+-------------------------------------------------------------------+
```

---

## 2. Strict Boundary & Hygiene Rules

1. **Zero Reverse Pollution:**
   - Files in `src/mk/`, `src/arch/`, `src/io/`, and `src/include/` (outside `src/include/bsd/`) MUST NEVER include `<bsd/...>` headers or reference `bsd_proc_t`, `pid`, `fd`, `vnode`, or `errno`.
   - Any low-level capability needed by the BSD layer (such as `vspace_clone()` for `fork()`) is exposed as a clean, generic microkernel primitive in `src/arch/x86_64/vspace.c` or `src/mk/`.
2. **Zero Mach Terminology:**
   - Continue enforcing `AGENT.md` prohibitions: never use `mach_*`, `pmap`, `vm_map`, `kern_return_t`, or Mach port terminology anywhere in `src/bsd/` or `user/`.
3. **Concurrency & Locking Discipline:**
   - All global BSD tables (`proc_table`, PID allocator, mount table, console input ring buffer) are guarded with `irq_save()` / `irq_restore()`.
   - Never invoke blocking VFS/IPC operations while holding an `irq_save()` spinlock.
   - Respect the storage lock hierarchy `file -> ext2 -> blk` (see [`ARCHITECTURE.md`](ARCHITECTURE.md) §5.2).

---

## 3. Directory Layout

```text
src/include/bsd/
├── bsd.h            # bsd_init() entry point, size limits, open flags, stat/dirent ABI
├── errno.h          # POSIX/BSD errno constants (BSD_ENOENT, BSD_EINVAL, ...)
├── proc.h           # bsd_proc_t, signal definitions, bsd_sigframe_t
├── vfs.h            # vnode_t, vnode_ops_t, file_t, mount structures
├── mmap.h           # anonymous mmap/munmap/brk/mprotect prototypes
├── console.h        # /dev/console TTY interface & TIOC* ioctl codes
├── ext2.h           # writable ext2 mount entry point
└── syscall_bsd.h    # BSD syscall numbers & dispatch prototype

src/bsd/
├── bsd_init.c       # Registers mk_hooks_t, mounts root, spawns PID 1
├── proc.c           # Process lifecycle, PID/pgid, signals, fork/execve/wait4
├── vfs.c            # Path resolution, fd_table management, file ops, pipes
├── ext2.c           # Writable ext2 filesystem driver (blocks, inodes, dirs)
├── initrd.c         # Read-only USTAR (tar) archive fallback root
├── console.c        # COM1-backed /dev/console character device
├── mmap.c           # Anonymous mmap/munmap, brk heap, mprotect
└── syscall_bsd.c    # Top-level on_personality_syscall dispatcher

user/bsd/            # Freestanding BSD Ring 3 programs (/bin/init, /bin/sh, /bin/echo)
```

`vfs_init()` mounts the writable `ext2` root at `/` when the VirtIO-Blk device is present.
If no disk exists or the mount fails, `bsd_init()` falls back to the Limine `initrd.tar`
module (read-only USTAR). `bsd_init()` then binds `/dev` and `/dev/console`, registers the
hooks, and starts PID 1 (`/bin/init`).

---

## 4. Microkernel Hook Integration (`mk_hooks_t`)

`kernel_main()` calls `bsd_init()` after kernel initialization completes. `bsd_init()`
registers the BSD callbacks via `mk_register_hooks()`:

1. **`on_personality_syscall(thread_t *caller, syscall_frame_t *frame)`**
   - Invoked whenever a Ring 3 thread executes `SYSCALL` with `rax[63] == 0`.
   - Resolves `bsd_proc_t *p = bsd_proc_from_task(caller->owner_task)`. If `p == NULL` or not `PROC_RUNNING` (e.g., a raw microkernel task), sets `frame->rax = (uint64_t)-BSD_ENOSYS`.
   - Otherwise calls `bsd_syscall_dispatch(proc, frame)`, then, if the process is still running, delivers any pending unmasked BSD signals before returning to Ring 3.
2. **`on_user_exception(thread_t *faulting_thread, uint64_t vector, uint64_t error_code, uint64_t fault_addr)`**
   - Translates hardware CPU faults in Ring 3 into standard BSD signals (`bsd_signal_for_vector`):
     - Vector `0` (`#DE`) & `16`/`19` (FP/SIMD) -> `BSD_SIGFPE`
     - Vector `1` (`#DB`) & `3` (`#BP`) -> `BSD_SIGTRAP`
     - Vector `6` (`#UD`) -> `BSD_SIGILL`
     - Vector `13` (`#GP`) & `17` (`#AC`) -> `BSD_SIGBUS`
     - Vector `14` (`#PF`), default -> `BSD_SIGSEGV`
   - Terminates the faulting process with exit status `128 + signo` via `bsd_proc_signal_exit()` and `task_terminate()`, returning `true` so the microkernel never panics.
3. **`on_task_exit(task_t *task, int64_t exit_status)`**
   - Invoked when the last thread in `task` terminates.
   - If the process is not already a zombie, transitions it via `bsd_proc_exit()`, then sends `BSD_SIGCHLD` to the parent (`ppid > 0`) to wake a parent blocked in `sys_wait4()`. Zombie reaping (closing the descriptor table, dropping the `cwd` reference) happens in `bsd_proc_exit()`.

---

## 5. Core BSD Subsystems

### 5.1 Process Model (`bsd_proc_t`, `src/include/bsd/proc.h`)
Each BSD process wraps a microkernel `task_t` via `task->personality_data`:
```c
#define BSD_MAX_FDS     64
#define BSD_MAX_PROCS   256
#define BSD_COMM_LEN    32
#define BSD_NSIG        32
#define BSD_MAX_MMAPS   16

typedef enum bsd_proc_state { PROC_UNUSED = 0, PROC_RUNNING, PROC_ZOMBIE } bsd_proc_state_t;

typedef struct bsd_proc {
    int32_t           pid;
    int32_t           ppid;
    int32_t           pgid;             /* Process group id */
    uint32_t          uid;
    uint32_t          gid;
    bsd_proc_state_t  state;
    int32_t           exit_status;      /* (status << 8) or 128 + signo */
    uint64_t          pending_signals;  /* Bitmask of pending BSD_SIG* */
    uint64_t          signal_mask;      /* Blocked signal mask */
    bsd_sigaction_t   actions[BSD_NSIG];/* Per-signal dispositions */
    task_t           *mk_task;          /* Underlying microkernel task */
    vnode_t          *cwd;              /* Current working directory */
    file_t           *fd_table[BSD_MAX_FDS];
    vaddr_t           brk_base;
    vaddr_t           brk_current;
    bsd_mmap_region_t mmap_regions[BSD_MAX_MMAPS];
    vaddr_t           mmap_next;
    struct bsd_proc  *parent;
    struct bsd_proc  *first_child;
    struct bsd_proc  *next_sibling;
    thread_t         *waiter_thread;    /* Parent sleeping in sys_wait4() */
    thread_t         *signal_waiter;    /* Thread parked in a pipe wait */
    size_t            fork_frames;      /* Free-frame count at fork() */
    uint64_t          fork_objects;     /* mk_object count at fork() */
    bool              fork_leak_check;
    char              comm[BSD_COMM_LEN];
} bsd_proc_t;
```

**Single-CPU affinity invariant.** The BSD layer serializes per-process state
(`fd_table`, `mmap_regions`, `brk`, signal masks) with `irq_save()`, which only excludes
the local CPU. `bsd_proc_create()` therefore **rejects any task whose `affinity_cpu ==
TASK_AFFINITY_ANY`**, so a BSD process never has threads that could race those sections
from two CPUs at once. The microkernel's rendezvous fast path (a direct handoff between
IPC peers) is likewise only sound when both threads share one run queue.

**`fork()`.** `bsd_proc_fork()` snapshots descriptors/cwd, `pgid`, `signal_mask`, signal
actions, `brk`, mmap regions, uid/gid, and the parent thread's `fs_base` under `irq_save`,
clones the address space with `vspace_clone()` (copy-on-write), and creates the child
thread with `thread_create_with_fs_base()` so the inherited TLS base is visible before the
child can be scheduled on any CPU. The child resumes via `user_frame_resume()` with
`rax = 0`.

**`execve()`.** `bsd_proc_execve()` builds the new `vspace`, loads the ELF64 image, lays out
`argv`/`envp` on the user stack, swaps the task vspace, then resets the data break and mmap
regions. Signal handling is reset per POSIX: **custom handlers become `SIG_DFL`, while
`SIG_IGN` dispositions and the blocked `signal_mask` are preserved**; `pending_signals` is
cleared. The thread's TLS base is zeroed (`exec_thread->fs_base = 0` and `IA32_FS_BASE`
MSR written to 0).

### 5.2 Signals
Signal numbers follow the BSD convention (`SIGHUP`=1, `SIGINT`=2, ... `SIGUSR2`=31).
`SIGKILL` and `SIGSTOP` are unmaskable and cannot be caught or ignored.

- **`sigaction` (46):** validates the handler address for custom handlers (must lie in
  `[0x1000, 0x00007FFFFFFFFFFF]`), strips `SIGKILL`/`SIGSTOP` from `sa_mask`, and stores
  the disposition.
- **`sigprocmask` (48):** applies `SIG_BLOCK`/`SIG_UNBLOCK`/`SIG_SETMASK`, always clearing
  the unmaskable bits afterward.
- **Delivery (`bsd_proc_deliver_signals`):** runs on the target's next syscall return
  (and on wakeups, e.g., console reads return `BSD_EINTR`). The lowest deliverable signal
  is chosen. If a custom handler is installed, a user-space frame is built and execution is
  redirected; otherwise the default action runs (fatal signals terminate with `128+signo`).
- **User-space signal delivery (`deliver_to_user_handler`):** builds a `bsd_sigframe_t`
  on the user stack, 16-byte aligned, with `restorer` (the `sa_restorer` trampoline) at the
  top so it acts as the handler's return address. It saves all user GPRs, sets `RDI =
  signo` and the user `RSP`/`RIP`, and blocks `sa_mask | signo` during the handler.

  ```c
  typedef struct bsd_sigframe {   /* sizeof == 160, 16-byte aligned */
      uint64_t restorer;          /* sa_restorer trampoline (return address) */
      uint32_t signo;             /* Signal number (delivered in RDI) */
      uint32_t _pad0;
      uint64_t saved_mask;        /* signal_mask to restore on sigreturn */
      uint64_t rax, rdi, rsi, rdx, r10, r8, r9;
      uint64_t rcx;               /* User RIP saved by SYSCALL */
      uint64_t r11;               /* User RFLAGS saved by SYSCALL */
      uint64_t rsp;
      uint64_t r15, r14, r13, r12, rbx, rbp;
      uint64_t _pad1;
  } bsd_sigframe_t;
  ```
- **`sigreturn` (103):** reads the frame address from `RDI`, validates the saved user `RIP`
  and `RSP` are canonical, restores every GPR, sanitizes `RFLAGS` (`&= ~0x77500` then
  `|= 0x202`, forcing `IF=1`, clearing `TF`/`DF`/`IOPL`/`NT`/`RF`/`VM`/`AC`), and restores
  the saved signal mask (with `SIGKILL`/`SIGSTOP` still unmaskable).

### 5.3 Process Groups & `/dev/console` (`console.c`)
Each process has a `pgid` (its own PID by default). `setpgid` (82) and `getpgid` (207)
manipulate it; only a process itself or its parent may change its group. The console tracks
a **foreground process group** (`console_fg_pgid`), set via `setpgid`'d `BSD_TIOCSPGRP`.

Incoming `Ctrl+C` (`0x03`) flushes the current line buffer and delivers `BSD_SIGINT` to the
foreground process group via `bsd_signal_send_group(fg_pgid, BSD_SIGINT)` (falling back to
the legacy single foreground PID if no group is set). `bsd_signal_send_group` collects wake
targets under the process-table lock and wakes them after releasing it, avoiding an SMP
TOCTOU race. `kill` process-group semantics (`pid == 0`, `pid < -1`) are documented in
[`BSD_SYSCALLS.md`](BSD_SYSCALLS.md) §3.1.

### 5.4 Thread-Local Storage (`IA32_FS_BASE`, MSR `0xC0000100`)
Each thread carries `thread_t.fs_base`. `arch_prctl(BSD_ARCH_SET_FS)` writes the MSR
immediately and records the base; `BSD_ARCH_GET_FS` reads it back. The scheduler saves and
restores `IA32_FS_BASE` across context switches. On `fork()` the child inherits the
parent's base through `thread_create_with_fs_base()`; on `execve()` the base is zeroed.

### 5.5 Virtual File System (`vnode_t` & `file_t`, `vfs.c`)
- **`vnode_t`:** filesystem-agnostic node (`VNODE_REG`, `VNODE_DIR`, `VNODE_CHR`) with
  `size`, `mode`, `vnode_ops_t *ops`, `void *fs_data`, `parent`, `name`, and an atomic
  `ref_count` (`vfs_vnode_ref`/`vfs_vnode_unref`).
- **`file_t`:** open file description shared across `dup()`/`fork()`, containing a
  `spinlock_t lock` (serializing offset + regular-file I/O), `vnode_t *vnode`,
  `uint64_t offset`, `uint32_t flags`, and `ref_count`.
- **Path resolution (`vfs_lookup`):** absolute paths, relative paths against `proc->cwd`,
  `.`/`..` traversal, and mount-point redirection (e.g., `/dev/console`).
- Descriptor table helpers (`bsd_fd_alloc`, `bsd_fd_get`, `bsd_fd_close`, `bsd_fd_close_all`)
  manage `fd_table[64]`; `vfs_dup`/`vfs_dup2` share the underlying `file_t`.

### 5.6 Memory Mapping (`mmap.c`)
`mmap`/`munmap` maintain a per-process table of tracked `mmap_regions`; `brk` grows or
shrinks the data break with fresh anonymous pages. **`mprotect` (74) enforces `W^X`:** a
request with `PROT_WRITE | PROT_EXEC` is rejected with `-BSD_EINVAL`, and the range must be
page-aligned and contained in a tracked region. Protection changes go through
`vspace_remap_user()`, which resolves **CoW** first when the new protection adds write
(copying a shared frame into a private one, or making a single-reference page writable in
place). After the PTE mutation, stale translations on remote CPUs are invalidated with
`tlb_shootdown_mask()` over `vspace->active_cpus`.

### 5.7 Writable `ext2` Root (`ext2.c`)
`ext2_mount()` reads the superblock and block-group descriptor table from the primary
`blkdev_t`, then installs the root inode as the VFS root (`vfs_set_root`). The driver
supports regular files and directories with:

- **Block mapping:** 12 direct blocks plus single- and double-indirect blocks (`i_block[0..14]`;
  a triple-indirect constant is reserved). Indirect trees are allocated lazily on write and
  freed recursively on truncate/unlink.
- **Bitmap guards:** `ext2_alloc_block()` scans the per-group block bitmap and skips
  reserved blocks (`ext2_block_is_reserved`: superblock, GDT, backup superblock/GDT areas,
  block/inode bitmaps, and inode tables); `ext2_alloc_inode()` scans the inode bitmap.
  Free counts in the superblock and group descriptors are kept in sync.
- **Deferred free on open-unlink:** an `ext2_open_entry` table tracks referenced inodes.
  `unlink()` of a still-open inode marks it unlinked but retains the inode (and its blocks)
  until the last live vnode reference is released, matching POSIX unlink-while-open
  semantics.
- All metadata operations are serialized by `ext2_lock`, below `file->lock` and above the
  block device.

### 5.8 Read-Only `initrd` Fallback (`initrd.c`)
When no writable root is available, `initrd_mount()` parses a standard 512-byte-block POSIX
**USTAR (`tar`)** archive loaded as a Limine module (`initrd.tar`), supporting regular files
(`typeflag == '0'` or `'\0'`) and directories (`typeflag == '5'`), with zero-copy reads
from the mapped HHDM archive window.

### 5.9 TTY Console (`/dev/console`)
Character device backed by `COM1` UART (`serial_putc` for output; polling `serial_getc` for
input). It implements a canonical line discipline: `\r` (`0x0D`) is translated to `\n`, `\r`
and `\n` are echoed as `\r\n`, and `Backspace` (`0x08`/`0x7F`) erases the previous character
(`\b \b`). `Ctrl+C` (`0x03`) delivers `BSD_SIGINT` to the foreground process group (§5.3).
It is opened as `fd 0` (stdin, read-only), `fd 1` (stdout) and `fd 2` (stderr) when PID 1
is created. A blocking read returns `BSD_EINTR` when the process has a pending deliverable
signal.
