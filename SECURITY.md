# Security Policy

LucyK is a **bare-metal x86_64 microkernel with a Ring 0 BSD personality layer**. A bug that
crosses a privilege or isolation boundary is a kernel security vulnerability, not an
ordinary crash. This document describes what is in scope and how to report issues
privately.

---

## 1. Supported Versions

LucyK is under active development on the `main` branch; only the latest `main` is supported
with security fixes. There are no released/stable versions yet.

| Version | Supported |
| :--- | :--- |
| `main` (latest) | :white_check_mark: |
| Older commits / tags | :x: |

---

## 2. Scope: Security-Sensitive Boundaries

Reports are in scope when they demonstrate a violation of one or more of the invariants
below. Please read [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md),
[`docs/SYSCALL_ABI.md`](docs/SYSCALL_ABI.md), and
[`docs/BSD_PERSONALITY.md`](docs/BSD_PERSONALITY.md) for the design contracts these
protect.

### 2.1 Ring 0 / Ring 3 isolation
- Escape from Ring 3 to Ring 0 (privilege escalation) other than the intended `SYSCALL`
  entry.
- Corrupting kernel state or another task's `vspace_t`, `cap_table_t`, `task_t`, or
  `thread_t` from Ring 3.
- Bypassing SMEP/SMAP, `CR0.WP`, or the guard pages protecting kernel stacks.

### 2.2 User-pointer validation (`copy_from_user` / `copy_to_user`)
- Reaching kernel memory through an unchecked user pointer (`buf`, `path`, `st`, `argv`,
  `envp`, ioctl argument, etc.).
- TOCTOU or partial-copy bugs where kernel code dereferences a Ring 3 address directly
  instead of marshalling it, or where length/range validation is bypassable.

### 2.3 `SYSCALL`/`SYSRET` canonical-address handling
- Returning to Ring 3 with a non-canonical `RIP`/`RSP`, or the fallback IRETQ path leaking
  privileges.
- `syscall_frame_t`/register-state confusion that lets a user control kernel control flow
  or `RFLAGS` (e.g. re-enabling privileged flags, `TF`, `DF`, `IOPL`, `NT`, `VM`).
- Per-CPU GS/`swapgs` invariant violations.

### 2.4 `sigreturn` frame sanitization (BSD personality)
- Register or flag restoration from a user-controlled `bsd_sigframe_t` that grants more
  privilege than the interrupted context had (e.g. unsanitized `RFLAGS`, non-canonical
  `RIP`/`RSP`, or unmasking `SIGKILL`/`SIGSTOP`).
- Signal-delivery paths that write the frame outside validated user memory.

### 2.5 CoW and MMU permissions
- Violating `W^X` (mapping a page both writable and executable, or otherwise executing
  user-controlled data at another's expense).
- Copy-on-write faults that let one process observe or write another process's private
  frame (cross-process isolation failure).
- Capability-`OWNED` frame or PTE refcount bugs that cause use-after-free, double-free, or
  frame aliasing across address spaces.
- Stale TLB entries or shootdown races that expose freed/remapped memory.

### 2.6 Capability rights
- Minting, deriving, or using a capability (`CAP_OBJ_ENDPOINT`, `CAP_OBJ_NOTIFICATION`,
  `CAP_OBJ_VSPACE`, `CAP_OBJ_IRQ`, `CAP_OBJ_FRAME`) that exceeds its rights mask
  (`CAP_RIGHT_READ`/`WRITE`/`CALL`/`GRANT`), or forging/confusing `cap_t` handles.
- Badge confusion that lets a client impersonate another client to a server.
- `SYS_VSPACE_MAP` granting more than the frame capability allows.

### 2.7 Anything else
General kernel memory-safety defects exploitable from unprivileged Ring 3 code
(out-of-bounds writes, use-after-free, uninitialized kernel memory disclosed to user space,
double-free of kernel objects/frames, etc.) are also in scope.

**Out of scope:** bugs that are only reachable by a privileged/malicious Ring 0 component,
missing hardening that requires an already-compromised kernel, purely functional bugs with
no security impact (use a [`bug_report`](.github/ISSUE_TEMPLATE/bug_report.md) instead), and
issues in third-party components (Limine, QEMU, the host toolchain) — report those upstream.

---

## 3. How to Report a Vulnerability

**Do not open a public GitHub issue, discussion, or pull request for an unpatched
vulnerability.** Public disclosure before a fix puts every user at risk.

Report privately through GitHub's built-in features on the repository
[`wylayss/lucyk`](https://github.com/wylayss/lucyk):

1. **Preferred — GitHub Private Vulnerability Reporting.** Go to the repository's
   **Security** tab → **Advisories** → **Report a vulnerability**. This opens a private
   advisory visible only to you and the maintainers.
2. **Alternative — GitHub Security Advisory (draft).** If private reporting is unavailable,
   contact the maintainer ([`@wylayss`](https://github.com/wylayss)) directly to arrange a
   private channel before sharing details.

### What to include

Please provide as much of the following as you can:

- **Summary** — the boundary crossed and the impact (e.g. "Ring 3 read of kernel memory via
  unchecked `ioctl` argument").
- **Affected subsystem** — `mk`, `arch`, `io`, `bsd`, or `user`, with `file:line`
  references where possible.
- **Reproduction** — the exact build (`make clean && make`), QEMU command and flags
  (`-smp`, `-cpu`, `-M`, drive setup), and any Ring 3 test program or input needed.
- **Full COM1 serial log** — the fault/panic trace, register dump, and the `[INFO]`
  lines around the failure.
- **Environment** — host OS, toolchain versions (`clang --version`, `nasm -v`,
  `qemu-system-x86_64 --version`), and the commit hash tested.
- **A proof of concept** if available, and any suggested fix or mitigation.

---

## 4. What to Expect

- **Acknowledgement** within a few days of your private report.
- **Triage** to confirm severity and scope, with follow-up questions if needed.
- **Coordinated disclosure:** we aim to publish a fix and a GitHub Security Advisory,
  crediting you unless you prefer to remain anonymous. Our target is to resolve confirmed
  high-severity issues within **90 days** of the initial report; complex kernel issues may
  take longer, and we will keep you updated.
- Please give us a reasonable window to ship a fix before any public disclosure.

Thank you for helping keep LucyK and its users safe.
