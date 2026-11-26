# AGENTS.md — Agent Entry Point (Codex, Roo Code, OpenCode)

**`AGENT.md` is the single source of truth** for LucyK's engineering rules. This file is
only a pointer and quick summary — it does not replace the specification.

## Read before editing any code

1. [`AGENT.md`](AGENT.md) — full architectural rules, ABI invariants, and the build gate.
2. [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) — subsystem map, memory layout, SMP/locking, I/O.
3. [`docs/SYSCALL_ABI.md`](docs/SYSCALL_ABI.md) — native microkernel syscall ABI.
4. [`docs/BSD_PERSONALITY.md`](docs/BSD_PERSONALITY.md) and
   [`docs/BSD_SYSCALLS.md`](docs/BSD_SYSCALLS.md) — BSD personality layer and syscalls.
5. [`CONTRIBUTING.md`](CONTRIBUTING.md) — workflow, standards, and AI-contribution policy.

Do not modify files under `src/` or `user/` until you have read the above.

## Non-negotiable rules (summary — `AGENT.md` is authoritative)

- **Freestanding C11:** `-std=c11 -ffreestanding -nostdlib -nostdinc -Wall -Wextra -Werror`
  (plus `-mcmodel=kernel -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -mno-80387`). Zero
  warnings; no libc.
- **Zero POSIX/BSD identifiers in `src/mk/` or `src/arch/`:** no `pid_t`, `fork`, `execve`,
  `vnode`, `inode`, `errno`, `bsd_*`, `BSD_*`, `sigaction`, `mprotect`, `pgid`.
- **Zero `#include <bsd/...>` in `src/io/`:** `src/io/` sits below `src/bsd/` and stays
  personality-neutral.
- **Zero Mach terminology** anywhere: no `mach_*`, `kern_return_t`, `vm_map`, `pmap`,
  `task_self`, `bootstrap_port`.
- **Zero stubs/TODOs:** no `TODO`, `FIXME`, `HACK`, `XXX`, empty bodies, fake returns, or
  commented-out logic.
- **Strict `W^X`:** never map a page writable **and** executable; route user memory through
  `copy_from_user`/`copy_to_user`; respect the lock hierarchy and `tlb_shootdown_mask()`.

## Verify before you finish

Run the full acceptance gate from the repository root:

```bash
./scripts/verify.sh      # or: make verify
```

It must pass all four gates: clean build with 0 warnings; architectural boundary/hygiene
scan; headless QEMU boots on `-smp 1` and `-cpu max -smp 4` reaching the `lucyk#` prompt
with Stage A–D self-tests and 0 leaked frames; and `e2fsck` integrity. **Do not report a
task complete until it passes.** If you change an ABI/syscall, update the matching file in
`docs/` in the same change.