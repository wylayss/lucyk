# ARCHITECTURE.md — Microkernel Specification & BSD Boundary

## 1. System Architecture Overview

This system follows a **Modular Hybrid-Ready Microkernel** design. The **Microkernel Core** enforces hardware isolation, virtual address spaces, preemptive scheduling, and capability-based IPC. A separate **hardware I/O layer** owns PCI, interrupt routing, and block storage. An optional **Personality Layer** (the BSD subsystem) attaches to the core via a well-defined C callback table (`mk_hooks_t`) and syscall delegation without altering microkernel data structures.

```text
+-------------------------------------------------------------------+
|                        User Space (Ring 3)                        |
|        Native Microkernel Tasks    |    BSD / POSIX Binaries      |
+------------------------------------+------------------------------+
                                     |  (syscall / sysret)
=====================================|============================== (Ring 0)
                                     v
+-------------------------------------------------------------------+
|               Personality Layer: BSD Subsystem (src/bsd/)         |
|   struct proc   |   VFS & vnodes   |   Signals   |   BSD Syscalls |
+------------------------------------+------------------------------+
                                     |  (mk_hooks_t & Core C API)
                                     v
+-------------------------------------------------------------------+
|                     Microkernel Core (src/mk/)                    |
|  - Task & Thread Engine (preemptive per-CPU round-robin)          |
|  - VSpace Manager (4-level paging, W^X, address isolation, CoW)   |
|  - Capability & IPC Engine (synchronous endpoints & notifications)|
|  - Kernel Heap (kalloc) & Diagnostic Logger (klog)                |
+-------------------------------------------------------------------+
                                     |
                                     v
+-------------------------------------------------------------------+
|           Hardware I/O & HAL (src/io/, src/arch/x86_64/)          |
|  PCI cfg | VirtIO-Blk | ACPI/IOAPIC | GDT/TSS | IDT/ISR | APIC    |
|  PMM bitmap | VSpace page tables | COM1 | SMP AP bringup (LAPIC)  |
+-------------------------------------------------------------------+
```

---

## 2. Repository Layout & Subsystem Map

```text
LucyK/
├── AGENT.md                  # Engineering rules & constraints
├── README.md                 # Project overview, build/run guide
├── Makefile                  # ISO + writable ext2 disk image
├── linker.ld                 # Higher-half kernel image layout
├── limine.conf               # Limine boot menu
├── docs/
│   ├── ARCHITECTURE.md       # This specification
│   ├── SYSCALL_ABI.md        # Native microkernel syscall ABI
│   ├── BSD_PERSONALITY.md    # BSD personality architecture
│   └── BSD_SYSCALLS.md       # BSD syscall ABI & errno codes
├── src/
│   ├── kernel_main.c         # Higher-half entry & subsystem bootstrap
│   ├── include/              # Public headers
│   │   ├── io/               #   blkdev.h, pci.h
│   │   └── bsd/              #   bsd.h, console.h, errno.h, ext2.h, mmap.h,
│   │                         #   proc.h, syscall_bsd.h, vfs.h
│   ├── mk/                   # Microkernel core (personality-neutral)
│   │   ├── task.c            #   task_t / thread_t lifecycle
│   │   ├── sched.c           #   preemptive scheduler, idle, reaping
│   │   ├── cap.c             #   capability tables & rights
│   │   ├── ipc.c             #   endpoints, notifications, IRQ binding
│   │   ├── kalloc.c          #   page-backed kernel heap
│   │   ├── klog.c            #   serial logging & kpanic
│   │   ├── spinlock.c        #   ticket spinlocks
│   │   ├── elf.c             #   ELF64 loader
│   │   └── syscall.c         #   native syscall dispatcher
│   ├── arch/x86_64/          # HAL (personality-neutral)
│   │   ├── gdt.c / gdt.asm   #   GDT, per-CPU TSS
│   │   ├── idt.c / isr.asm   #   IDT & exception/IRQ stubs
│   │   ├── pmm.c             #   physical frame bitmap allocator
│   │   ├── vspace.c          #   4-level paging, CoW, TLB shootdown
│   │   ├── cpu.c             #   per-CPU GS block, SMEP/SMAP
│   │   ├── smp.c             #   AP bringup, IPIs, shootdown protocol
│   │   ├── ap_trampoline.asm #   16->32->64-bit AP bootstrap (flat binary)
│   │   ├── ap_image.asm      #   embeds the trampoline blob
│   │   ├── acpi.c            #   RSDP -> RSDT/XSDT -> MADT walk
│   │   ├── ioapic.c          #   IOAPIC routing & ISA overrides
│   │   ├── lapic.c           #   Local APIC & preemption timer
│   │   ├── serial.c          #   COM1 UART
│   │   ├── switch.asm        #   context switch
│   │   └── syscall.asm / syscall.c  # SYSCALL/SYSRET entry & MSRs
│   ├── io/                   # Hardware I/O & block storage (below src/bsd/)
│   │   ├── pci.c             #   PCI configuration scanner
│   │   └── virtio_blk.c      #   VirtIO-Blk DMA driver
│   └── bsd/                  # BSD personality (Ring 0, co-located)
│       ├── bsd_init.c        #   mounts root, registers hooks, spawns PID 1
│       ├── proc.c            #   process model, signals, pgid, fork/exec/wait4
│       ├── vfs.c             #   path resolution, fd table, pipes
│       ├── ext2.c            #   writable ext2 filesystem
│       ├── initrd.c          #   read-only USTAR fallback root
│       ├── console.c         #   COM1-backed /dev/console
│       └── mmap.c            #   anonymous mmap/munmap, brk, mprotect
└── user/
    ├── bsd/                  # bsd_user.h, lib.c, init.c, sh.c, echo.c
    ├── init_test.c           # Ring 3 syscall/ELF smoke test
    ├── fault_test.c          # Ring 3 fault test
    └── start.asm, runtime.c, user_lib.h, linker.ld
```

