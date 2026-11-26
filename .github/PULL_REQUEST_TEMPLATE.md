<!--
Thanks for contributing to LucyK! Please read CONTRIBUTING.md before opening this PR.
Keep the diff focused on one logical change. Fill in the summary and tick every box below.
-->

## Summary

<!-- What does this change do, and why? Reference issues with "Fixes #123". -->

- Affected layer(s): <!-- src/mk | src/arch/x86_64 | src/io | src/bsd | user | docs -->
- Type: <!-- feat | fix | audit | docs -->

## Verification

<!-- Paste the relevant [INFO]/self-test lines or panic output below. -->

```text
$ make clean && make
<build output tail — must show 0 warnings>

$ make run-smp4
<serial output tail — self-tests must report 0 leaked frames>
```

## Checklist

- [ ] `make clean && make` completes with **0 warnings** under `-Wall -Wextra -Werror`.
- [ ] No BSD/POSIX identifiers leak into `src/mk/` or `src/arch/` (or `src/io/`); `src/io/`
      contains **no** `#include <bsd/...>`, and no Mach terminology was introduced.
- [ ] No `TODO`/`FIXME`/stubbed functions, empty bodies, fake returns, or commented-out
      logic were added.
- [ ] QEMU boots and passes self-tests under **`-smp 1`** (`make run-smp1`) **and**
      **`-cpu max -smp 4`** (`make run-smp4`) with **0 panics, 0 deadlocks, and 0 leaked
      PMM frames**.
- [ ] `W^X` is preserved (no page is mapped writable **and** executable); kernel lock
      hierarchy (`file -> ext2 -> blk`, `sched/cap -> heap -> vspace -> PMM`) is respected.
- [ ] If an ABI, syscall, struct, or memory/locking contract changed, the relevant files in
      `docs/` were updated in this PR (`docs/SYSCALL_ABI.md` for native syscalls,
      `docs/BSD_SYSCALLS.md` for BSD syscalls/ioctls/errno, `docs/BSD_PERSONALITY.md` and
      `docs/ARCHITECTURE.md` as applicable), along with the matching headers under
      `src/include/`.
- [ ] I have read and agree to the
      [Code of Conduct](https://github.com/wylayss/lucyk/blob/main/CODE_OF_CONDUCT.md).

## Security

- [ ] This PR does not disclose an unpatched vulnerability. (Security issues must be
      reported privately per the
      [Security Policy](https://github.com/wylayss/lucyk/blob/main/SECURITY.md), not in a
      public PR.)
