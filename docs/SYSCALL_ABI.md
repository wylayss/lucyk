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
| `SYS_THREAD_SET_QOS` | `0x8000000000000004` | `(uint64_t tid, uint64_t qos)` | Set a thread's base QoS class (`tid 0` = caller, else a thread of the calling task); `qos = SYS_QOS_QUERY` only reads. Base class in `rdx`, effective class in `r8` |
| `SYS_TASK_ID` | `0x8000000000000005` | `(void)` | Caller's task id in `rdx` (the `SYS_CAP_GRANT` target selector), thread id in `r8` |
| `SYS_IPC_CALL` | `0x8000000000000010` | `(cap_t ep, uint64_t m0, m1, m2, m3)` | Synchronous send + block for reply on endpoint |
| `SYS_IPC_RECV` | `0x8000000000000011` | `(cap_t ep)` | Block until a message arrives; returns `msg[0..3]` and badge |
| `SYS_IPC_REPLY` | `0x8000000000000012` | `(uint64_t m0, m1, m2, m3)` | Reply to the last caller received by the current thread |
| `SYS_ENDPOINT_CREATE` | `0x8000000000000013` | `(void)` | Create an endpoint; new handle (`CAP_RIGHT_ALL`) in `rdx` |
| `SYS_NOTIFY_SIGNAL` | `0x8000000000000020` | `(cap_t ntf, uint64_t mask)` | Atomically OR `mask` into the notification word |
| `SYS_NOTIFY_WAIT` | `0x8000000000000021` | `(cap_t ntf)` | Wait for nonzero bits and atomically clear them; bits in `rdx` |
| `SYS_NOTIFY_CREATE` | `0x8000000000000022` | `(void)` | Create a notification; new handle (`CAP_RIGHT_ALL`) in `rdx` |
| `SYS_CAP_DERIVE` | `0x8000000000000030` | `(cap_t src, uint64_t rights, uint64_t badge)` | Derive an attenuated capability; new handle in `rdx` |
| `SYS_CAP_GRANT` | `0x8000000000000031` | `(cap_t src, uint64_t target_task, uint64_t rights)` | Copy `src` (needs `CAP_RIGHT_GRANT`) into the target task's table with `rights & src.rights`; `target_task 0` = caller. Handle in the target's table in `rdx` |
| `SYS_CAP_DELETE` | `0x8000000000000032` | `(cap_t cap)` | Remove a capability from the calling task's table |
| `SYS_VSPACE_MAP` | `0x8000000000000040` | `(cap_t frame, uint64_t vaddr, uint64_t prot)` | Map a held frame capability into the task vspace |
| `SYS_VSPACE_UNMAP` | `0x8000000000000041` | `(uint64_t vaddr, uint64_t size)` | Unmap a user virtual range |
| `SYS_IRQ_BIND` | `0x8000000000000050` | `(cap_t irq, cap_t ntf, uint64_t bit)` | Attach a notification to a `CAP_OBJ_IRQ` (both need `CAP_RIGHT_WRITE`, `bit != 0`); unmasks a GSI-backed line |
| `SYS_IRQ_ACK` | `0x8000000000000051` | `(cap_t irq)` | Re-arm (unmask) a GSI-backed IRQ after its driver serviced the device |
| `SYS_IRQ_CREATE` | `0x8000000000000052` | `(cap_t hw_cap, uint64_t rights)` | Privileged: `hw_cap` must name a `CAP_OBJ_HARDWARE` capability with `CAP_RIGHT_WRITE` (`SYS_ERR_CAP_INVALID` / `SYS_ERR_CAP_RIGHTS` otherwise); `rights & ~CAP_RIGHT_ALL` = `SYS_ERR_INVAL`. Mint a fresh unbound `CAP_OBJ_IRQ` (`cap_irq_object_create(0)`) with `CAP_RIGHT_ALL & rights`, install it in the caller's capability table, new handle in `rdx`; `SYS_ERR_NOMEM`. No interrupt vector is claimed until e.g. `SYS_PCI_ALLOC_MSI` programs one; `SYS_IRQ_BIND` needs a bound vector. Deleting the capability releases it. `SYS_OK` in `rax`. |
| `SYS_DEBUG_LOG` | `0x80000000000000F0` | `(const char *buf, size_t len)` | Write a bounded user string (`<= 256` bytes) to the kernel console |
| `SYS_DMA_ALLOC` | `0x8000000000000060` | `(uint64_t page_count, uint64_t rights)` | Stage F: allocate zeroed contiguous PMM pages (`page_count` cap `SYS_DMA_ALLOC_MAX_PAGES`=256; `rights & ~CAP_RIGHT_ALL`=`SYS_ERR_INVAL`, `READ|WRITE` required) as `CAP_FRAME_DMA`; handle in `rdx`; `SYS_ERR_NOMEM` |
| `SYS_FRAME_PHYS_ADDR` | `0x8000000000000061` | `(cap_t frame_cap)` | Stage F: physical base in `rdx` **only** for `CAP_FRAME_DMA` (needs `CAP_RIGHT_READ`); `CAP_FRAME_ANON`/`CAP_FRAME_SHARED`/`CAP_FRAME_DEVICE`/invalid -> `SYS_ERR_INVAL` |
| `SYS_PCI_CLAIM_BAR` | `0x8000000000000062` | `(uint64_t bdf, uint64_t bar_index)` | Stage F (privileged `CAP_OBJ_HARDWARE`, PID 1): validate `bar_index` 0..5 + MMIO (32/64-bit) nonzero BAR, mark claimed (`SYS_ERR_BUSY` on duplicate), enable `PCI_COMMAND_MEMORY\|PCI_COMMAND_BUS_MASTER`, page-round, wrap `CAP_FRAME_DEVICE` (`PTE_DEVICE\|PTE_NX`, never PMM-freed, `CAP_RIGHT_READ\|WRITE\|GRANT` so it can be granted on); handle `rdx`, sub-page offset `r8`. `bdf=(bus<<16)\|(slot<<8)\|func`. `SYS_ERR_CAP_RIGHTS` if unprivileged |
| `SYS_PCI_ALLOC_MSI` | `0x8000000000000063` | `(uint64_t bdf, cap_t irq_cap)` | Stage F (privileged `CAP_OBJ_HARDWARE`): `irq_cap` `CAP_OBJ_IRQ`+`CAP_RIGHT_WRITE`; allocate LAPIC vector `0x50..0x6F`, program PCI MSI/MSI-X (`0xFEE00000\|(bsp_apic_id<<12)`, vector), enable MSI/MSI-X + bus-master, bind vector to `irq_cap`; `SYS_ERR_NOSYS` if no MSI/MSI-X, `SYS_ERR_NOMEM` no vector, `SYS_ERR_CAP_RIGHTS` unprivileged; vector in `rdx` |
| `SYS_FBCON_RELEASE` | `0x8000000000000064` | `(void)` | Stage F: atomically stop kernel framebuffer text drawing (`fbcon_release`); also triggered when user-space maps the boot framebuffer `CAP_OBJ_FRAME`; `SYS_OK` |