**Layering:** `src/io/` is the hardware I/O layer beneath `src/bsd/` and must never include `<bsd/...>`. `src/mk/`, `src/arch/x86_64/`, and `src/io/` are personality-neutral: they never reference BSD types or `errno`. The BSD layer reaches the core only through `mk_hooks_t` and the public microkernel headers.

---

## 3. x86_64 Virtual Memory Layout

The kernel executes in the upper canonical half (`-2 GiB`) using Limine's Higher-Half Direct Map (HHDM) to access arbitrary physical RAM frames without temporary mappings.

| Virtual Address Range | Size | Mapping Type | Purpose |
| :--- | :--- | :--- | :--- |
| `0x0000000000000000` – `0x0000000000000FFF` | `4 KiB` | Unmapped | Null-pointer dereference guard page |
| `0x0000000000001000` – `0x00007FFFFFFFFFFF` | `~128 TiB` | Per-`vspace_t` | User code, heap, stack, mmap regions, IPC buffers |
| `0x0000800000000000` – `0xFFFF7FFFFFFFFFFF` | Non-canonical | Hardware fault | Invalid canonical hole (triggers `#GP`) |
| `0xFFFF800000000000` – `0xFFFFAFFFFFFFFFFF` | Dynamic | Global Kernel | Limine HHDM physical memory direct window |
| `0xFFFFB00000000000` – `0xFFFFBFFFFFFFFFFF` | Dynamic | Global Kernel | Guarded per-thread kernel stacks (`KSTACK_BASE..KSTACK_LIMIT`) |
| `0xFFFFFFFF80000000` – `0xFFFFFFFFFFFFFFFF` | `2 GiB` | Global Kernel | Kernel ELF image (`.text`, `.rodata`, `.data`, `.bss`) |

Each kernel stack slot is five 4 KiB pages: one unmapped guard page followed by a 16 KiB RW/NX stack (`KSTACK_PAGE_COUNT = 4`, `KSTACK_GUARD_PAGES = 1`).

---

## 4. Core Microkernel Primitives (`src/mk/`)

