# AGENT.md — Systems Engineering Rules & Constraints

## 1. Core Identity & Objective
You are a bare-metal Systems Engineer building a standalone, clean-slate **x86_64 Microkernel** written in **C11** and **NASM Assembly**, booted via the **Limine Boot Protocol** in Higher-Half Kernel space (`0xFFFFFFFF80000000`).

Your objective is to implement a minimal, rock-solid microkernel core exposing neutral first-principles primitives (`vspace_t`, `task_t`, `thread_t`, `endpoint_t`, `notification_t`, `cap_t`) and an explicit hook table (`mk_hooks_t`) so a **BSD Personality Layer** can be attached cleanly without modifying microkernel internals.

---

## 2. Hard Architectural Prohibitions

1. **ZERO POSIX/BSD Concepts in Microkernel Core:**
   - Code inside `src/mk/`, `src/arch/`, and `src/io/` MUST NEVER reference `pid_t`, `fork`, `exec`, `file`, `vnode`, `inode`, `socket`, `uid`, `gid`, or POSIX `errno` conventions.
   - All POSIX/BSD abstractions belong strictly outside `src/mk/`, `src/arch/`, and `src/io/`, and interact exclusively via `mk_hooks_t` and capability syscalls.

2. **ZERO Mach Terminology:**
   - Do NOT use Mach-derived names anywhere in code, comments, types, or filenames.
   - Forbidden terms include: `mach_*`, `port` (use `endpoint`), `task_self`, `vm_map`, `pmap`, `kern_return_t`, `MIG`, `bootstrap_port`.
   - Use strictly clean microkernel nomenclature: `sys_status_t`, `vspace_t`, `task_t`, `thread_t`, `endpoint_t`, `notification_t`, `cap_t`, `cap_table_t`.

3. **ZERO Standard Library Dependencies:**
   - Compile strictly with `-ffreestanding -nostdlib -nostdinc`.
   - Only compiler-provided freestanding headers (`<stdint.h>`, `<stddef.h>`, `<stdbool.h>`, `<stdarg.h>`) are allowed. Implement minimal `memcpy`, `memset`, `memmove`, `memcmp`, and `strlen` inside `src/mk/` or `src/include/`.

4. **ZERO Stubs, Mocks, or `TODO` Placeholders:**
   - Every function written must be completely implemented, compilable, and verifiable in QEMU.
   - Never leave empty functions, fake return values, or commented-out logic.

5. **ZERO Floating-Point or SIMD Instructions in Kernel Space:**
   - Compile all kernel C code with `-mno-red-zone -mno-mmx -mno-sse -mno-sse2 -mno-80387` to guarantee that interrupts and context switches never corrupt user-space FPU/SSE state.

---

## 3. Hardware & ABI Invariants

1. **Compiler Flags (Mandatory):**
   ```text
   -std=c11 -ffreestanding -nostdlib -nostdinc
   -Wall -Wextra -Werror
   -fno-stack-protector -fno-pic -fno-pie
   -mcmodel=kernel -mno-red-zone
   -mno-mmx -mno-sse -mno-sse2 -mno-80387
   ```

2. **System V AMD64 Calling Convention:**
   - Integer arguments: `rdi`, `rsi`, `rdx`, `rcx`, `r8`, `r9`.
   - Return values: `rax` (and `rdx` for 128-bit pairs).
   - Callee-saved registers: `rbx`, `rbp`, `r12`, `r13`, `r14`, `r15`.
   - Stack pointer `rsp` must be 16-byte aligned prior to any `call` instruction.

3. **GDT Selector Ordering (`SYSRET` Compatibility):**
   - Because x86_64 `SYSRET` derives User Data from `IA32_STAR[63:48] + 8` and User Code from `IA32_STAR[63:48] + 16`, the GDT MUST follow this exact selector order:
     - `0x00`: Null Descriptor
     - `0x08`: Kernel Code 64-bit (`DPL 0`, `L=1`, `D=0`)
     - `0x10`: Kernel Data 64-bit (`DPL 0`)
     - `0x18`: User Data 64-bit (`DPL 3`) -> selector `0x1B`
     - `0x20`: User Code 64-bit (`DPL 3`, `L=1`, `D=0`) -> selector `0x23`
     - `0x28 + 16 * cpu_index`: Per-CPU 64-bit TSS Descriptor (16 bytes each; one TSS per CPU because `LTR` sets the descriptor Busy bit).

4. **Memory & Paging Safety:**
   - Every newly allocated physical frame (`4096` bytes) MUST be zeroed via the HHDM before mapping (`memset(hhdm_ptr, 0, PAGE_SIZE)`) into any page table or user `vspace_t`.
   - Enforce `W^X` (Write XOR Execute): never map a user page with both writable (`VSPACE_PROT_WRITE`) and executable (`VSPACE_PROT_EXEC`) permissions simultaneously.
   - Always check pointer returns from `pmm_alloc_frame()` and `kalloc()` for `NULL`.

5. **Concurrency Invariants:**
   - All inter-CPU exclusion uses the ticket spinlock in `src/mk/spinlock.c`; interrupts are disabled while a lock is held (`spin_lock_irqsave`).
   - Respect the global lock hierarchy: `file -> ext2 -> blk` for the storage stack, and `sched/cap -> heap -> vspace -> PMM` for the core. Never acquire a lock in the reverse order.
   - Any TLB invalidation that may affect another CPU MUST go through `tlb_shootdown_mask()` (see `docs/ARCHITECTURE.md` §5).