IPC call/receive return the status in `rax`, the badge in `rdx`, and the four message
words in `rsi`, `r8`, `r9`, and `r10`. `notify_wait` returns the observed bits in
`rdx`. `SYS_CAP_DERIVE`, `SYS_CAP_GRANT`, `SYS_ENDPOINT_CREATE` and `SYS_NOTIFY_CREATE`
return the new capability handle in `rdx`. `SYS_VSPACE_MAP` requires
`CAP_RIGHT_READ` (plus `CAP_RIGHT_WRITE` when `prot` includes `VSPACE_PROT_WRITE`),
rejects addresses below the null guard (`0x1000`) or unaligned to `PAGE_SIZE`, and maps
every page the frame capability covers according to its kind (§4.1).

### 3.1 `SYS_THREAD_SET_QOS` — Quality-of-Service Classes

```c
#define SYS_QOS_BACKGROUND  0ULL   /* THREAD_QOS_BACKGROUND:  bulk / maintenance work      */
#define SYS_QOS_DEFAULT     1ULL   /* THREAD_QOS_DEFAULT:     every thread starts here     */
#define SYS_QOS_INTERACTIVE 2ULL   /* THREAD_QOS_INTERACTIVE: compositor, HID, audio      */
#define SYS_QOS_QUERY       0xFFFFFFFFFFFFFFFFULL /* read only, no change              */
```