### 4.1 `vspace_t` — Virtual Address Space
Encapsulates an isolated x86_64 4-level page table hierarchy (`PML4`).
```c
typedef struct vspace {
    uint64_t *pml4;        /* HHDM pointer to the top-level table */
    uint64_t  pml4_phys;   /* Physical address loaded into CR3 */
    uint64_t  ref_count;
    uint64_t  active_cpus; /* Bitmask of CPUs that may hold cached TLB entries */
} vspace_t;
```
- **Operations:**
  - `vspace_init()`: Captures the kernel page tables, enables `NXE` and `CR0.WP`, and pre-allocates one shared `PDPT` per kernel `PML4[256..511]` slot so later kernel mappings appear in every task.
  - `vspace_create()`: Allocates a zeroed `PML4` and shares the kernel upper half (`PML4[256..511]`).
  - `vspace_clone(src)`: Clones the lower-half user mappings. Owned writable pages become read-only copy-on-write in both address spaces; the kernel upper half is shared.
  - `vspace_map_user(vs, vaddr, paddr, flags)`: Maps a lower-half `4 KiB` page, always sets `PTE_USER`, and enforces `W^X`.
  - `vspace_map_kernel(vaddr, paddr, flags)`: Maps an upper-half (`>= 0xFFFF800000000000`) shared page, forbids `PTE_USER`, enforces `W^X`, and honors `VSPACE_PROT_PCD`/`VSPACE_PROT_PWT` for MMIO.
  - `vspace_unmap(vs, vaddr)` / `vspace_unmap_kernel(vaddr)`: Clear the leaf `PTE`, invalidate the TLB entry, and drop the `OWNED` frame reference.
  - `vspace_handle_cow_fault(vs, addr)`: Resolves a write fault on a present `VSPACE_PROT_COW` page.
  - `vspace_remap_user(vs, vaddr, new_flags, ...)`: Changes protection of an existing present user leaf, resolving CoW first if the new protection adds write.
  - `vspace_destroy(vs)`: Frees user-half page tables, releases `OWNED` leaf frames, and decrements frame references.

### 4.2 `task_t` — Resource & Isolation Container
A passive resource container that owns an address space, a capability table, and zero or more execution threads. A `task_t` never runs on a CPU directly; its threads do.
```c
typedef struct task {
    uint64_t      id;
    vspace_t     *vspace;         /* Isolated virtual address space */
    cap_table_t  *caps;           /* Capability table */
    thread_t     *threads;        /* Member thread list */
    uint32_t      thread_count;
    uint32_t      affinity_cpu;   /* Owning CPU, or TASK_AFFINITY_ANY */
    spinlock_t    thread_lock;    /* Guards threads[] and thread_count */
    bool          terminating;
    void         *personality_data; /* Opaque pointer for BSD struct bsd_proc */
} task_t;
```
A task's threads default to the CPU that created the task. Only a task explicitly marked `TASK_AFFINITY_ANY` is placed on any online core; this keeps the IPC rendezvous fast path sound (see `ipc.c`).

### 4.3 `thread_t` — Schedulable Execution Unit
```c
typedef enum thread_state {
    THREAD_READY = 0,
    THREAD_RUNNING,
    THREAD_BLOCKED_SEND,
    THREAD_BLOCKED_RECV,
    THREAD_BLOCKED_REPLY,
    THREAD_BLOCKED_NOTIFY,
    THREAD_SLEEPING,
    THREAD_DEAD
} thread_state_t;

typedef struct cpu_context {
    uint64_t r15, r14, r13, r12, rbx, rbp;
    uint64_t rip;
} cpu_context_t;

typedef struct thread {
    uint64_t        tid;
    task_t         *owner_task;
    cpu_context_t  *kstack_ctx;      /* Saved stack pointer for switch_context */
    uint8_t        *kstack_base;     /* Guarded stack slot base (guard page) */
    uint64_t        kstack_top;      /* Top of stack (written to TSS.rsp0) */
    thread_state_t  state;
    uint64_t        wakeup_tick;     /* LAPIC tick deadline for sleeping threads */
    uint64_t        ipc_msg_regs[4]; /* Inline IPC message payload */
    uint64_t        ipc_badge;
    struct thread  *reply_target;
    struct thread  *ipc_sender;
    sys_status_t    ipc_status;
    int64_t         exit_code;
    void           *ipc_wait_object; /* Retained endpoint/notification while waiting */
    uint32_t        ipc_wait_kind;   /* SEND/RECV/REPLY/NOTIFY wait reason */
    uint32_t        cpu_index;       /* CPU whose run queue owns this thread */
    uint64_t        fs_base;         /* IA32_FS_BASE for user-space TLS */
    void          (*entry)(void *arg);
    void           *arg;
    struct thread  *sched_next;
    struct thread  *wait_next;       /* Endpoint/notification wait-queue link */
    struct thread  *task_next;       /* Parent task thread-list link */
} thread_t;
```
`thread_create_with_fs_base()` publishes the inherited TLS base before the thread can be scheduled on any CPU (used by `fork()`).

