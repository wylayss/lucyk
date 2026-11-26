# Microkernel Core (x86_64)

A clean-slate, standalone **x86_64 Microkernel** written in **C11** and **NASM Assembly**, booted via the **Limine Boot Protocol** in Higher-Half Kernel space (`-2 GiB`).

This kernel implements minimal, strictly isolated hardware and execution primitives—**Virtual Address Spaces (`vspace_t`)**, **Tasks (`task_t`)**, **Threads (`thread_t`)**, **Capabilities (`cap_t`)**, and **Endpoints (`endpoint_t`)**—while exposing a clean internal boundary (`mk_hooks_t`) that hosts a **BSD Personality Layer** (process model, VFS, signals, and POSIX syscalls) without polluting the microkernel core.

---

## Documentation Suite

| File | Purpose |
| :--- | :--- |
| [`AGENT.md`](AGENT.md) | Hard architectural constraints, compiler/ABI rules, memory invariants, layering rules, and build verification gates. |
| [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) | Directory tree, memory layout, core primitives, SMP & locking, and hardware I/O / storage architecture. |
| [`docs/SYSCALL_ABI.md`](docs/SYSCALL_ABI.md) | Native microkernel register convention, syscall numbers, capability object types, and `sys_status_t` codes. |
| [`docs/BSD_PERSONALITY.md`](docs/BSD_PERSONALITY.md) | BSD personality architecture, hooks, signals, process groups, TLS, `mprotect`, and the writable `ext2` filesystem. |
| [`docs/BSD_SYSCALLS.md`](docs/BSD_SYSCALLS.md) | BSD syscall table, ABI structures, errno codes, and user-pointer marshalling rules. |

---

## Target Repository Layout

```text
LucyK/
├── README.md
├── AGENT.md
├── Makefile
├── linker.ld
├── limine.conf
├── docs/
│   ├── ARCHITECTURE.md
│   ├── SYSCALL_ABI.md
│   ├── BSD_PERSONALITY.md
│   └── BSD_SYSCALLS.md
└── src/
    ├── kernel_main.c      # Higher-half entry point & subsystem bootstrap
    ├── include/           # Public headers (io/ and bsd/ subdirectories)
    │   ├── types.h        # sys_status_t, fixed-width types, alignment macros
    │   ├── klog.h         # Serial UART logger & kpanic declarations
    │   ├── syscall.h      # syscall_frame_t and syscall entry declarations
    │   ├── syscall_numbers.h  # Native microkernel syscall numbers
    │   └── mk_hooks.h     # Personality layer integration interface
    ├── arch/x86_64/       # GDT/TSS, IDT/ISR, PMM, vspace, SMP, ACPI, IOAPIC, serial
    ├── io/                # pci.c, virtio_blk.c (hardware I/O & block storage)
    ├── mk/                # task, sched, cap, ipc, kalloc, klog, spinlock, elf, syscall
    └── bsd/               # proc, vfs, ext2, initrd, console, mmap, syscall_bsd
```

---

## Build & Run Prerequisites

Ensure the following toolchain packages are installed on your Linux/WSL2 host:

- `clang` or `x86_64-elf-gcc` (freestanding C11 cross-compiler)
- `ld.lld` or `x86_64-elf-ld`
- `nasm` (Netwide Assembler)
- `xorriso` (for generating bootable ISO-9660 images)
- `mkfs.ext2` (for the writable ext2 root volume)
- `qemu-system-x86_64` (for testing and serial debugging)
- `git` (for fetching the official `limine` binary release branch)

### Standard QEMU Boot Command

```bash
# Single core (default)
make run

# 4-core SMP
make run-smp4

# Equivalently, by hand:
qemu-system-x86_64 \
  -M q35 \
  -m 512M \
  -cdrom build/os.iso \
  -serial stdio \
  -no-reboot \
  -no-shutdown
```