- `rdi = tid`: `0` names the calling thread, otherwise a live thread of the **calling
  task** (`SYS_ERR_INVAL` otherwise). `rsi = qos`: one of the classes above or
  `SYS_QOS_QUERY`; any other value returns `SYS_ERR_INVAL`.
- On success `rax = SYS_OK`, `rdx = base_qos`, `r8 = effective_qos`.
- Every thread has a `base_qos` (what it asked for) and an `effective_qos =
  max(base_qos, inherited)`. A thread created by duplicating its caller (BSD `fork()`)
  inherits the creator's base class.
- **Dispatch:** each per-CPU run queue always dispatches the runnable thread with the
  highest `effective_qos`; equal classes rotate round-robin. **Aging:** each time a
  runnable `DEFAULT`/`BACKGROUND` thread loses a dispatch decision to a higher class its
  credit grows; at 8 (`DEFAULT`) or 16 (`BACKGROUND`) lost decisions it is dispatched
  for one quantum regardless of class. A `BACKGROUND` thread therefore receives at least
  one 2 ms quantum in every 17 dispatches even under saturating interactive load.
- **Priority inheritance:** while a caller is blocked in `SYS_IPC_CALL`
  (`THREAD_BLOCKED_REPLY`) on a server thread, the server's `effective_qos` is raised to
  at least the caller's `effective_qos` (in `sched_handoff()` for the direct rendezvous
  and in `ipc_recv()` when the server picks a queued caller). The boost ends — the server
  falls back to its `base_qos` — when `SYS_IPC_REPLY` completes, or when the caller is
  destroyed or woken by endpoint teardown.

### 3.2 `SYS_CAP_GRANT` — Cross-Task Capability Transfer

`SYS_CAP_GRANT(src, target_task, rights)` copies capability `src` of the calling task
into the capability table of the task whose id is `target_task` (as reported by that
task's `SYS_TASK_ID`; `0` or the caller's own id duplicates in place):

- `src` must hold `CAP_RIGHT_GRANT` (`SYS_ERR_CAP_RIGHTS` otherwise).
- The copy receives `rights & src.rights` — equal or attenuated, never amplified
  (`rights` with bits outside `CAP_RIGHT_ALL` is `SYS_ERR_INVAL`). Granting without
  `CAP_RIGHT_GRANT` produces a capability the receiver cannot pass on.
- The badge and object are shared; the object gains one reference. Every capability
  kind may be granted: `CAP_OBJ_ENDPOINT`, `CAP_OBJ_NOTIFICATION`, `CAP_OBJ_IRQ`,
  `CAP_OBJ_FRAME` (and `CAP_OBJ_VSPACE`).
- An unknown task id returns `SYS_ERR_INVAL`; a task that is being torn down returns
  `SYS_ERR_DEAD`; a full target table returns `SYS_ERR_NOMEM`. On success `rdx` is the
  handle **in the target's table**; the caller delivers it to the target out of band
  (an IPC message or a pipe).

The task registry and capability tables are serialised by spinlocks (`task registry ->
cap`), so a grant is safe against concurrent use of either table from any CPU.

### 3.3 `SYS_IRQ_BIND` / `SYS_IRQ_ACK` — User-Mode Interrupt Delivery