### 4.4 `cap_t` & `cap_table_t` — Capability-Based Access Control
Tasks hold opaque integer handles (`cap_t`) indexing into a private `cap_table_t` of 256 slots (`CAP_TABLE_SLOTS`). Slot 0 (`CAP_NULL`) never holds a capability. Each slot binds a kernel object pointer to an immutable rights bitmask and an optional **badge** (identifying the sender to a server).
```c
typedef uint32_t cap_t;
#define CAP_NULL ((cap_t)0)

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

typedef struct cap_slot {
    mk_object_t   *object;  /* Common refcounted object header */
    uint64_t       rights;
    uint64_t       badge;
    cap_obj_type_t type;
} cap_slot_t;
```

### 4.5 `endpoint_t` & `notification_t` — Inter-Process Communication
1. **Synchronous Endpoints (`endpoint_t`):** rendezvous message passing (`ipc_call`, `ipc_recv`, `ipc_reply`). Transfers 4 x 64-bit message words directly through CPU registers without copying intermediate kernel buffers. Supports **badge minting**: when a server grants a derived endpoint capability with a custom `badge`, every incoming `ipc_call` delivers that `badge` to the receiver.
2. **Asynchronous Notifications (`notification_t`):** an interrupt-safe 64-bit word (`uint64_t word`, protected by `irq_save`/`irq_restore`) supporting non-blocking bitwise-OR signaling (`notify_signal`) and blocking wait (`notify_wait`). External IRQs may be bound to a notification bit via `irq_bind_notification()`; a `cap_irq_object_t` owns the binding's lifetime.

---

## 5. SMP & Locking Architecture

### 5.1 Ticket Spinlocks (`src/mk/spinlock.c`)
All multi-CPU exclusion uses a strictly FIFO **ticket spinlock** (`next_ticket` / `now_serving`), which bounds worst-case wait and prevents starvation. `spin_lock_irqsave()` disables interrupts and records the exact RFLAGS for `spin_unlock_irqrestore()`. `owner_cpu` and `locked` fields are diagnostics only and never used for exclusion. `cpu_relax()` (`pause`) is issued while spinning.

### 5.2 Global Lock Hierarchy
To prevent deadlock, locks are always acquired top-down:

- **Storage stack:** `file -> ext2 -> blk`.
  - `file_t.lock` (`src/bsd/vfs.h`) serializes a regular file's offset and I/O.
  - `ext2_lock` (`src/bsd/ext2.c`) serializes on-disk ext2 metadata.
  - The VirtIO-Blk driver uses an atomic `blk_busy` guard for the single request queue.
- **Core:** `sched/cap -> heap -> vspace -> PMM`.
  - Scheduler run-queue locks, `task->thread_lock`, and capability tables are the outermost core locks.
  - `heap_lock` (`kalloc.c`) is released before mapping new chunks so `vspace -> PMM` acquisition happens without the heap lock held.
  - `vspace_lock` (the global page-table lock) serializes all lower-half page-table/PTE mutation and keeps shootdown targets coherent.
  - `pmm_lock` is the innermost core lock.

Fine-grained per-process BSD state (`fd_table`, `mmap_regions`, `brk`, signal masks, `proc_table`) is serialized with `irq_save()`/`irq_restore()`.

### 5.3 Monotonic TLB Shootdown Protocol (`src/arch/x86_64/smp.c`, `vspace.c`)
Every vspace tracks `active_cpus`, the set of CPUs that may hold cached translations. When an address space is shared or a PTE changes, stale entries are flushed with a monotonic request/sequence protocol:

