# Contributing to LucyK

Thanks for your interest in LucyK, a clean-slate **x86_64 microkernel with a BSD
personality layer**. This guide explains how the project is structured, how to build and
verify your work, and how to get a change merged. Please read it before opening a pull
request.

By participating, you agree to abide by our [`CODE_OF_CONDUCT.md`](CODE_OF_CONDUCT.md).
All project documentation lives in [`AGENT.md`](AGENT.md), [`README.md`](README.md), and the
[`docs/`](docs/) directory — start with [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) and
[`docs/SYSCALL_ABI.md`](docs/SYSCALL_ABI.md) to understand the design.

---

## 1. Ways to Contribute

- **Bug reports** — use the [`bug_report`](.github/ISSUE_TEMPLATE/bug_report.md) issue
  template.
- **Feature proposals** — use the
  [`feature_proposal`](.github/ISSUE_TEMPLATE/feature_proposal.md) issue template.
- **Code, tests, and documentation** — open a pull request (see §5).
- **Security issues** — do **not** open a public issue; follow
  [`SECURITY.md`](SECURITY.md).

If you are new, look for issues labelled `good first issue`. Hardware bring-up, self-test
coverage, and documentation fixes are all valuable.

---

## 2. Architecture & Layer Boundaries

LucyK enforces a strict layering. Violating it is a review blocker regardless of how well
the code works. The full rules are in [`AGENT.md`](AGENT.md) §2 and §5; the essentials are:

```text
user/ (Ring 3)        ->  syscall
src/bsd/ (Ring 0)     ->  BSD personality (process, vfs, ext2, signals, console)
src/io/               ->  hardware I/O & block storage (PCI, VirtIO-Blk)
src/mk/  src/arch/    ->  personality-neutral microkernel core & HAL
```

1. **Zero POSIX/BSD concepts in the core.** Code in `src/mk/`, `src/arch/x86_64/`, and
   `src/io/` MUST NEVER reference `pid_t`, `fork`, `exec`, `file`, `vnode`, `inode`,
   `socket`, `uid`, `gid`, or POSIX `errno` conventions. POSIX/BSD abstractions live
   strictly outside those directories and interact only through `mk_hooks_t` and capability
   syscalls.
2. **`src/io/` sits below `src/bsd/`.** `src/io/` (`pci.c`, `virtio_blk.c`, headers
   `src/include/io/*.h`) is the hardware I/O layer and **must never `#include <bsd/...>`** or
   reference any BSD type. The dependency arrow only points `src/bsd/ -> src/io/`.
3. **Zero Mach terminology** anywhere in code, comments, types, or filenames. Forbidden:
   `mach_*`, `port` (use `endpoint`), `task_self`, `vm_map`, `pmap`, `kern_return_t`, `MIG`,
   `bootstrap_port`. Use `sys_status_t`, `vspace_t`, `task_t`, `thread_t`, `endpoint_t`,
   `notification_t`, `cap_t`, `cap_table_t`.
4. **The personality attaches through the hook table only.** `src/bsd/` reaches the core
   through `src/include/mk_hooks.h` and the public primitives (`task.h`, `vspace.h`,
   `cap.h`, `ipc.h`, `elf.h`) — never by editing microkernel internals.

When adding a low-level capability the BSD layer needs (for example a new clone primitive),
expose it as a clean, generic primitive in `src/mk/` or `src/arch/x86_64/`, not as a
BSD-specific API.

---

## 3. Code Standards

### 3.1 Language & Compiler Flags
The kernel is freestanding **C11** compiled with clang (the `Makefile` defaults
`CC := clang`, `LD := ld.lld`). The exact kernel flags are mandatory:

```text
-std=c11 -ffreestanding -nostdlib -nostdinc -isystem $(clang -print-resource-dir)/include
-Wall -Wextra -Werror -O2 -g
-fno-stack-protector -fno-builtin -fno-pic -fno-pie
-mcmodel=kernel -mno-red-zone
-mno-mmx -mno-sse -mno-sse2 -mno-80387
-I src/include -I build/limine
```

- Only compiler-provided freestanding headers (`<stdint.h>`, `<stddef.h>`, `<stdbool.h>`,
  `<stdarg.h>`) are allowed. Do **not** pull in libc. Minimal `memcpy`/`memset`/`memmove`/
  `memcmp`/`strlen` live in `src/mk/` and `src/include/string.h`.
- No floating-point or SIMD in kernel space (`-mno-sse` etc. is not negotiable): interrupts
  and context switches must never corrupt user FPU/SSE state.
- `-Werror` is on: **your change must compile with zero warnings.**

