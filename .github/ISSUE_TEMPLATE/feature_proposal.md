---
name: Feature proposal
about: Propose a new capability, syscall, subsystem, or design change for LucyK
title: "[feature] "
labels: enhancement
assignees: ""
---

<!--
Use this template to pitch a design before writing code. For bugs use the bug_report
template. For security issues, report privately per
https://github.com/wylayss/lucyk/blob/main/SECURITY.md — do not open a public issue.
-->

## Problem Statement

<!-- What problem does this solve? Who benefits? Why is the status quo insufficient?
     Describe the need, not the implementation, first. -->

## Proposed Solution

<!-- Sketch the approach. Keep it high-level here; details go in the sections below. -->

## Architectural Layer

<!-- Which layer owns this feature, and does it respect the boundaries in AGENT.md §2/§5
     and docs/ARCHITECTURE.md? Tick the primary layer. -->

- [ ] `mk` — microkernel core (must stay POSIX/BSD-free; no Mach terminology)
- [ ] `arch` — HAL / x86_64 hardware abstraction
- [ ] `io` — hardware I/O & block storage (must never `#include <bsd/...>`)
- [ ] `bsd` — BSD personality layer
- [ ] `user` — Ring 3 programs / libc
- [ ] docs / tooling / build

Explain how the design preserves the layering (`src/io/` below `src/bsd/`; BSD attaches only
via `mk_hooks_t` and the public primitives `task.h`, `vspace.h`, `cap.h`, `ipc.h`, `elf.h`):

## ABI & Syscall Impact

<!-- Be explicit. "None" is a valid answer, but say why. -->

- Does this add or change a **native** microkernel syscall? If so, which number in
  `src/include/syscall_numbers.h`, and what are the register/argument and return semantics?
- Does this add or change a **BSD** syscall, `ioctl`, errno value, or user ABI struct? If so,
  list the number and signature (`src/include/bsd/syscall_bsd.h`, `user/bsd/bsd_user.h`).
- Which documents must change?
  - [ ] `docs/SYSCALL_ABI.md`
  - [ ] `docs/BSD_SYSCALLS.md`
  - [ ] `docs/BSD_PERSONALITY.md`
  - [ ] `docs/ARCHITECTURE.md`
  - [ ] `AGENT.md`
- New capability object type or right required? (`src/include/cap.h`)

## Security Considerations

<!-- How does this interact with the security-sensitive boundaries in SECURITY.md?
     Consider: Ring 3 -> Ring 0 entry, copy_from_user/copy_to_user validation, canonical
     address checks, user-pointer marshalling, W^X, CoW frame handling, capability rights,
     and whether any new surface is reachable from unprivileged Ring 3 code. -->

- New attack surface introduced?
- Does it enforce `W^X` and never map a page writable **and** executable?
- Are all user pointers validated before use?

## Locking & Concurrency Considerations

<!-- How does this behave under SMP? Consider the lock hierarchy in
     docs/ARCHITECTURE.md §5.2: `file -> ext2 -> blk` and
     `sched/cap -> heap -> vspace -> PMM`. -->

- Which locks are taken, and in what order? Does it fit the existing hierarchy?
- Does it introduce blocking operations while holding `irq_save()`/a spinlock?
- Does it require TLB shootdown (`tlb_shootdown_mask`) or new IPIs?
- Is new per-CPU state needed (`cpu_local_t`)?

## Alternatives Considered

<!-- Other designs you weighed and why you rejected them. -->

## Additional Context

<!-- Prior art in other kernels, references to Intel SDM chapters, related issues, and an
     implementation/testing plan (self-tests, QEMU configs, `-smp` coverage). -->