- `tlb_request_counter` is a global counter, monotonically increased with release ordering **after** the page-table change. `tlb_shootdown_mask(cpu_mask, vaddr)` allocates an id from it and only then sends the `IPI_TLB_SHOOTDOWN_VECTOR`.
- `tlb_service_local()` is the flush routine. It reads `tlb_request_counter` **before** flushing, reloads CR3, then publishes every serviced id into the per-CPU `tlb_flush_sequence[MAX_CPUS]` array using a compare-exchange loop. Re-reading after the flush and repeating (bounded to two passes) guarantees the published value always *predates* the flush, so an acknowledgement can never satisfy a request it did not actually cover.
- The waiter spins until every target's `tlb_flush_sequence[cpu] >= request`, servicing incoming requests itself (`tlb_service_local()`) so two interrupt-disabled CPUs cannot deadlock.
- `vspace_note_cpu_enter()`/`vspace_note_cpu_exit()`, called by the scheduler around CR3 switches, maintain `active_cpus`. CPUs that are not yet online are skipped (their first CR3 load starts with a cold TLB). `tlb_shootdown_service()` exposes the handler for callers that must flush locally.

Vspace mutations use `vspace_lock_acquire()`/`vspace_lock_release()`. Identity mappings for AP startup (`vspace_identity_map`/`vspace_identity_unmap`) broadcast a full shootdown when recycled.

### 5.4 Application Processor Bringup (`ap_trampoline.asm`, `smp.c`)
`smp_prepare()` copies the flat 16→32→64-bit trampoline to physical `0x8000`, installs a temporary 4 KiB identity leaf in `PML4[0]`, and registers the IPI handlers. `smp_bringup()` walks the cached ACPI MADT topology and, for each non-BSP CPU, fires **INIT-SIPI-SIPI** with `AP_SIPI_VECTOR = 0x8000 >> 12`.

Each AP:
1. runs the real-mode/32-bit prologue (A20, bootstrap GDT, PAE, load `CR3` from `params->cr3`, enable `EFER.LME`+`NXE`, enter long mode while still executing at the identity-mapped low page);
2. far-jumps into the kernel 64-bit code segment, loads the kernel GDT/IDT from the parameter block, and jumps to `ap_entry_c(cpu_index)` on its pre-allocated guarded stack;
3. in `ap_entry_c()`: `cpu_local_bind()` (per-CPU `GS_BASE`), `gdt_load_tss_for_cpu()` (per-CPU TSS), `idt_load()`, `cpu_init_ap_features()`, `syscall_init_ap()` (per-CPU `STAR`/`LSTAR`/`FMASK`), `lapic_init_ap()`, `sched_register_cpu()`, and `lapic_timer_start()` (per-CPU 1000 Hz timer);
4. switches onto its idle context, publishes itself online (`smp_cpu_park_ready`), after which the BSP reclaims the bootstrap stack and tears down the low identity window.

`cpu_local_t` (in `src/include/cpu.h`) holds `kernel_rsp` at offset 0 and `user_rsp_scratch` at offset 8 (fixed offsets used by `syscall.asm`/`switch.asm`), plus `cpu_index`, `lapic_id`, `current_thread`, `idle_thread`, `online`, and `self`. The IPC reschedule IPI (`IPI_RESCHEDULE_VECTOR = 0xF0`) and shootdown IPI (`IPI_TLB_SHOOTDOWN_VECTOR = 0xF1`) keep remote CPUs coherent.

---

## 6. Hardware I/O & Storage Architecture

### 6.1 PCI Configuration Scanner (`src/io/pci.c`)
`pci_init()` enumerates bus `0..255`, slot `0..31`, and every function of a multi-function device via configuration mechanism #1: the address is written to `0xCF8` (with `0x80000000` enable bit, bus/slot/func/offset fields) and the value read/written through `0xCFC`. `pci_cfg_read*`/`pci_cfg_write*` are serialized by a dedicated `pci_cfg_lock` spinlock. Base Address Registers are probed with an all-ones write to recover size and address, including 64-bit BAR pairs. Up to `PCI_MAX_DEVICES = 256` functions are cached; `pci_find_device(vendor, device)` and `pci_enable_bus_master()` are the driver-facing helpers.