### 3.2 Assembly
Assembly is **NASM x86_64**. Kernel and user objects are assembled as ELF64
(`nasm -f elf64 -g -F dwarf`); the AP bootstrap trampoline
(`src/arch/x86_64/ap_trampoline.asm`) is a flat binary (`nasm -f bin`) that runs in real
mode. Keep the System V AMD64 calling convention (`rdi`, `rsi`, `rdx`, `rcx`, `r8`, `r9`;
`rsp` 16-byte aligned before a `call`) and preserve callee-saved registers
(`rbx`, `rbp`, `r12`–`r15`).

### 3.3 Memory & Safety Invariants
- **Strict `W^X`:** never map a user page with both `VSPACE_PROT_WRITE` and
  `VSPACE_PROT_EXEC`. `vspace_map_user()` and `mprotect()` reject the combination.
- Zero every freshly allocated frame through the HHDM before mapping it.
- Always check `pmm_alloc_frame()` and `kalloc()` returns for `NULL`.
- Respect the lock hierarchy from [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) §5.2:
  `file -> ext2 -> blk` for storage, `sched/cap -> heap -> vspace -> PMM` for the core.
  Route inter-CPU TLB invalidation through `tlb_shootdown_mask()`.
- Route all user-pointer access through `copy_from_user()`/`copy_to_user()`; never
  dereference a Ring 3 address directly in Ring 0.

### 3.4 House Rules
- **No `TODO`, `FIXME`, `XXX`, stubbed functions, empty function bodies, fake return
  values, or commented-out logic.** If it is committed, it must be fully implemented and
  verifiable in QEMU.
- Match surrounding style: 4-space indentation, `snake_case` for functions and locals, and
  the existing brace/`switch` conventions. Do not reformat unrelated code.
- Keep diffs focused; one logical change per pull request. Do not add comments that merely
  restate the code.
- Do not alter `Makefile` build flags, the linker scripts, or the GDT selector ordering
  (`0x00` null, `0x08` kcode, `0x10` kdata, `0x18` udata, `0x20` ucode, then per-CPU TSS at
  `0x28 + 16*cpu_index`) without a dedicated discussion — they are `SYSRET`-critical.

---

## 4. Local Build & Verification Setup

### 4.1 Toolchain dependencies

Install the following (Debian/Ubuntu package names in parentheses):

| Tool | Purpose |
| :--- | :--- |
| `clang` (`clang`) and `ld.lld` (`lld`) | freestanding C11 compiler and linker |
| `nasm` (`nasm`) | x86_64 assembler |
| `xorriso` (`xorriso`) | bootable ISO-9660 image creation |
| `mkfs.ext2` / `mke2fs` (`e2fsprogs`) | writable ext2 root volume (`build/disk.img`) |
| `qemu-system-x86_64` (`qemu-system-x86`) | boot testing and serial debugging |
| `git` (`git`) | fetches the Limine `v8.x-binary` release on demand |

The build fetches Limine automatically into `build/limine` on the first `make`; the first
build therefore needs network access. `make clean` removes `build/`, including the Limine
clone, so a subsequent `make` re-clones it.

> `CC` and `LD` are hard-coded to `clang`/`ld.lld` in the `Makefile`, but can be overridden
> per invocation, e.g. `make CC=x86_64-elf-gcc LD=x86_64-elf-ld`.

### 4.2 Build

```bash
make clean && make
```

This builds the kernel (`build/lucyk.elf`), the Ring 3 test/user programs, the read-only
USTAR `initrd` (`build/initrd.tar`), the writable ext2 disk image (`build/disk.img`), and
the bootable ISO (`build/os.iso`). It must complete with **zero warnings** under
`-Wall -Wextra -Werror`.

### 4.3 Boot and test in QEMU

The `Makefile` provides ready-made targets. Every one boots with `-serial stdio
-no-reboot -no-shutdown`, so the kernel's `[INFO]` output and any panic appear on your
terminal:

```bash
make run        # default single-vCPU boot: -M q35 -m 512M
make run-smp1   # uniprocessor:            -smp 1
make run-smp4   # SMP:                     -cpu max -smp 4
make run-uefi   # UEFI boot via OVMF
```

`make run-smp4` is the exact `-cpu max -smp 4` configuration required for merge. The
underlying commands are, equivalently:

```bash
qemu-system-x86_64 -M q35 -m 512M -smp 1 -cdrom build/os.iso \
  -serial stdio -no-reboot -no-shutdown \
  -drive file=build/disk.img,if=none,id=hd0,format=raw \
  -device virtio-blk-pci,drive=hd0

qemu-system-x86_64 -M q35 -m 512M -cpu max -smp 4 -cdrom build/os.iso \
  -serial stdio -no-reboot -no-shutdown \
  -drive file=build/disk.img,if=none,id=hd0,format=raw \
  -device virtio-blk-pci,drive=hd0
```

### 4.4 Acceptance criteria