A `CAP_OBJ_IRQ` backed by an IOAPIC GSI owns that line: the kernel allocates a vector
(`0x50..0x6F`) and routes the GSI there **masked** (`ioapic_bind_cap_irq()`, which also
refuses a GSI already routed by a kernel driver). `SYS_IRQ_BIND` attaches a notification
bit and unmasks the line. When the interrupt fires the kernel masks the GSI, sends the
LAPIC EOI and ORs the bit into the notification, waking `SYS_NOTIFY_WAIT`. The Ring 3
driver services its device and calls `SYS_IRQ_ACK`, which unmasks the line; a
level-triggered source therefore cannot storm the CPU while its driver works. Deleting
the last capability masks the line and releases the GSI and vector. IRQ capabilities are
minted by the kernel (boot capabilities) and passed on with `SYS_CAP_GRANT`, and a
privileged `CAP_OBJ_HARDWARE` holder can dynamically mint fresh unbound `CAP_OBJ_IRQ`
objects with `SYS_IRQ_CREATE` (`cap_irq_object_create(0)`); a vector is assigned later
(e.g. `SYS_PCI_ALLOC_MSI`) before `SYS_IRQ_BIND`.

### 3.4 FPU / SSE State

Every thread owns a 512-byte `FXSAVE64` image, 16-byte aligned inside `thread_t`. It
starts as the clean default (`FCW = 0x037F`, `MXCSR = 0x1F80`, `XMM0-XMM15 = 0`, empty x87
stack); `switch_context` executes `FXSAVE64` for the outgoing and `FXRSTOR64` for the
incoming thread on every switch (eager switching, `CR0.TS` stays clear). Native threads
created by duplication (BSD `fork()`) start with a copy of the creator's live registers;
loading a new program image (BSD `execve()`) resets them to the default. The kernel
itself is compiled with `-mno-sse -mno-80387` and never alters user FPU/SSE state.

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
    CAP_OBJ_FRAME,
    CAP_OBJ_HARDWARE
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

### 4.1 `CAP_OBJ_FRAME` Kinds

| Kind | Backing | `SYS_VSPACE_MAP` | Release |
| :--- | :--- | :--- | :--- |
| `CAP_FRAME_ANON` | one PMM frame | once; ownership moves to the vspace | frame freed if never mapped |
| `CAP_FRAME_SHARED` | one PMM frame, object holds a reference | any number of times; each mapping holds its own frame reference | object drops its reference; the frame lives until the last mapping goes |
| `CAP_FRAME_DEVICE` | page-aligned physical range outside the PMM (MMIO, framebuffer) | any number of times; all pages of the range; always `NX`; write-combining (PAT) or uncached (`PCD`/`PWT`); `VSPACE_PROT_EXEC` is `SYS_ERR_INVAL` | **never** calls `pmm_free_frame()` |
| `CAP_FRAME_DMA` | contiguous PMM-managed pages (`pmm_alloc_contiguous`, `SYS_DMA_ALLOC`) | any number of times; `NX`, no `EXEC`; per-page PMM refcount + object refcount | every page `pmm_frame_unref`'d; `SYS_FRAME_PHYS_ADDR` allowed only here |

Device leaves carry the software `PTE_DEVICE` bit: unmapping or tearing down a vspace
never returns them to the PMM, `vspace_clone()` shares them instead of copying,
`mprotect` refuses to remap them, and `copy_from_user()`/`copy_to_user()` reject them.

### 4.2 Boot Capabilities: Zero-Copy Framebuffer Handoff

During boot the kernel captures the Limine linear framebuffer and registers two boot
capabilities that `bsd_init()` installs into **PID 1** before its first instruction:

| Handle | Object | Rights |
| :--- | :--- | :--- |
| `1` (`DISPLAY_BOOT_CAP_FRAMEBUFFER`) | `CAP_FRAME_DEVICE` over the whole framebuffer (page-rounded), write-combining | `READ | WRITE | GRANT` |
| `2` (`DISPLAY_BOOT_CAP_INFO`) | `CAP_FRAME_SHARED` page holding a `display_info_t` | `READ | GRANT` (read-only) |

`display_info_t` (`src/include/display.h`, 80 bytes) carries `magic` (`"LUCYDISP"`),
`version`, `framebuffer_count`, `phys_base`, `size_bytes`, `mapped_bytes`, `width`,
`height`, `pitch`, `bpp`, `memory_model`, the red/green/blue mask sizes and shifts, and
`write_combining`. User space maps both frames and renders directly; the kernel contains
no graphics code. A display server receives them from PID 1 via `SYS_CAP_GRANT`.

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