### 6.2 ACPI MADT & IOAPIC External IRQ Routing (`acpi.c`, `ioapic.c`)
`acpi_discover_topology()` walks `RSDP -> RSDT/XSDT -> MADT`, validating every checksum, and caches: the LAPIC MMIO base, enabled CPU APIC ids, IOAPIC units (MMIO base and GSI base), and Interrupt Source Overrides. `ioapic_init()` (after `pci_init()`) masks the legacy 8259 pair, maps each IOAPIC MMIO window, and masks all redirection entries.

External interrupts are routed into **vectors `32..254`** (`IOAPIC_VECTOR_MIN..IOAPIC_VECTOR_MAX`). `ioapic_resolve_irq()` maps a legacy IRQ to its GSI through the MADT overrides; `ioapic_route_irq(gsi, vector, dest_lapic_id, active_low, level_triggered)` programs the redirection entry and unmasks it. The LAPIC timer uses its own vector and is never bound to the IOAPIC.

### 6.3 VirtIO-Blk DMA Driver (`src/io/virtio_blk.c`, `src/include/io/blkdev.h`)
`blk_init()` finds the transitional `0x1AF4:0x1001` PCI function, resets it, negotiates one legacy split virtqueue, and allocates a physically contiguous virtqueue, a request-control page, and a DMA **bounce buffer** (`VIRTIO_BLK_MAX_SECTORS = 128` sectors). Transfers stage data through the bounce buffer, publish a three-descriptor chain (header, data, status), notify the device, and wait for completion from the device IRQ (with a bounded poll fallback). The device IRQ is routed through the IOAPIC to `VIRTIO_BLK_IRQ_VECTOR = 0x40`. The driver exposes a driver-agnostic `blkdev_t { capacity_sectors, read, write }` through `blkdev_primary()`; a timed-out or failed request latches `blk.failed` so timed-out descriptors are never reused.

### 6.4 VFS Mount Hierarchy (`src/bsd/vfs.c`, `bsd_init.c`)
`vfs_init()` mounts the block device's **writable `ext2` root at `/`** when a disk is present. If `ext2_mount()` fails or no disk exists, `bsd_init()` falls back to the read-only **USTAR `initrd.tar`** Limine module as the root. `bsd_init()` then binds the static `/dev` directory node and the permanent `/dev/console` character node:

```text
/                 root (writable ext2, or read-only initrd USTAR fallback)
├── bin/          init, sh, echo (from ext2 image in Stage D builds)
├── etc/          motd, ...
├── dev/          virtual directory node
│   └── console   COM1-backed character device (fd 0/1/2 for PID 1)
└── tmp/          writable scratch directory (ext2 builds)
```

`ext2.c` implements direct (12), single-indirect, and double-indirect block mapping over `blkdev_t`, plus a triple-indirect constant reserved for future use. Block and inode allocation walk the group bitmaps and treat superblock/GDT/bitmap/inode-table blocks as reserved (`ext2_block_is_reserved`). An **open-entry table** keeps an inode alive after `unlink()` until the last live vnode reference is released (POSIX unlink-while-open semantics).

---

## 7. BSD Personality Integration Contract (`mk_hooks_t`)

The personality subsystem registers a single `mk_hooks_t` during boot (`src/include/mk_hooks.h`):

```c
#include <syscall.h>
#include <task.h>
#include <types.h>

typedef struct mk_hooks {
    /* Called when a Ring 3 thread invokes SYSCALL with bit 63 == 0. */
    void (*on_personality_syscall)(thread_t *caller, syscall_frame_t *frame);

    /* Called on a Ring 3 hardware fault. Returns true if handled. */
    bool (*on_user_exception)(thread_t *faulting_thread, uint64_t vector,
                              uint64_t error_code, uint64_t fault_addr);

    /* Called when the last thread of a task terminates. */
    void (*on_task_exit)(task_t *task, int64_t exit_status);
} mk_hooks_t;

void mk_register_hooks(const mk_hooks_t *hooks);
const mk_hooks_t *mk_hooks_get(void);
```

The full BSD integration behavior is specified in [`BSD_PERSONALITY.md`](BSD_PERSONALITY.md); the native ABI is in [`SYSCALL_ABI.md`](SYSCALL_ABI.md).
