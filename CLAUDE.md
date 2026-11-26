# CLAUDE.md — Claude Code Entry Point

**`AGENT.md` is the single source of truth** for LucyK's engineering rules. This file is a
pointer and quick summary; read the specification before changing anything.

## Read first

- [`AGENT.md`](AGENT.md) — full rules, ABI invariants, build/verification gate.
- [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) — subsystem map, memory layout, SMP/locking, I/O.
- [`docs/SYSCALL_ABI.md`](docs/SYSCALL_ABI.md) — native syscall ABI.
- [`docs/BSD_PERSONALITY.md`](docs/BSD_PERSONALITY.md) and
  [`docs/BSD_SYSCALLS.md`](docs/BSD_SYSCALLS.md) — BSD personality layer.
- [`CONTRIBUTING.md`](CONTRIBUTING.md) — workflow and AI-contribution policy.

Read them in full before editing anything under `src/` or `user/`.

## Non-negotiable rules (summary)

- **Freestanding C11** with `-std=c11 -ffreestanding -nostdlib -nostdinc -Wall -Wextra
  -Werror` (and `-mcmodel=kernel -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -mno-80387`).
  Zero warnings; no libc.
- **Zero POSIX/BSD identifiers in `src/mk/` or `src/arch/`** (`pid_t`, `fork`, `execve`,
  `vnode`, `inode`, `errno`, `bsd_*`, `BSD_*`, `sigaction`, `mprotect`, `pgid`).
- **Zero `#include <bsd/...>` in `src/io/`** (it sits below `src/bsd/`).
- **Zero Mach terminology** (`mach_*`, `kern_return_t`, `vm_map`, `pmap`, `task_self`,
  `bootstrap_port`).
- **Zero stubs/TODOs** — no `TODO`/`FIXME`/`HACK`/`XXX`, empty bodies, or fake returns.
- **Strict `W^X`** and bounds-checked user copies; respect lock ordering and TLB shootdown.

## Required verification

Before declaring any task complete, run:

```bash
./scripts/verify.sh      # or: make verify
```

All four gates must pass: clean build (0 warnings), boundary/hygiene scan, headless QEMU
boots on `-smp 1` and `-cpu max -smp 4` (Stage A–D self-tests, 0 leaked frames, `lucyk#`
prompt), and `e2fsck` integrity. Never finish with a failing or skipped gate. If you touch
an ABI/syscall, update the corresponding document under `docs/`.