A change is ready for review only when **all** of the following hold:

- `make clean && make` succeeds with **0 warnings** (`-Wall -Wextra -Werror`).
- The kernel boots under **`-smp 1`** (`make run-smp1`) and **`-cpu max -smp 4`**
  (`make run-smp4`) with:
  - **0 panics** (no `kpanic`, no unhandled exception, no triple fault),
  - **0 deadlocks** (every SMP/CoW stress self-test completes within its deadline),
  - **0 leaked PMM frames** (the self-tests report `0 leaked frames` / `0 leaked PMM
    frames` at shutdown), and
  - no leaked `mk_object` handles or live pipes.
- New or changed behavior is covered by a self-test where practical (see `kernel_main.c`
  self-test helpers and the `user/bsd/init.c` acceptance suite).

Capture the relevant serial output (the `[INFO]`/self-test lines, or the full panic dump)
in your pull request description.

---

## 5. Branching, Commits & Pull Request Workflow

### 5.1 Branches

Branch off `main` and use one of these prefixes:

| Prefix | Use for |
| :--- | :--- |
| `feat/...` | new functionality |
| `fix/...` | bug fixes |
| `audit/...` | correctness/consistency hardening, refactors, self-test additions |
| `docs/...` | documentation only |

Examples: `feat/apic-timer-calibration`, `fix/cap-table-race`, `audit/tlb-shootdown`,
`docs/bsd-syscalls-table`.

### 5.2 Commits

Write focused commits with a subsystem prefix, matching the layer you touched:

```text
mk:      src/mk/        e.g. "mk: fix endpoint wait-queue unlink on thread exit"
arch:    src/arch/      e.g. "arch: clamp PID validation to canonical user range"
io:      src/io/        e.g. "io: bounded-poll fallback for virtio-blk completion"
bsd:     src/bsd/       e.g. "bsd: honor O_APPEND under file lock"
user:    user/          e.g. "user: print errno in sh"
docs:    docs/, *.md    e.g. "docs: document sigreturn RFLAGS sanitization"
```

Keep the subject line imperative and under ~72 characters, explain *why* (not just *what*)
in the body, and reference issues with `Fixes #123` / `Refs #123`. Sign off your commits
when you can (`git commit -s`) to certify the Developer Certificate of Origin.

### 5.3 Pull requests

Open PRs against `main` and fill in the
[`.github/PULL_REQUEST_TEMPLATE.md`](.github/PULL_REQUEST_TEMPLATE.md) checklist. A PR is
mergeable when:

1. CI/local build is warning-free and the QEMU self-tests pass on `-smp 1` and
   `-cpu max -smp 4`.
2. The layer boundaries in §2 are respected (a reviewer will check for BSD/POSIX leakage
   into `src/mk/`/`src/arch/`/`src/io/` and for `TODO`/`FIXME` stubs).
3. Documentation is updated (see §6).
4. At least one maintainer has approved the change.

Prefer small, reviewable PRs. Do not force-push over review history on a shared branch.

---

## 6. Documentation Duties

Documentation is part of the contract, not an afterthought. If you change a stable
interface, update the matching document in the same pull request:

- **Native syscall numbers, register semantics, capability rights, or `sys_status_t`
  codes** → update [`docs/SYSCALL_ABI.md`](docs/SYSCALL_ABI.md) (and
  `src/include/syscall_numbers.h` / `src/include/types.h`).
- **BSD syscall numbers, signatures, ioctls, errno values, or ABI structs** → update
  [`docs/BSD_SYSCALLS.md`](docs/BSD_SYSCALLS.md) (and `src/include/bsd/syscall_bsd.h` /
  `user/bsd/bsd_user.h`).
- **BSD personality behavior (signals, pgid, TLS, mmap, filesystem)** → update
  [`docs/BSD_PERSONALITY.md`](docs/BSD_PERSONALITY.md).
- **Subsystem layout, memory map, locking hierarchy, or AP bringup** → update
  [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md).

Keep the header comments in `src/include/bsd/*.h` and `src/include/*.h` consistent with the
documents they reference. Never edit files under `src/` or `user/` merely to satisfy a
documentation change without a corresponding functional reason.

---

## 7. Reporting Bugs & Getting Help

- Reproducible faults, panics, or build failures → the
  [`bug_report`](.github/ISSUE_TEMPLATE/bug_report.md) template (include the full COM1
  serial log and your exact QEMU command).
- Design ideas and new functionality → the
  [`feature_proposal`](.github/ISSUE_TEMPLATE/feature_proposal.md) template.
- Vulnerabilities → [`SECURITY.md`](SECURITY.md) only. **Do not file public issues for
  unpatched Ring 0 exploits.**

Thank you for helping make LucyK more correct, more secure, and easier to understand.