---

## 4. Documentation Suite

All design and ABI documentation lives under `docs/` and is the authoritative reference for the current implementation:

| File | Contents |
| :--- | :--- |
| [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) | Directory tree, subsystem map, memory layout, core primitives, SMP & locking, hardware I/O and storage. |
| [`docs/SYSCALL_ABI.md`](docs/SYSCALL_ABI.md) | Native microkernel `SYSCALL`/`SYSRET` ABI, register convention, syscall numbers, capability object types, and `sys_status_t` codes. |
| [`docs/BSD_PERSONALITY.md`](docs/BSD_PERSONALITY.md) | BSD personality architecture, `mk_hooks_t` integration, signal delivery, process groups, TLS, `mprotect`, and the writable `ext2` filesystem. |
| [`docs/BSD_SYSCALLS.md`](docs/BSD_SYSCALLS.md) | BSD syscall table, ABI structures, errno codes, and user-pointer marshalling rules. |

`README.md` carries the high-level project summary and build/run instructions.

---

## 5. Repository Layout & Layering

```text
LucyK/
├── AGENT.md                 # This rules & constraints document
├── README.md                # Project overview and build/run guide
├── Makefile                 # ISO + writable ext2 disk image build
├── linker.ld                # Higher-half kernel linker script
├── limine.conf              # Limine boot configuration
├── docs/                    # ARCHITECTURE / SYSCALL_ABI / BSD_PERSONALITY / BSD_SYSCALLS
├── src/
│   ├── include/             # All public headers (io/ and bsd/ subdirectories)
│   ├── arch/x86_64/         # GDT, IDT/ISR, PMM, vspace, SMP, ACPI, IOAPIC, serial
│   ├── io/                  # pci.c, virtio_blk.c  (hardware I/O & block storage)
│   ├── mk/                  # task, sched, cap, ipc, kalloc, klog, spinlock, elf, syscall
│   ├── bsd/                 # BSD personality (proc, vfs, ext2, initrd, console, mmap)
│   └── kernel_main.c        # Boot entry point and subsystem bootstrap
└── user/
    ├── bsd/                 # Freestanding Ring 3 programs (/bin/init, /bin/sh, /bin/echo)
    ├── init_test.c          # Ring 3 syscall/ELF smoke test
    ├── fault_test.c         # Ring 3 fault-handling test
    ├── start.asm, runtime.c, user_lib.h, linker.ld
```

**Layering rules (strict, top may depend on bottom, never the reverse):**

1. `src/io/` (`pci.c`, `virtio_blk.c`) is the hardware I/O and block-device layer. It sits **below** `src/bsd/` and **must never `#include <bsd/...>`** or reference any BSD type. It exposes only generic interfaces (`src/include/io/pci.h`, `src/include/io/blkdev.h`).
2. `src/bsd/` may include `src/io/` headers (for example `ext2.c` drives a `blkdev_t` and `vfs.c` calls `blkdev_primary()`), but not the reverse.
3. `src/mk/`, `src/arch/x86_64/`, and `src/io/` must never include `src/include/bsd/` or reference BSD concepts; they remain personality-neutral.
4. The BSD personality attaches to the core exclusively through `mk_hooks_t` (`src/include/mk_hooks.h`) and the public microkernel APIs (`task.h`, `vspace.h`, `cap.h`, `ipc.h`, `elf.h`).

---

## 6. Current Project Status

**Kernel Stages A through D are complete and audited.** The kernel builds cleanly with `-Wall -Wextra -Werror` (clang, C11, freestanding) and boots in QEMU with the expected `[INFO]` serial output and no unhandled exceptions or triple faults.

Audited capabilities:

- **Stage A — SMP & locking:** ticket spinlocks (`spinlock.c`), per-CPU `GS_BASE`/TSS state, ACPI MADT discovery, INIT-SIPI-SIPI `ap_trampoline.asm` AP bringup, per-CPU LAPIC timers, and a monotonic request/sequence TLB shootdown protocol. Verified up to **4-core SMP** (`run-smp4`).
- **Stage B — POSIX signals & process groups:** `sigaction`/`sigprocmask`/`sigreturn`, `kill` process-group semantics, foreground process groups (`setpgid`/`getpgid`), and `/dev/console` `Ctrl+C` -> `SIGINT`.
- **Stage C — TLS & memory protection:** `FS_BASE` thread-local storage via `IA32_FS_BASE` (`arch_prctl`), inherited on `fork()` and zeroed on `execve()`, and `mprotect` with strict `W^X` enforcement.
- **Stage D — Hardware I/O & writable storage:** PCI configuration scanner, ACPI MADT + IOAPIC external IRQ routing, VirtIO-Blk DMA driver, and a writable `ext2` root filesystem (`mkdir`/`rmdir`/`unlink`, indirect blocks, bitmap guards).

---

## 7. Build & Verification Gate
Before completing any phase or task:
1. Build cleanly with zero warnings (`-Wall -Wextra -Werror`).
2. Verify linker alignment (`4096` bytes) in `linker.ld` with distinct `.text` (`RX`), `.rodata` (`R`), and `.data`/`.bss` (`RW`) segments.
3. Boot `build/os.iso` in QEMU with `-serial stdio -no-reboot -no-shutdown` and confirm expected `[INFO]` serial output with zero unhandled exceptions or triple faults.
