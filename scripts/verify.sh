#!/usr/bin/env bash
#
# LucyK local acceptance gate.
#
# Runs four sequential gates:
#   1. Clean build:            make clean && make (0 warnings/errors, -Werror)
#   2. Boundary & hygiene:     no POSIX/BSD leaks, no Mach terms, no stubs
#   3. Headless QEMU boots:    -smp 1 and -cpu max -smp 4 reach the shell prompt
#                              with all Stage A-D self-tests and 0 leaked frames
#   4. Filesystem integrity:   e2fsck -fn build/disk.img
#
# Usage:
#   ./scripts/verify.sh        # or: make verify
#
# Environment overrides:
#   VERIFY_QEMU_TIMEOUT   per-boot wall-clock limit (default: 30s)
#   VERIFY_LOG_DIR        log directory (default: build/verify)
#
# This script never modifies source files. It only consumes the build tree and
# the writable ext2 image produced by `make`.

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

QEMU_TIMEOUT="${VERIFY_QEMU_TIMEOUT:-30s}"
LOG_DIR="${VERIFY_LOG_DIR:-build/verify}"
BUILD_LOG="$LOG_DIR/build.log"

if [[ -t 1 ]]; then
    C_RED=$'\033[31m' C_GRN=$'\033[32m' C_YEL=$'\033[33m'
    C_BLD=$'\033[1m' C_RST=$'\033[0m'
else
    C_RED="" C_GRN="" C_YEL="" C_BLD="" C_RST=""
fi

gate() { printf '\n%s==> %s%s\n' "$C_BLD" "$*" "$C_RST"; }
info() { printf '    %s\n' "$*"; }
ok()   { printf '    %sPASS%s %s\n' "$C_GRN" "$C_RST" "$*"; }
warn() { printf '    %sWARN%s %s\n' "$C_YEL" "$C_RST" "$*"; }
fail() { printf '\n%sFAIL%s %s\n' "$C_RED" "$C_RST" "$*" >&2; exit 1; }

# Fail if an extended-regex pattern matches anywhere under the given paths.
scan_forbidden() {
    local label="$1" pattern="$2"
    shift 2
    local matches
    if matches="$(grep -rInE -- "$pattern" "$@" 2>/dev/null)"; then
        printf '%s\n' "$matches" >&2
        fail "$label"
    fi
}

# ---------------------------------------------------------------------------
# Preflight
# ---------------------------------------------------------------------------
gate "Preflight: required toolchain"
for tool in make clang nasm xorriso qemu-system-x86_64 e2fsck; do
    command -v "$tool" >/dev/null 2>&1 || \
        fail "missing required tool: $tool (see CONTRIBUTING.md §4.1)"
done
info "make, clang, nasm, xorriso, qemu-system-x86_64, e2fsck present"
ok "toolchain available"

# ---------------------------------------------------------------------------
# Gate 1: clean build
# ---------------------------------------------------------------------------
gate "Gate 1/4: clean build (make clean && make)"
mkdir -p "$LOG_DIR"
make clean >/dev/null 2>&1 || fail "make clean failed"
# `make clean` removes build/, so recreate the log directory afterwards.
mkdir -p "$LOG_DIR"
if ! make >"$BUILD_LOG" 2>&1; then
    tail -n 100 "$BUILD_LOG" >&2
    fail "make failed (see $BUILD_LOG)"
fi
if grep -nE 'warning:' "$BUILD_LOG" >/dev/null; then
    grep -nE 'warning:' "$BUILD_LOG" | head -n 40 >&2
    fail "build produced warnings (must be 0 under -Wall -Wextra -Werror)"
fi
if grep -nE 'error:' "$BUILD_LOG" >/dev/null; then
    grep -nE 'error:' "$BUILD_LOG" | head -n 40 >&2
    fail "build produced errors (see $BUILD_LOG)"
fi
[[ -f build/os.iso ]]   || fail "build/os.iso was not produced"
[[ -f build/disk.img ]] || fail "build/disk.img was not produced"
ok "kernel, ISO and writable ext2 image built with 0 warnings"

