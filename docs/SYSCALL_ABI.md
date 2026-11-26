# SYSCALL_ABI.md — x86_64 System Call & IPC ABI

## 1. Entry Mechanism (`SYSCALL` / `SYSRET`)

All user-space transitions into the kernel use the fast `SYSCALL` instruction configured via Model-Specific Registers (MSRs) in `src/arch/x86_64/syscall.c`:

- **`IA32_EFER` (`0xC0000080`):** Bit 0 (`SCE`) enabled; Bit 11 (`NXE`) enabled.
- **`IA32_STAR` (`0xC0000081`):**
  - Bits `47:32` = `0x0008` (Kernel CS = `0x08`, Kernel SS = `0x10`).
  - Bits `63:48` = `0x0010` (SYSRET derives User SS = `0x18 | 3`, User CS = `0x20 | 3`).
- **`IA32_LSTAR` (`0xC0000082`):** Virtual address of `syscall_entry` in `src/arch/x86_64/syscall.asm`.
- **`IA32_FMASK` (`0xC0000084`):** `0x0000000000040700` (clears `IF`, `TF`, `DF`, and `AC` immediately upon Ring 0 entry).
- **`IA32_GS_BASE` (`0xC0000101`) / `IA32_KERNEL_GS_BASE` (`0xC0000102`):** Strict per-CPU GS invariant: while executing in Ring 0, `GS_BASE = &cpu_local`; while executing in Ring 3, `GS_BASE = 0` and `KERNEL_GS_BASE = &cpu_local`. `SYSCALL`/ISR entry executes `swapgs`; `SYSRETQ`/`IRETQ` exit executes `swapgs`. `cpu_local` holds `kernel_rsp` at offset 0 and `user_rsp_scratch` at offset 8 (`self` is at offset 48).
- **`IA32_FS_BASE` (`0xC0000100`):** User-space Thread-Local Storage base. Managed per-thread by the scheduler (`thread_t.fs_base`) and via `SYS_ARCH_PRCTL` (see [`BSD_SYSCALLS.md`](BSD_SYSCALLS.md)).

The entry stub (`syscall.asm`) builds a 16-qword `syscall_frame_t` (matching `src/include/syscall.h`), calls `syscall_dispatch()`, then returns via `SYSRETQ` when the user `RIP` is canonical or via a constructed `IRETQ` frame otherwise. User `RFLAGS` are sanitized on the way out (`&~0x77500`, `| 0x202`).

---

## 2. Register Passing Convention

| Register | Direction | Role |
| :--- | :--- | :--- |
| `rax` | In / Out | Syscall number on entry; `sys_status_t` (or BSD return value) on exit |
| `rdi` | In / Out | Argument 1; `signo` on signal-handler entry |
| `rsi` | In / Out | Argument 2 on entry; `msg[0]` on IPC receive/call exit |
| `rdx` | In / Out | Argument 3 on entry; `ipc_badge` (or new capability handle) on exit |
| `r10` | In / Out | Argument 4 (replaces `rcx`, which holds user `RIP`); `msg[3]` on IPC exit |
| `r8` | In / Out | Argument 5 on entry; `msg[1]` on IPC receive/call exit |
| `r9` | In / Out | Argument 6 on entry; `msg[2]` on IPC receive/call exit |
| `rcx` | Clobbered | Hardware-saved user `RIP` |
| `r11` | Clobbered | Hardware-saved user `RFLAGS` (sanitized on return) |

---

## 3. Syscall Number Partitioning

Bit `63` of `rax` cleanly partitions native **Microkernel Traps** from **Personality Layer Syscalls** (such as BSD POSIX syscalls):

- **Bit `63 == 1` (`0x8000000000000000`+):** Handled directly by the **Microkernel Core** (`src/mk/syscall.c`).
- **Bit `63 == 0` (`0x0000000000000000` – `0x7FFFFFFFFFFFFFFF`):** Delegated to `mk_hooks.on_personality_syscall()` (e.g., BSD `SYS_read`, `SYS_write`, `SYS_fork`, `SYS_execve`). If no personality is registered, returns `SYS_ERR_NOSYS`.

### Native Microkernel Syscall Table

