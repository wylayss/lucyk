# GitHub Copilot Instructions — LucyK

`AGENT.md` is the single source of truth for this repository. Before proposing or editing
code, read `AGENT.md`, then `docs/ARCHITECTURE.md`, `docs/SYSCALL_ABI.md`,
`docs/BSD_PERSONALITY.md`, and `docs/BSD_SYSCALLS.md` (see also `CONTRIBUTING.md`).

## Non-negotiable rules

- Use freestanding C11 compiled with `-std=c11 -ffreestanding -nostdlib -nostdinc -Wall
  -Wextra -Werror` (and `-mcmodel=kernel -mno-red-zone -mno-mmx -mno-sse -mno-sse2
  -mno-80387`). Produce zero warnings and never pull in libc.
- Keep POSIX/BSD identifiers (`pid_t`, `fork`, `execve`, `vnode`, `inode`, `errno`, `bsd_*`,
  `BSD_*`, `sigaction`, `mprotect`, `pgid`) **out of** `src/mk/` and `src/arch/`.
- Never add `#include <bsd/...>` to `src/io/`; that layer sits below `src/bsd/`.
- Never use Mach terminology (`mach_*`, `kern_return_t`, `vm_map`, `pmap`, `task_self`,
  `bootstrap_port`).
- Never leave `TODO`, `FIXME`, `HACK`, `XXX`, stubs, empty bodies, or fake returns.
- Preserve strict `W^X`; use `copy_from_user`/`copy_to_user` for user memory; respect the
  lock hierarchy and `tlb_shootdown_mask()`.

## Verification

All changes must pass the repository acceptance gate:

```bash
./scripts/verify.sh      # or: make verify
```

This runs a clean warning-free build, an architectural boundary/hygiene scan, headless QEMU
boots on `-smp 1` and `-cpu max -smp 4`, and `e2fsck`. Do not mark work complete until all
four gates pass. When changing syscalls or ABI structures, update the matching document in
`docs/`.

## Git repository hygiene (mandatory)

Summary of `AGENT.md` §8 (Git Workflow & Branch Discipline) — `AGENT.md` remains the single
source of truth; this is not a replacement for the full spec. Every AI coding agent working
on `wylayss/lucyk` must:

- **never work on main — checkout feat/*, fix/*, audit/*, or docs/* first.** Run
  `git branch --show-current` before modifying any file; if it prints `main`, create and
  switch to a dedicated topic branch (`feat/<topic>`, `fix/<topic>`, `audit/<topic>`, or
  `docs/<topic>`).
- **Run `make verify` (`./scripts/verify.sh`) before committing.** Never create a commit
  until all four gates pass cleanly.
- **Use subsystem commit prefixes:** conventional commits scoped `mk:`, `arch:`, `io:`,
  `bsd:`, `user:`, `docs:`, or `chore:`. Syscall/ABI/locking-contract changes update
  `docs/*.md` in the same branch. Never stage build outputs (`build/`, `*.o`, `*.iso`,
  `*.img`).
- **Never auto-merge to `main`.** Merging into `main` and tagging releases are reserved for
  human maintainers after review. Never run `git push --force` or `git reset --hard` over
  unbacked local edits.
