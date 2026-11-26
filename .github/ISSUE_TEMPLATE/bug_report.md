---
name: Bug report
about: Report a reproducible fault, panic, hang, build failure, or incorrect behavior in LucyK
title: "[bug] "
labels: bug
assignees: ""
---

<!--
Before filing: search existing issues, and make sure this is not a security
vulnerability. Security issues must be reported privately per
https://github.com/wylayss/lucyk/blob/main/SECURITY.md — do NOT open a public issue.
-->

## Summary

<!-- A clear, one- or two-sentence description of the bug. -->

## Affected Subsystem

<!-- Tick the layer(s) involved. -->

- [ ] `mk` — microkernel core (`src/mk/`: task, sched, cap, ipc, kalloc, elf, spinlock)
- [ ] `arch` — HAL (`src/arch/x86_64/`: GDT/IDT, pmm, vspace, smp, acpi, ioapic, lapic, serial)
- [ ] `io` — hardware I/O & storage (`src/io/`: pci, virtio_blk)
- [ ] `bsd` — BSD personality (`src/bsd/`: proc, vfs, ext2, initrd, console, mmap, syscall_bsd)
- [ ] `user` — Ring 3 programs (`user/`, `user/bsd/`)
- [ ] build/tooling (`Makefile`, `linker.ld`, Limine configuration)

Relevant `file:line` references (if known):

## Build & Environment

- Commit / ref tested:
- Host OS and version:
- Toolchain versions:
  - `clang --version`:
  - `ld.lld --version`:
  - `nasm -v`:
  - `qemu-system-x86_64 --version`:
- Did `make clean && make` succeed, and with how many warnings?
  <details><summary>Build output tail</summary>

  ```text

  ```

  </details>

## QEMU Command & Flags

<!-- Paste the exact command. The Makefile targets are `make run` (default), `make run-smp1`
     (-smp 1), `make run-smp4` (-cpu max -smp 4), and `make run-uefi`. -->

```bash
# e.g. make run-smp4   —  eq. to:
qemu-system-x86_64 -M q35 -m 512M -cpu max -smp 4 -cdrom build/os.iso \
  -serial stdio -no-reboot -no-shutdown \
  -drive file=build/disk.img,if=none,id=hd0,format=raw \
  -device virtio-blk-pci,drive=hd0
```

- `-smp`/`-cpu` used: <!-- e.g. -smp 1, or -cpu max -smp 4 -->
- Reproduces on `-smp 1`? <!-- yes / no -->
- Reproduces on `-cpu max -smp 4`? <!-- yes / no -->

## Reproduction Steps

1.
2.
3.

<!-- Include any Ring 3 program, input, or disk/image setup needed. If you added a
     self-test to trigger it, link the branch/PR. -->

## Full COM1 Serial Log

<!-- Paste the COMPLETE serial output from boot to the fault, including all [INFO] lines,
     the panic/fault trace, and any register dump. This is usually the single most useful
     artifact. -->

<details><summary>Serial log (boot .. fault)</summary>

```text

```

</details>

## Expected vs Actual Behavior

- **Expected:**
- **Actual:**

## Additional Context

<!-- Anything else: bisected commit, frequency (always/intermittent), suspected cause,
     related issues. -->