# ---------------------------------------------------------------------------
# Gate 2: architectural boundary & hygiene scan
# ---------------------------------------------------------------------------
gate "Gate 2/4: architectural boundary & hygiene scan"

scan_forbidden "POSIX/BSD identifier leaked into src/mk or src/arch" \
    '\b(pid_t|fork|execve|vnode|inode|errno|sigaction|mprotect|pgid)\b|\b(bsd_|BSD_)' \
    src/mk src/arch
ok "no POSIX/BSD identifiers in src/mk or src/arch"

scan_forbidden 'src/io includes a BSD header (must stay below src/bsd/)' \
    '#include[[:space:]]*[<"]bsd/' \
    src/io
ok "src/io contains no #include <bsd/...>"

scan_forbidden "Mach terminology anywhere in src/" \
    '\bmach_|\bkern_return_t\b|\bvm_map\b|\bpmap\b|\btask_self\b|\bbootstrap_port\b' \
    src
ok "no Mach terminology in src/"

scan_forbidden "TODO/FIXME/HACK/XXX annotation in src/ or user/" \
    '\b(TODO|FIXME|HACK|XXX)\b' \
    src user
ok "no TODO/FIXME/HACK/XXX annotations"

# ---------------------------------------------------------------------------
# Gate 3: headless QEMU acceptance boots
# ---------------------------------------------------------------------------
gate "Gate 3/4: headless QEMU acceptance boots (-smp 1 and -cpu max -smp 4)"

boot_gate() {
    local name="$1"
    shift
    local log="$LOG_DIR/$name.log"
    local qemu_log="$LOG_DIR/$name.qemu.log"
    rm -f "$log" "$qemu_log"

    info "booting $name (limit $QEMU_TIMEOUT)"
    local rc=0
    timeout "$QEMU_TIMEOUT" qemu-system-x86_64 \
        -M q35 -m 512M "$@" \
        -cdrom build/os.iso \
        -display none -monitor none -no-reboot -no-shutdown \
        -serial "file:$log" \
        -drive file=build/disk.img,if=none,id=hd0,format=raw \
        -device virtio-blk-pci,drive=hd0 \
        >"$qemu_log" 2>&1 || rc=$?

    # timeout(1) returns 124 after killing the idle guest; accept that.
    if [[ $rc -ne 0 && $rc -ne 124 ]]; then
        cat "$qemu_log" >&2 || true
        fail "$name: qemu exited unexpectedly (rc=$rc)"
    fi
    [[ -s "$log" ]] || fail "$name: serial log is empty ($log)"

    if grep -qaiE 'panic|triple fault' "$log"; then
        grep -aiE 'panic|triple fault' "$log" | head -n 20 >&2
        fail "$name: panic or triple fault detected"
    fi

    local marker
    for marker in \
        'ext2: mounted at /' \
        'smp stress:' \
        'cow stress:' \
        'init: Stage C automated tests passed' \
        'init: Stage D2 ext2 persistence tests passed' \
        '0 leaked frames' \
        'lucyk#'; do
        if ! grep -qaF -- "$marker" "$log"; then
            tail -n 60 "$log" >&2 || true
            fail "$name: missing required marker: '$marker'"
        fi
    done
    ok "$name: Stage A-D self-tests passed, 0 leaked frames, shell prompt reached"
}

boot_gate smp1 -smp 1
boot_gate smp4 -cpu max -smp 4

# ---------------------------------------------------------------------------
# Gate 4: ext2 filesystem integrity
# ---------------------------------------------------------------------------
gate "Gate 4/4: ext2 filesystem integrity (e2fsck -fn)"
FS_LOG="$LOG_DIR/e2fsck.log"
if ! e2fsck -fn build/disk.img >"$FS_LOG" 2>&1; then
    cat "$FS_LOG" >&2
    fail "e2fsck reported filesystem errors or inconsistencies"
fi
ok "build/disk.img is a consistent ext2 filesystem"

printf '\n%sAll four verification gates passed.%s\n' "$C_GRN" "$C_RST"