| Syscall Constant | Number (`rax`) | Arguments (`rdi`, `rsi`, `rdx`, `r10`, `r8`, `r9`) | Description |
| :--- | :--- | :--- | :--- |
| `SYS_THREAD_EXIT` | `0x8000000000000001` | `(int64_t code)` | Terminate calling thread |
| `SYS_THREAD_YIELD` | `0x8000000000000002` | `(void)` | Voluntarily yield current CPU timeslice |
| `SYS_THREAD_SLEEP_MS` | `0x8000000000000003` | `(uint64_t ms)` | Sleep for the requested millisecond interval |
| `SYS_IPC_CALL` | `0x8000000000000010` | `(cap_t ep, uint64_t m0, m1, m2, m3)` | Synchronous send + block for reply on endpoint |
| `SYS_IPC_RECV` | `0x8000000000000011` | `(cap_t ep)` | Block until a message arrives; returns `msg[0..3]` and badge |
| `SYS_IPC_REPLY` | `0x8000000000000012` | `(uint64_t m0, m1, m2, m3)` | Reply to the last caller received by the current thread |
| `SYS_NOTIFY_SIGNAL` | `0x8000000000000020` | `(cap_t ntf, uint64_t mask)` | Atomically OR `mask` into the notification word |
| `SYS_NOTIFY_WAIT` | `0x8000000000000021` | `(cap_t ntf)` | Wait for nonzero bits and atomically clear them; bits in `rdx` |
| `SYS_CAP_DERIVE` | `0x8000000000000030` | `(cap_t src, uint64_t rights, uint64_t badge)` | Derive an attenuated capability; new handle in `rdx` |
| `SYS_CAP_GRANT` | `0x8000000000000031` | `(cap_t src)` | Duplicate within the calling task; new handle in `rdx` |
| `SYS_CAP_DELETE` | `0x8000000000000032` | `(cap_t cap)` | Remove a capability from the calling task's table |
| `SYS_VSPACE_MAP` | `0x8000000000000040` | `(cap_t frame, uint64_t vaddr, uint64_t prot)` | Map a held frame capability into the task vspace |
| `SYS_VSPACE_UNMAP` | `0x8000000000000041` | `(uint64_t vaddr, uint64_t size)` | Unmap a user virtual range |
| `SYS_DEBUG_LOG` | `0x80000000000000F0` | `(const char *buf, size_t len)` | Write a bounded user string (`<= 256` bytes) to the kernel console |

IPC call/receive return the status in `rax`, the badge in `rdx`, and the four message
words in `rsi`, `r8`, `r9`, and `r10`. `notify_wait` returns the observed bits in
`rdx`. `SYS_CAP_DERIVE` and `SYS_CAP_GRANT` return the new capability handle in `rdx`.
`SYS_CAP_GRANT` currently only duplicates a capability within the calling task (a
task capability is not yet exposed by this ABI). `SYS_VSPACE_MAP` requires
`CAP_RIGHT_READ` (plus `CAP_RIGHT_WRITE` when `prot` includes `VSPACE_PROT_WRITE`),
rejects addresses below the null guard (`0x1000`) or unaligned to `PAGE_SIZE`, and
transfers frame ownership to the vspace exactly once.

---

## 4. Capability Object Types & Rights (`src/include/cap.h`)

Capabilities index a per-task `cap_table_t` of `CAP_TABLE_SLOTS = 256` slots; slot 0 is
`CAP_NULL` and is always invalid. Each slot names one kernel object kind, an immutable
rights mask, and an optional badge:

```c
typedef enum cap_obj_type {
    CAP_OBJ_NONE = 0,
    CAP_OBJ_ENDPOINT,
    CAP_OBJ_NOTIFICATION,
    CAP_OBJ_VSPACE,
    CAP_OBJ_IRQ,
    CAP_OBJ_FRAME
} cap_obj_type_t;

#define CAP_RIGHT_READ   (1ULL << 0)
#define CAP_RIGHT_WRITE  (1ULL << 1)
#define CAP_RIGHT_CALL   (1ULL << 2)
#define CAP_RIGHT_GRANT  (1ULL << 3)
#define CAP_RIGHT_ALL    (CAP_RIGHT_READ | CAP_RIGHT_WRITE | \
                          CAP_RIGHT_CALL | CAP_RIGHT_GRANT)
```

`cap_lookup_retained()` is used by blocking IPC so the capability table may be mutated
while the caller sleeps; the caller releases the retained object with
`mk_object_release()`.

---

## 5. Standard Return Status Codes (`sys_status_t`)

All native microkernel syscalls return a signed 64-bit `sys_status_t` in `rax`
(`src/include/types.h`):

```c
typedef int64_t sys_status_t;

#define SYS_OK              ((sys_status_t)0)
#define SYS_ERR_INVAL       ((sys_status_t)-1)  // Invalid argument or unaligned address
#define SYS_ERR_NOMEM       ((sys_status_t)-2)  // Out of physical frames or kernel heap
#define SYS_ERR_CAP_INVALID ((sys_status_t)-3)  // Capability slot empty or wrong object type
#define SYS_ERR_CAP_RIGHTS  ((sys_status_t)-4)  // Capability lacks required permission bit
#define SYS_ERR_BUSY        ((sys_status_t)-5)  // Resource locked or state conflict
#define SYS_ERR_DEAD        ((sys_status_t)-6)  // Target thread or endpoint terminated
#define SYS_ERR_NOSYS       ((sys_status_t)-7)  // Unimplemented syscall or no personality attached
```
