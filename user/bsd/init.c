#include "bsd_user.h"

#include <display.h>
#include <syscall_numbers.h>

/* Native ABI values used by the Stage E tests (docs/SYSCALL_ABI.md). */
#define SYS_ERR_INVAL_USER      (-1)
#define SYS_ERR_CAP_RIGHTS_USER (-4)
#define SYS_CAP_RIGHT_READ      0x1ULL
#define SYS_CAP_RIGHT_ALL       0xFULL
#define PAGE_BYTES              4096ULL

#define PIPE_TEST_CHUNK   256
#define PIPE_TEST_CHUNKS  3
#define PIPE_TEST_TOTAL   (PIPE_TEST_CHUNK * PIPE_TEST_CHUNKS)

static volatile uint64_t exec_marker;
static char iobuf[512];
static char oversized_arg[300];

/* CoW test: cow_marker exercises user-mode write faults; the status page is
 * deliberately left untouched after fork so the kernel's wait4 status copy is
 * the first writer and must resolve the CoW PTE itself. */
static volatile uint64_t cow_marker;
static uint8_t cow_status_page[4096] __attribute__((aligned(4096)));

__attribute__((noreturn)) static void fail(int32_t code)
{
    write_text(2, "init: verification failed, exit(");
    write_signed(2, code);
    write_text(2, ")\n");
    user_exit((uint64_t)code);
}

/* Probes whether `/` is writable by creating and removing a scratch file.
 * Returns nonzero on a writable ext2 root, zero on a read-only initrd
 * fallback. Used to skip Stage D2 ext2 tests cleanly on read-only roots
 * while still enforcing them strictly whenever ext2 is mounted. */
static int root_writable(void)
{
    const char *probe = "/.lucyk_wtest";
    int64_t fd = bsd_open(probe, BSD_O_CREAT | BSD_O_WRONLY | BSD_O_TRUNC,
                          0644);

    if (fd < 0) {
        return 0;
    }

    (void)bsd_close((int32_t)fd);
    (void)bsd_unlink(probe);
    return 1;
}

/* fork + pipe: the child delays then writes three chunks while the parent
 * blocks in read(); the parent verifies the stream, EOF, and wait status. */
static void test_pipe_fork(void)
{
    int32_t fds[2];

    if (bsd_pipe(fds) != 0) {
        fail(10);
    }

    int64_t pid = bsd_fork();

    if (pid < 0) {
        fail(11);
    }

    if (pid == 0) {
        if (bsd_close(fds[0]) != 0) {
            user_exit(12);
        }

        raw_sleep_ms(40);

        for (int chunk = 0; chunk < PIPE_TEST_CHUNKS; chunk++) {
            uint8_t data[PIPE_TEST_CHUNK];

            for (int i = 0; i < PIPE_TEST_CHUNK; i++) {
                data[i] = (uint8_t)(chunk * 17 + i);
            }

            if (bsd_write(fds[1], data, sizeof(data)) !=
                (int64_t)sizeof(data)) {
                user_exit(13);
            }
            raw_sleep_ms(10);
        }

        if (bsd_close(fds[1]) != 0) {
            user_exit(14);
        }

        user_exit(0);
    }


    if (bsd_close(fds[1]) != 0) {
        fail(15);
    }

    uint8_t received[PIPE_TEST_CHUNK];
    size_t total = 0;

    while (total < PIPE_TEST_TOTAL) {
        int64_t bytes = bsd_read(fds[0], received, sizeof(received));

        if (bytes < 0) {
            fail(16);
        }
        if (bytes == 0) {
            break;
        }

        for (int64_t i = 0; i < bytes; i++) {
            size_t position = total + (size_t)i;
            uint8_t expected =
                (uint8_t)((position / PIPE_TEST_CHUNK) * 17 +
                          (position % PIPE_TEST_CHUNK));

            if (received[i] != expected) {
                fail(17);
            }
        }

        total += (size_t)bytes;
    }

    if (total != PIPE_TEST_TOTAL) {
        fail(18);
    }

    int32_t status = -1;

    if (bsd_wait4((int32_t)pid, &status, 0) != pid) {
        fail(19);
    }
    if (status != 0) {
        fail(20);
    }

    uint8_t eof = 0;

    if (bsd_read(fds[0], &eof, 1) != 0) {
        fail(21);
    }
    if (bsd_close(fds[0]) != 0) {
        fail(22);
    }

    write_text(1,
               "init: fork+pipe blocking read/write, EOF and wait4 verified\n");
}

static void test_exec_echo(void)
{
    int64_t pid = bsd_fork();

    if (pid < 0) {
        fail(30);
    }

    if (pid == 0) {
        char *argv[3];

        argv[0] = "echo";
        argv[1] = "phase3-exec-ok";
        argv[2] = NULL;

        (void)bsd_execve("/bin/echo", argv, NULL);
        user_exit(31);
    }

    int32_t status = -1;

    if (bsd_wait4((int32_t)pid, &status, 0) != pid) {
        fail(32);
    }
    if (status != 0) {
        fail(33);
    }

    write_text(1, "init: fork+execve(/bin/echo)+wait4 verified\n");
}

static void test_exec_failure(void)
{
    int64_t pid = bsd_fork();

    if (pid < 0) {
        fail(40);
    }

    if (pid == 0) {
        exec_marker = 0x123456789ABCDEF0ULL;

        char *argv[2];

        argv[0] = "nonexistent";
        argv[1] = NULL;

        int64_t result = bsd_execve("/nonexistent", argv, NULL);

        if (result != -BSD_ENOENT) {
            user_exit(41);
        }
        if (exec_marker != 0x123456789ABCDEF0ULL) {
            user_exit(42);
        }

        write_text(1, "init: failed execve preserved the address space\n");
        user_exit(0);
    }

    int32_t status = -1;

    if (bsd_wait4((int32_t)pid, &status, 0) != pid) {
        fail(43);
    }
    if (status != 0) {
        fail(44);
    }

    write_text(1, "init: failed execve ENOENT/preservation verified\n");
}

static void test_wait_nohang(void)
{
    int64_t pid = bsd_fork();

    if (pid < 0) {
        fail(50);
    }

    if (pid == 0) {
        raw_sleep_ms(150);
        user_exit(0);
    }

    int32_t status = -1;

    if (bsd_wait4((int32_t)pid, &status, BSD_WNOHANG) != 0) {
        fail(51);
    }
    if (bsd_wait4((int32_t)pid, &status, 0) != pid) {
        fail(52);
    }
    if (status != 0) {
        fail(53);
    }

    write_text(1, "init: wait4 WNOHANG and blocking wake verified\n");
}

static void test_exec_bad_pointer(void)
{
    int64_t pid = bsd_fork();

    if (pid < 0) {
        fail(60);
    }

    if (pid == 0) {
        exec_marker = 0xABCDEF0123456789ULL;

        char *argv[2];
        argv[0] = "echo";
        argv[1] = NULL;

        int64_t result = bsd_execve(
            "/bin/echo", (char *const *)(uintptr_t)0xFFFFFFFF80000000ULL,
            NULL);

        if (result != -BSD_EFAULT) {
            user_exit(61);
        }

        result = bsd_execve(
            "/bin/echo", argv,
            (char *const *)(uintptr_t)0xFFFFFFFF80000000ULL);

        if (result != -BSD_EFAULT) {
            user_exit(62);
        }
        if (exec_marker != 0xABCDEF0123456789ULL) {
            user_exit(63);
        }

        write_text(1, "init: execve rejected kernel argv/envp pointers\n");
        user_exit(0);
    }

    int32_t status = -1;

    if (bsd_wait4((int32_t)pid, &status, 0) != pid) {
        fail(64);
    }
    if (status != 0) {
        fail(65);
    }

    write_text(1, "init: execve bad user pointers -> EFAULT verified\n");
}

static void test_exec_oversized(void)
{
    int64_t pid = bsd_fork();

    if (pid < 0) {
        fail(70);
    }

    if (pid == 0) {
        exec_marker = 0x1234ABCD5678EF90ULL;

        char *argv[18];

        for (int i = 0; i < 17; i++) {
            argv[i] = "arg";
        }
        argv[17] = NULL;

        int64_t result = bsd_execve("/bin/echo", argv, NULL);
        if (result != -BSD_E2BIG) {
            user_exit(71);
        }

        for (int i = 0; i < (int)sizeof(oversized_arg) - 1; i++) {
            oversized_arg[i] = 'a';
        }
        oversized_arg[sizeof(oversized_arg) - 1] = '\0';

        char *long_argv[2];
        long_argv[0] = oversized_arg;
        long_argv[1] = NULL;

        result = bsd_execve("/bin/echo", long_argv, NULL);
        if (result != -BSD_E2BIG) {
            user_exit(72);
        }
        if (exec_marker != 0x1234ABCD5678EF90ULL) {
            user_exit(73);
        }

        write_text(1, "init: oversized execve arguments -> E2BIG verified\n");
        user_exit(0);
    }

    int32_t status = -1;

    if (bsd_wait4((int32_t)pid, &status, 0) != pid) {
        fail(74);
    }
    if (status != 0) {
        fail(75);
    }

    write_text(1, "init: execve E2BIG preserved the address space\n");
}

static void test_wait_bad_status(void)
{
    int64_t pid = bsd_fork();

    if (pid < 0) {
        fail(80);
    }

    if (pid == 0) {
        user_exit(0);
    }

    if (bsd_wait4((int32_t)pid,
                  (int32_t *)(uintptr_t)0xFFFFFFFF80000000ULL,
                  0) != -BSD_EFAULT) {
        fail(81);
    }

    int32_t status = -1;

    if (bsd_wait4((int32_t)pid, &status, 0) != pid) {
        fail(82);
    }
    if (status != 0) {
        fail(83);
    }

    write_text(1, "init: wait4 bad status -> EFAULT, zombie preserved\n");
}

static void test_orphan_adoption(void)
{
    int64_t middle = bsd_fork();

    if (middle < 0) {
        fail(90);
    }

    if (middle == 0) {
        int64_t leaf = bsd_fork();

        if (leaf < 0) {
            user_exit(91);
        }
        if (leaf == 0) {
            raw_sleep_ms(80);
            user_exit(0);
        }

        user_exit(0);
    }

    int32_t status = -1;

    if (bsd_wait4((int32_t)middle, &status, 0) != middle) {
        fail(92);
    }
    if (status != 0) {
        fail(93);
    }

    int64_t adopted = bsd_wait4(-1, &status, 0);

    if (adopted <= 0) {
        fail(94);
    }
    if (status != 0) {
        fail(95);
    }

    write_text(1, "init: orphan adoption to PID 1 and wait4(-1) verified\n");
}

static void test_cow_fork(void)
{
    /* Poison the page before fork so a successful wait4 status copy is
     * observable: the parent never touches this page after fork, so only the
     * kernel's copy_to_user can turn the 0x5A filler into status 0. */
    for (size_t i = 0; i < sizeof(cow_status_page); i++) {
        cow_status_page[i] = 0x5A;
    }

    cow_marker = 0x1122334455667788ULL;

    int64_t pid = bsd_fork();

    if (pid < 0) {
        fail(110);
    }

    if (pid == 0) {
        if (cow_marker != 0x1122334455667788ULL) {
            user_exit(111);
        }

        cow_marker = 0x3333444455556666ULL;

        if (cow_marker != 0x3333444455556666ULL) {
            user_exit(112);
        }

        user_exit(0);
    }

    if (cow_marker != 0x1122334455667788ULL) {
        fail(113);
    }

    cow_marker = 0x777788889999AAAAULL;

    if (cow_marker != 0x777788889999AAAAULL) {
        fail(114);
    }

    if (bsd_wait4((int32_t)pid, (int32_t *)(void *)cow_status_page, 0) != pid) {
        fail(115);
    }

    volatile int32_t *status = (volatile int32_t *)(void *)cow_status_page;

    if (*status != 0) {
        fail(116);
    }

    write_text(1,
               "init: CoW fork isolated data; kernel copy_to_user resolved CoW\n");
}

static void test_fork_storm(void)
{
    for (int i = 0; i < 16; i++) {
        int64_t pid = bsd_fork();

        if (pid < 0) {
            fail(100);
        }

        if (pid == 0) {
            user_exit(0);
        }

        int32_t status = -1;

        if (bsd_wait4((int32_t)pid, &status, 0) != pid) {
            fail(101);
        }
        if (status != 0) {
            fail(102);
        }
    }

    write_text(1, "init: 16 rapid fork/exit/wait4 cycles verified\n");
}

/* ====================================================================== */
/* Kernel Stage C Tests                                                    */
/* ====================================================================== */

/* Signal handler trampoline: invokes sigreturn with the frame pointer. */
static void sig_restorer(void)
{
    /* When the signal handler returns via `ret`, it pops the restorer address
     * from the top of the sigframe. RSP now points 8 bytes into the sigframe
     * (past the restorer field). We subtract 8 to get back to the sigframe
     * start, then pass that as the first argument to sigreturn. */
    __asm__ volatile(
        "leaq -8(%%rsp), %%rdi\n"
        "movq %0, %%rax\n"
        "syscall\n"
        "ud2\n"
        :
        : "i"((uint64_t)BSD_SYS_SIGRETURN)
        : "rdi", "rax", "rcx", "r11", "memory");
    __builtin_unreachable();
}

static volatile uint32_t sigusr1_count;
static volatile uint64_t sigusr1_arg;

/* Signal handler that deliberately clobbers every caller-saved and callee-saved
 * register to prove sigreturn restores them all from the sigframe. The
 * function attribute naked ensures the compiler generates no prologue/epilogue
 * that might save/restore callee-saved registers and mask our clobber. We use
 * a regular function instead so the compiler handles the ABI; we clobber via
 * inline asm inside the body. */
static void sigusr1_handler(uint64_t signo)
{
    sigusr1_count++;
    sigusr1_arg = signo;

    /* Clobber every GPR the ABI says the callee must preserve (rbx, rbp,
     * r12-r15) and the caller-saved registers (r8-r11) with known poison
     * values. If sigreturn does not restore from the sigframe, the caller
     * will observe these poison values. */
    __asm__ volatile(
        "movq $0xBAD0BAD0BAD0BAD0, %%rbx\n"
        "movq $0xBAD1BAD1BAD1BAD1, %%rbp\n"
        "movq $0xBAD2BAD2BAD2BAD2, %%r12\n"
        "movq $0xBAD3BAD3BAD3BAD3, %%r13\n"
        "movq $0xBAD4BAD4BAD4BAD4, %%r14\n"
        "movq $0xBAD5BAD5BAD5BAD5, %%r15\n"
        "movq $0xBAD6BAD6BAD6BAD6, %%r8\n"
        "movq $0xBAD7BAD7BAD7BAD7, %%r9\n"
        "movq $0xBAD8BAD8BAD8BAD8, %%r10\n"
        "movq $0xBAD9BAD9BAD9BAD9, %%r11\n"
        :
        :
        : "rbx", "rbp", "r12", "r13", "r14", "r15",
          "r8", "r9", "r10", "r11", "memory");
}

static void test_sigaction_basic(void)
{
    sigusr1_count = 0;
    sigusr1_arg = 0;

    bsd_sigaction_t act;
    act.sa_handler = (uint64_t)(uintptr_t)sigusr1_handler;
    act.sa_mask = 0;
    act.sa_flags = BSD_SA_RESTORER;
    act._pad = 0;
    act.sa_restorer = (uint64_t)(uintptr_t)sig_restorer;

    bsd_sigaction_t old;

    if (bsd_sigaction(BSD_SIGUSR1, &act, &old) != 0) {
        fail(200);
    }

    /* Old action should have been SIG_DFL. */
    if (old.sa_handler != BSD_SIG_DFL) {
        fail(201);
    }

    /* Block SIGUSR1, verify sigprocmask. */
    uint64_t block_set = (1ULL << (BSD_SIGUSR1 - 1));
    uint64_t old_mask = 0;
    if (bsd_sigprocmask(BSD_SIG_BLOCK, &block_set, &old_mask) != 0) {
        fail(202);
    }

    /* Send SIGUSR1 to self. It should be pending but not delivered yet. */
    if (bsd_kill((int32_t)bsd_getpid(), BSD_SIGUSR1) != 0) {
        fail(203);
    }

    /* Should NOT have been delivered because it is blocked. */
    if (sigusr1_count != 0) {
        fail(204);
    }

    /* Load known sentinels into every callee-saved register before unblocking.
     * The signal handler will clobber them all with BADx patterns. If sigreturn
     * works correctly, these sentinels must survive the round-trip. */
    register uint64_t rbx_val __asm__("rbx") = 0xAAAA111122223333ULL;
    register uint64_t rbp_val __asm__("rbp") = 0xBBBB444455556666ULL;
    register uint64_t r12_val __asm__("r12") = 0xCCCC777788889999ULL;
    register uint64_t r13_val __asm__("r13") = 0xDDDDAAAABBBBCCCCULL;
    register uint64_t r14_val __asm__("r14") = 0xEEEEDDDDEEEEFFFFULL;
    register uint64_t r15_val __asm__("r15") = 0xFFFF000011112222ULL;

    /* Force the compiler to materialise the registers before the syscall. */
    __asm__ volatile("" : "+r"(rbx_val), "+r"(rbp_val), "+r"(r12_val),
                          "+r"(r13_val), "+r"(r14_val), "+r"(r15_val));

    /* Unblock SIGUSR1. The signal should now be delivered on the next
     * syscall return. */
    if (bsd_sigprocmask(BSD_SIG_UNBLOCK, &block_set, NULL) != 0) {
        fail(205);
    }

    /* Issue a trivial syscall so the signal delivery path runs. */
    (void)bsd_getpid();

    /* Force the compiler to read back the registers after the syscall. */
    __asm__ volatile("" : "+r"(rbx_val), "+r"(rbp_val), "+r"(r12_val),
                          "+r"(r13_val), "+r"(r14_val), "+r"(r15_val));

    if (sigusr1_count != 1) {
        fail(206);
    }
    if (sigusr1_arg != BSD_SIGUSR1) {
        fail(207);
    }

    /* Verify every callee-saved register was restored to its sentinel value
     * by sigreturn, despite the handler clobbering them all. */
    if (rbx_val != 0xAAAA111122223333ULL) {
        fail(240);
    }
    if (rbp_val != 0xBBBB444455556666ULL) {
        fail(241);
    }
    if (r12_val != 0xCCCC777788889999ULL) {
        fail(242);
    }
    if (r13_val != 0xDDDDAAAABBBBCCCCULL) {
        fail(243);
    }
    if (r14_val != 0xEEEEDDDDEEEEFFFFULL) {
        fail(244);
    }
    if (r15_val != 0xFFFF000011112222ULL) {
        fail(245);
    }

    /* Verify signal_mask was restored after sigreturn. */
    uint64_t current_mask = 0;
    if (bsd_sigprocmask(BSD_SIG_SETMASK, NULL, &current_mask) != 0) {
        fail(208);
    }

    /* Reset handler to SIG_DFL for cleanup. */
    bsd_sigaction_t dfl;
    dfl.sa_handler = BSD_SIG_DFL;
    dfl.sa_mask = 0;
    dfl.sa_flags = 0;
    dfl._pad = 0;
    dfl.sa_restorer = 0;
    (void)bsd_sigaction(BSD_SIGUSR1, &dfl, NULL);

    write_text(1,
               "init: sigaction+sigprocmask+sigreturn GPR restore verified\n");
}

static void test_pgid_sigint(void)
{
    int32_t parent_pid = (int32_t)bsd_getpid();

    /* Create a unique process group for this test. Use setpgid to put
     * ourselves in a group numbered after our pid. */
    int32_t test_pgid = parent_pid;
    if (bsd_setpgid(0, test_pgid) != 0) {
        fail(210);
    }
    if (bsd_getpgid(0) != test_pgid) {
        fail(211);
    }

    /* Ignore SIGINT in parent so group-kill does not kill us. */
    bsd_sigaction_t ign;
    ign.sa_handler = BSD_SIG_IGN;
    ign.sa_mask = 0;
    ign.sa_flags = 0;
    ign._pad = 0;
    ign.sa_restorer = 0;
    (void)bsd_sigaction(BSD_SIGINT, &ign, NULL);

    int64_t child = bsd_fork();
    if (child < 0) {
        fail(212);
    }

    if (child == 0) {
        /* Child inherits the parent's process group. SIGINT's default
         * action will kill us. */

        /* Reset SIGINT to SIG_DFL so the child terminates. */
        bsd_sigaction_t dfl;
        dfl.sa_handler = BSD_SIG_DFL;
        dfl.sa_mask = 0;
        dfl.sa_flags = 0;
        dfl._pad = 0;
        dfl.sa_restorer = 0;
        (void)bsd_sigaction(BSD_SIGINT, &dfl, NULL);

        /* Spin on BSD syscalls so signal delivery runs on each return. */
        for (int i = 0; i < 2000; i++) {
            (void)bsd_getpid();
            raw_sleep_ms(1);
        }
        /* If we get here, the signal was not delivered. */
        user_exit(0);
    }

    /* Give the child time to park. */
    raw_sleep_ms(50);

    /* Send SIGINT to our process group via pid == 0. */
    if (bsd_kill(0, BSD_SIGINT) != 0) {
        fail(213);
    }

    int32_t status = -1;
    int64_t reaped = bsd_wait4((int32_t)child, &status, 0);
    if (reaped != child) {
        fail(214);
    }
    /* Child should have been terminated by SIGINT. The wait4 status encoding
     * for signal death is: (128 + signo) & 0x7F == signo (the low 7 bits
     * of the raw exit_status). */
    if (status != BSD_SIGINT) {
        fail(215);
    }

    /* Restore SIGINT to SIG_DFL in parent. */
    bsd_sigaction_t dfl;
    dfl.sa_handler = BSD_SIG_DFL;
    dfl.sa_mask = 0;
    dfl.sa_flags = 0;
    dfl._pad = 0;
    dfl.sa_restorer = 0;
    (void)bsd_sigaction(BSD_SIGINT, &dfl, NULL);

    write_text(1,
               "init: process group SIGINT delivery verified (child killed)\n");
}

static void test_arch_prctl_tls(void)
{
    /* Set a TLS base. */
    static volatile uint64_t tls_data = 0xDEADBEEFCAFE1234ULL;
    uint64_t tls_addr = (uint64_t)(uintptr_t)&tls_data;

    if (bsd_arch_prctl(BSD_ARCH_SET_FS, tls_addr) != 0) {
        fail(220);
    }

    /* Read it back via arch_prctl. */
    uint64_t read_back = bsd_arch_prctl(BSD_ARCH_GET_FS, 0);
    if (read_back != tls_addr) {
        fail(221);
    }

    /* Read through %fs:0 to verify the actual MSR works. */
    uint64_t fs_value;
    __asm__ volatile("movq %%fs:0, %0" : "=r"(fs_value));
    if (fs_value != 0xDEADBEEFCAFE1234ULL) {
        fail(222);
    }

    /* Write through %fs:0 and verify. */
    uint64_t new_val = 0xAAAABBBBCCCCDDDDULL;
    __asm__ volatile("movq %0, %%fs:0" : : "r"(new_val) : "memory");
    if (tls_data != 0xAAAABBBBCCCCDDDDULL) {
        fail(223);
    }

    /* Test that fs_base is inherited across fork and survives context switches. */
    tls_data = 0x1111222233334444ULL;
    int64_t pid = bsd_fork();
    if (pid < 0) {
        fail(224);
    }

    if (pid == 0) {
        /* Child: should have inherited fs_base. */
        uint64_t child_fs;
        __asm__ volatile("movq %%fs:0, %0" : "=r"(child_fs));
        if (child_fs != 0x1111222233334444ULL) {
            user_exit(225);
        }

        /* Force a context switch. */
        raw_sleep_ms(10);

        /* Verify TLS survives context switch. */
        uint64_t after_switch;
        __asm__ volatile("movq %%fs:0, %0" : "=r"(after_switch));
        if (after_switch != 0x1111222233334444ULL) {
            user_exit(226);
        }

        user_exit(0);
    }

    int32_t status = -1;
    if (bsd_wait4((int32_t)pid, &status, 0) != pid) {
        fail(227);
    }
    if (status != 0) {
        fail(228);
    }

    /* Reset FS base. */
    (void)bsd_arch_prctl(BSD_ARCH_SET_FS, 0);

    write_text(1,
               "init: arch_prctl TLS read/write, fork inheritance, ctx switch verified\n");
}

static void test_mprotect(void)
{
    /* Allocate a page via mmap (RW). */
    int64_t addr = bsd_mmap(NULL, 4096, BSD_PROT_READ | BSD_PROT_WRITE,
                            BSD_MAP_PRIVATE | BSD_MAP_ANON, -1, 0);
    if (addr < 0) {
        fail(230);
    }

    volatile uint8_t *page = (volatile uint8_t *)(uintptr_t)addr;
    page[0] = 0x42;
    if (page[0] != 0x42) {
        fail(231);
    }

    /* Reject W^X (RWX). */
    int64_t result = bsd_mprotect((void *)(uintptr_t)addr, 4096,
                                  BSD_PROT_READ | BSD_PROT_WRITE | BSD_PROT_EXEC);
    if (result != -BSD_EINVAL) {
        fail(232);
    }

    /* Transition RW -> RX. */
    result = bsd_mprotect((void *)(uintptr_t)addr, 4096,
                          BSD_PROT_READ | BSD_PROT_EXEC);
    if (result != 0) {
        fail(233);
    }

    /* Test CoW safety after fork: fork with an mmap page, then mprotect. */
    int64_t mmap2 = bsd_mmap(NULL, 4096, BSD_PROT_READ | BSD_PROT_WRITE,
                             BSD_MAP_PRIVATE | BSD_MAP_ANON, -1, 0);
    if (mmap2 < 0) {
        fail(234);
    }

    volatile uint8_t *page2 = (volatile uint8_t *)(uintptr_t)mmap2;
    page2[0] = 0xAB;

    int64_t pid = bsd_fork();
    if (pid < 0) {
        fail(235);
    }

    if (pid == 0) {
        /* Child: the page should be CoW. Try mprotect to RX. This must resolve
         * the CoW before granting new permissions. */
        int64_t res = bsd_mprotect((void *)(uintptr_t)mmap2, 4096,
                                   BSD_PROT_READ | BSD_PROT_EXEC);
        if (res != 0) {
            user_exit(236);
        }
        user_exit(0);
    }

    int32_t status = -1;
    if (bsd_wait4((int32_t)pid, &status, 0) != pid) {
        fail(237);
    }
    if (status != 0) {
        fail(238);
    }

    /* Parent's page should still be writable. */
    page2[0] = 0xCD;
    if (page2[0] != 0xCD) {
        fail(239);
    }

    /* Clean up. */
    (void)bsd_munmap((void *)(uintptr_t)addr, 4096);
    (void)bsd_munmap((void *)(uintptr_t)mmap2, 4096);

    write_text(1,
               "init: mprotect RW->RX, W^X rejection, CoW safety after fork verified\n");
}

static void test_ext2_motd(void)
{
    int64_t fd = bsd_open("/etc/motd", BSD_O_RDONLY, 0);

    if (fd < 0) {
        fail(300);
    }

    char buffer[128];
    int64_t bytes = bsd_read((int32_t)fd, buffer, sizeof(buffer) - 1);

    (void)bsd_close((int32_t)fd);

    if (bytes <= 0) {
        fail(301);
    }

    buffer[(size_t)bytes] = '\0';

    if (!str_eq_len(buffer, "LucyK ext2 root", 15)) {
        fail(302);
    }

    write_text(1, "init: /etc/motd read directly from ext2 verified\n");
}

static void test_ext2_indirect(void)
{
    enum { EXT2_TEST_BYTES = 64 * 1024 };
    static uint8_t pattern[EXT2_TEST_BYTES];
    static uint8_t readback[EXT2_TEST_BYTES];

    for (size_t i = 0; i < sizeof(pattern); i++) {
        pattern[i] = (uint8_t)((i * 31U + 0x5AU) ^ (i >> 5));
    }
    for (size_t i = 0; i < sizeof(readback); i++) {
        readback[i] = 0;
    }

    int64_t fd = bsd_open("/tmp/test_ext2.bin", BSD_O_CREAT | BSD_O_RDWR,
                          0644);

    if (fd < 0) {
        fail(310);
    }

    size_t written = 0;

    while (written < sizeof(pattern)) {
        size_t chunk = sizeof(pattern) - written;

        if (chunk > 4096) {
            chunk = 4096;
        }

        int64_t bytes = bsd_write((int32_t)fd, pattern + written, chunk);

        if (bytes <= 0) {
            fail(311);
        }

        written += (size_t)bytes;
    }

    if (bsd_close((int32_t)fd) != 0) {
        fail(312);
    }

    fd = bsd_open("/tmp/test_ext2.bin", BSD_O_RDONLY, 0);

    if (fd < 0) {
        fail(313);
    }

    size_t got = 0;

    while (got < sizeof(readback)) {
        size_t chunk = sizeof(readback) - got;

        if (chunk > 4096) {
            chunk = 4096;
        }

        int64_t bytes = bsd_read((int32_t)fd, readback + got, chunk);

        if (bytes <= 0) {
            fail(314);
        }

        got += (size_t)bytes;
    }

    (void)bsd_close((int32_t)fd);

    for (size_t i = 0; i < sizeof(pattern); i++) {
        if (pattern[i] != readback[i]) {
            fail(315);
        }
    }

    if (bsd_unlink("/tmp/test_ext2.bin") != 0) {
        fail(316);
    }

    write_text(1,
               "init: ext2 64 KiB write/read/verify via singly indirect blocks passed\n");
}

static void test_ext2_dirs(void)
{
    if (bsd_mkdir("/tmp/sub", 0755) != 0) {
        fail(320);
    }

    int64_t fd = bsd_open("/tmp/sub/file.tmp", BSD_O_CREAT | BSD_O_RDWR,
                          0644);

    if (fd < 0) {
        fail(321);
    }

    static const char payload[] = "ext2 directory test";

    if (bsd_write((int32_t)fd, payload, sizeof(payload)) !=
        (int64_t)sizeof(payload)) {
        fail(322);
    }
    if (bsd_close((int32_t)fd) != 0) {
        fail(323);
    }
    if (bsd_unlink("/tmp/sub/file.tmp") != 0) {
        fail(324);
    }
    if (bsd_rmdir("/tmp/sub") != 0) {
        fail(325);
    }

    write_text(1, "init: ext2 mkdir/unlink/rmdir verified\n");
}


static int write_exact(int32_t fd, const void *buffer, size_t length)
{
    const uint8_t *bytes = (const uint8_t *)buffer;
    size_t done = 0;

    while (done < length) {
        size_t chunk = length - done;

        if (chunk > 4096) {
            chunk = 4096;
        }

        int64_t written = bsd_write(fd, bytes + done, chunk);

        if (written <= 0) {
            return 0;
        }
        done += (size_t)written;
    }
    return 1;
}

static int read_exact(int32_t fd, void *buffer, size_t length)
{
    uint8_t *bytes = (uint8_t *)buffer;
    size_t done = 0;

    while (done < length) {
        size_t chunk = length - done;

        if (chunk > 4096) {
            chunk = 4096;
        }

        int64_t got = bsd_read(fd, bytes + done, chunk);

        if (got <= 0) {
            return 0;
        }
        done += (size_t)got;
    }
    return 1;
}

static int bytes_equal(const void *a, const void *b, size_t length)
{
    const uint8_t *x = (const uint8_t *)a;
    const uint8_t *y = (const uint8_t *)b;

    for (size_t i = 0; i < length; i++) {
        if (x[i] != y[i]) {
            return 0;
        }
    }
    return 1;
}

static void test_ext2_trunc_append(void)
{
    const char *path = "/tmp/trunc_append.bin";
    char buffer[32];

    int64_t fd = bsd_open(path, BSD_O_CREAT | BSD_O_RDWR, 0644);
    if (fd < 0) {
        fail(330);
    }
    if (!write_exact((int32_t)fd, "ABCDEFGH", 8)) {
        fail(331);
    }
    if (bsd_close((int32_t)fd) != 0) {
        fail(332);
    }

    fd = bsd_open(path, BSD_O_RDWR | BSD_O_TRUNC, 0644);
    if (fd < 0) {
        fail(333);
    }
    if (!write_exact((int32_t)fd, "xyz", 3)) {
        fail(334);
    }
    if (bsd_close((int32_t)fd) != 0) {
        fail(335);
    }

    for (size_t i = 0; i < sizeof(buffer); i++) {
        buffer[i] = 0;
    }
    fd = bsd_open(path, BSD_O_RDONLY, 0);
    if (fd < 0) {
        fail(336);
    }
    int64_t bytes = bsd_read((int32_t)fd, buffer, sizeof(buffer));
    if (bytes != 3 || !bytes_equal(buffer, "xyz", 3)) {
        fail(337);
    }
    if (bsd_read((int32_t)fd, buffer, sizeof(buffer)) != 0) {
        fail(338);
    }
    (void)bsd_close((int32_t)fd);

    fd = bsd_open(path, BSD_O_WRONLY | BSD_O_APPEND, 0);
    if (fd < 0) {
        fail(339);
    }
    if (!write_exact((int32_t)fd, "123", 3)) {
        fail(340);
    }
    if (bsd_close((int32_t)fd) != 0) {
        fail(341);
    }

    for (size_t i = 0; i < sizeof(buffer); i++) {
        buffer[i] = 0;
    }
    fd = bsd_open(path, BSD_O_RDONLY, 0);
    if (fd < 0) {
        fail(342);
    }
    bytes = bsd_read((int32_t)fd, buffer, sizeof(buffer));
    if (bytes != 6 || !bytes_equal(buffer, "xyz123", 6)) {
        fail(343);
    }
    (void)bsd_close((int32_t)fd);

    if (bsd_unlink(path) != 0) {
        fail(344);
    }

    write_text(1, "init: ext2 O_TRUNC and O_APPEND verified\n");
}

static void test_ext2_rmdir_nonempty(void)
{
    if (bsd_mkdir("/tmp/ne", 0755) != 0) {
        fail(350);
    }

    int64_t fd = bsd_open("/tmp/ne/child", BSD_O_CREAT | BSD_O_RDWR, 0644);
    if (fd < 0) {
        fail(351);
    }
    if (!write_exact((int32_t)fd, "child", 5)) {
        fail(352);
    }
    if (bsd_close((int32_t)fd) != 0) {
        fail(353);
    }

    if (bsd_rmdir("/tmp/ne") != -BSD_ENOTEMPTY) {
        fail(354);
    }
    if (bsd_unlink("/tmp/ne/child") != 0) {
        fail(355);
    }
    if (bsd_rmdir("/tmp/ne") != 0) {
        fail(356);
    }

    write_text(1, "init: ext2 rmdir non-empty returned -ENOTEMPTY\n");
}

static void test_ext2_unlink_open(void)
{
    const char *path = "/tmp/unlink_open.bin";

    int64_t fd = bsd_open(path, BSD_O_CREAT | BSD_O_RDWR, 0644);
    if (fd < 0) {
        fail(360);
    }
    if (!write_exact((int32_t)fd, "persist", 7)) {
        fail(361);
    }
    if (bsd_unlink(path) != 0) {
        fail(362);
    }
    if (bsd_lseek((int32_t)fd, 0, 0) != 0) {
        fail(363);
    }

    char buffer[8];
    for (size_t i = 0; i < sizeof(buffer); i++) {
        buffer[i] = 0;
    }
    if (!read_exact((int32_t)fd, buffer, 7) || !bytes_equal(buffer, "persist", 7)) {
        fail(364);
    }
    if (bsd_close((int32_t)fd) != 0) {
        fail(365);
    }

    write_text(1, "init: ext2 unlink-while-open preserved file data\n");
}

static size_t append_u32(char *buffer, size_t position, uint32_t value)
{
    char digits[10];
    size_t count = 0;

    if (value == 0) {
        buffer[position++] = '0';
        return position;
    }

    while (value > 0) {
        digits[count++] = (char)('0' + (value % 10U));
        value /= 10U;
    }
    while (count > 0) {
        buffer[position++] = digits[--count];
    }
    return position;
}

static void concurrent_name(char *buffer, uint32_t child, uint32_t iteration)
{
    const char *prefix = "/tmp/conc_";
    size_t position = 0;

    while (*prefix != '\0') {
        buffer[position++] = *prefix++;
    }
    position = append_u32(buffer, position, child);
    buffer[position++] = '_';
    position = append_u32(buffer, position, iteration);
    buffer[position++] = '.';
    buffer[position++] = 'b';
    buffer[position++] = 'i';
    buffer[position++] = 'n';
    buffer[position] = '\0';
}

static void test_ext2_concurrent(void)
{
    enum {
        CONC_PROCS = 4,
        CONC_ITERS = 12,
        CONC_BYTES = 8192
    };
    int64_t pids[CONC_PROCS];

    for (int32_t child = 0; child < CONC_PROCS; child++) {
        int64_t pid = bsd_fork();

        if (pid < 0) {
            fail(370);
        }

        if (pid == 0) {
            static uint8_t data[CONC_BYTES];
            static uint8_t check[CONC_BYTES];

            for (uint32_t iteration = 0; iteration < CONC_ITERS;
                 iteration++) {
                char path[64];

                concurrent_name(path, (uint32_t)child, iteration);

                for (size_t i = 0; i < CONC_BYTES; i++) {
                    data[i] = (uint8_t)((i + child * 17U +
                                         iteration * 31U) ^ 0xA5U);
                    check[i] = 0;
                }

                int64_t fd = bsd_open(path,
                                      BSD_O_CREAT | BSD_O_RDWR | BSD_O_TRUNC,
                                      0644);
                if (fd < 0) {
                    user_exit(180);
                }
                if (!write_exact((int32_t)fd, data, CONC_BYTES)) {
                    user_exit(181);
                }
                if (bsd_close((int32_t)fd) != 0) {
                    user_exit(182);
                }

                fd = bsd_open(path, BSD_O_RDONLY, 0);
                if (fd < 0) {
                    user_exit(183);
                }
                if (!read_exact((int32_t)fd, check, CONC_BYTES)) {
                    user_exit(184);
                }
                (void)bsd_close((int32_t)fd);

                if (!bytes_equal(data, check, CONC_BYTES)) {
                    user_exit(185);
                }
                if (bsd_unlink(path) != 0) {
                    user_exit(186);
                }
            }

            user_exit(0);
        }

        pids[child] = pid;
    }

    for (int32_t child = 0; child < CONC_PROCS; child++) {
        int32_t status = -1;
        int64_t reaped = bsd_wait4((int32_t)pids[child], &status, 0);

        if (reaped != pids[child] || status != 0) {
            fail(371);
        }
    }

    write_text(1,
               "init: ext2 4-process concurrent create/write/read/unlink passed\n");
}


/* ------------------------------------------------------------------------- */
/* Kernel Stage E: FPU/SSE, QoS + priority inheritance, capability handoff,   */
/* /proc and /sys, execve envp                                                */
/* ------------------------------------------------------------------------- */

#define SSE_ITERATIONS 250000ULL
#define SSE_ROUNDS     4U
#define SSE_CHILDREN   3

#define STAGE_E_INFO_VA 0x0000007300000000ULL
#define STAGE_E_FB_VA   0x0000007400000000ULL
#define STAGE_E_PROT_WRITE 1ULL

typedef struct native_result {
    int64_t  status;
    uint64_t rdx;
    uint64_t rsi;
    uint64_t r8;
    uint64_t r9;
    uint64_t r10;
} native_result_t;

/* Native microkernel syscall (bit 63 set) returning every result register. */
static native_result_t native_call(uint64_t number, uint64_t a1, uint64_t a2,
                                   uint64_t a3, uint64_t a4, uint64_t a5)
{
    register uint64_t rdi __asm__("rdi") = a1;
    register uint64_t rsi __asm__("rsi") = a2;
    register uint64_t rdx __asm__("rdx") = a3;
    register uint64_t r10 __asm__("r10") = a4;
    register uint64_t r8 __asm__("r8") = a5;
    register uint64_t r9 __asm__("r9") = 0;
    uint64_t rax = number;

    __asm__ volatile("syscall"
                     : "+a"(rax), "+r"(rdi), "+r"(rsi), "+r"(rdx), "+r"(r10),
                       "+r"(r8), "+r"(r9)
                     :
                     : "rcx", "r11", "cc", "memory");

    native_result_t result = { (int64_t)rax, rdx, rsi, r8, r9, r10 };
    return result;
}

/* binary64 bit pattern of an integer below 2^53 (integer arithmetic only:
 * this program is compiled with -mno-sse and never lets the compiler touch
 * the XMM registers, so all SSE work below lives in asm blocks). */
static uint64_t f64_bits_of(uint64_t value)
{
    if (value == 0) {
        return 0;
    }

    uint32_t exponent = 63U - (uint32_t)__builtin_clzll(value);
    uint64_t mantissa = (value << (52U - exponent)) & ((1ULL << 52) - 1ULL);

    return ((uint64_t)(1023U + exponent) << 52) | mantissa;
}

static void sse_image_fill(uint64_t image[32], uint64_t base)
{
    for (uint64_t lane = 0; lane < 32; lane++) {
        image[lane] = f64_bits_of(base + lane);
    }
}

static bool sse_image_matches(const uint64_t image[32], uint64_t base)
{
    for (uint64_t lane = 0; lane < 32; lane++) {
        if (image[lane] != f64_bits_of(base + lane)) {
            return false;
        }
    }
    return true;
}

static const uint64_t sse_one_pd[2] __attribute__((aligned(16))) = {
    0x3FF0000000000000ULL, 0x3FF0000000000000ULL
};

/* XMM0-15 <- image; `iterations` rounds of addpd 1.0 on every lane; image <-
 * XMM0-15. Long enough to be preempted many times on a 1 kHz timer. */
static void sse_lane_stress(uint64_t image[32], uint64_t iterations)
{
    __asm__ volatile(
        "movapd 0(%[img]), %%xmm0\n\t"
        "movapd 16(%[img]), %%xmm1\n\t"
        "movapd 32(%[img]), %%xmm2\n\t"
        "movapd 48(%[img]), %%xmm3\n\t"
        "movapd 64(%[img]), %%xmm4\n\t"
        "movapd 80(%[img]), %%xmm5\n\t"
        "movapd 96(%[img]), %%xmm6\n\t"
        "movapd 112(%[img]), %%xmm7\n\t"
        "movapd 128(%[img]), %%xmm8\n\t"
        "movapd 144(%[img]), %%xmm9\n\t"
        "movapd 160(%[img]), %%xmm10\n\t"
        "movapd 176(%[img]), %%xmm11\n\t"
        "movapd 192(%[img]), %%xmm12\n\t"
        "movapd 208(%[img]), %%xmm13\n\t"
        "movapd 224(%[img]), %%xmm14\n\t"
        "movapd 240(%[img]), %%xmm15\n\t"
        "1:\n\t"
        "addpd (%[one]), %%xmm0\n\t"
        "addpd (%[one]), %%xmm1\n\t"
        "addpd (%[one]), %%xmm2\n\t"
        "addpd (%[one]), %%xmm3\n\t"
        "addpd (%[one]), %%xmm4\n\t"
        "addpd (%[one]), %%xmm5\n\t"
        "addpd (%[one]), %%xmm6\n\t"
        "addpd (%[one]), %%xmm7\n\t"
        "addpd (%[one]), %%xmm8\n\t"
        "addpd (%[one]), %%xmm9\n\t"
        "addpd (%[one]), %%xmm10\n\t"
        "addpd (%[one]), %%xmm11\n\t"
        "addpd (%[one]), %%xmm12\n\t"
        "addpd (%[one]), %%xmm13\n\t"
        "addpd (%[one]), %%xmm14\n\t"
        "addpd (%[one]), %%xmm15\n\t"
        "decq %[n]\n\t"
        "jnz 1b\n\t"
        "movapd %%xmm0, 0(%[img])\n\t"
        "movapd %%xmm1, 16(%[img])\n\t"
        "movapd %%xmm2, 32(%[img])\n\t"
        "movapd %%xmm3, 48(%[img])\n\t"
        "movapd %%xmm4, 64(%[img])\n\t"
        "movapd %%xmm5, 80(%[img])\n\t"
        "movapd %%xmm6, 96(%[img])\n\t"
        "movapd %%xmm7, 112(%[img])\n\t"
        "movapd %%xmm8, 128(%[img])\n\t"
        "movapd %%xmm9, 144(%[img])\n\t"
        "movapd %%xmm10, 160(%[img])\n\t"
        "movapd %%xmm11, 176(%[img])\n\t"
        "movapd %%xmm12, 192(%[img])\n\t"
        "movapd %%xmm13, 208(%[img])\n\t"
        "movapd %%xmm14, 224(%[img])\n\t"
        "movapd %%xmm15, 240(%[img])\n\t"
        : [n] "+r"(iterations)
        : [img] "r"(image), [one] "r"(sse_one_pd)
        : "cc", "memory");
}

/* XMM0-15 <- image, then fork() with the registers live, then out <- XMM0-15
 * in both the parent and the child. Returns fork()'s rax. */
static int64_t sse_fork_with_live_xmm(const uint64_t image[32],
                                      uint64_t out[32])
{
    uint64_t rax = BSD_SYS_FORK;

    __asm__ volatile(
        "movapd 0(%[img]), %%xmm0\n\t"
        "movapd 16(%[img]), %%xmm1\n\t"
        "movapd 32(%[img]), %%xmm2\n\t"
        "movapd 48(%[img]), %%xmm3\n\t"
        "movapd 64(%[img]), %%xmm4\n\t"
        "movapd 80(%[img]), %%xmm5\n\t"
        "movapd 96(%[img]), %%xmm6\n\t"
        "movapd 112(%[img]), %%xmm7\n\t"
        "movapd 128(%[img]), %%xmm8\n\t"
        "movapd 144(%[img]), %%xmm9\n\t"
        "movapd 160(%[img]), %%xmm10\n\t"
        "movapd 176(%[img]), %%xmm11\n\t"
        "movapd 192(%[img]), %%xmm12\n\t"
        "movapd 208(%[img]), %%xmm13\n\t"
        "movapd 224(%[img]), %%xmm14\n\t"
        "movapd 240(%[img]), %%xmm15\n\t"
        "syscall\n\t"
        "movapd %%xmm0, 0(%[out])\n\t"
        "movapd %%xmm1, 16(%[out])\n\t"
        "movapd %%xmm2, 32(%[out])\n\t"
        "movapd %%xmm3, 48(%[out])\n\t"
        "movapd %%xmm4, 64(%[out])\n\t"
        "movapd %%xmm5, 80(%[out])\n\t"
        "movapd %%xmm6, 96(%[out])\n\t"
        "movapd %%xmm7, 112(%[out])\n\t"
        "movapd %%xmm8, 128(%[out])\n\t"
        "movapd %%xmm9, 144(%[out])\n\t"
        "movapd %%xmm10, 160(%[out])\n\t"
        "movapd %%xmm11, 176(%[out])\n\t"
        "movapd %%xmm12, 192(%[out])\n\t"
        "movapd %%xmm13, 208(%[out])\n\t"
        "movapd %%xmm14, 224(%[out])\n\t"
        "movapd %%xmm15, 240(%[out])\n\t"
        : "+a"(rax)
        : [img] "r"(image), [out] "r"(out)
        : "rcx", "r11", "rdi", "rsi", "rdx", "r8", "r9", "r10", "cc",
          "memory");
    return (int64_t)rax;
}

static void sse_store_all(uint64_t out[32], uint32_t *mxcsr)
{
    __asm__ volatile(
        "movapd %%xmm0, 0(%[out])\n\t"
        "movapd %%xmm1, 16(%[out])\n\t"
        "movapd %%xmm2, 32(%[out])\n\t"
        "movapd %%xmm3, 48(%[out])\n\t"
        "movapd %%xmm4, 64(%[out])\n\t"
        "movapd %%xmm5, 80(%[out])\n\t"
        "movapd %%xmm6, 96(%[out])\n\t"
        "movapd %%xmm7, 112(%[out])\n\t"
        "movapd %%xmm8, 128(%[out])\n\t"
        "movapd %%xmm9, 144(%[out])\n\t"
        "movapd %%xmm10, 160(%[out])\n\t"
        "movapd %%xmm11, 176(%[out])\n\t"
        "movapd %%xmm12, 192(%[out])\n\t"
        "movapd %%xmm13, 208(%[out])\n\t"
        "movapd %%xmm14, 224(%[out])\n\t"
        "movapd %%xmm15, 240(%[out])\n\t"
        "stmxcsr (%[mx])\n\t"
        :
        : [out] "r"(out), [mx] "r"(mxcsr)
        : "memory");
}

static void sse_load_all(const uint64_t image[32])
{
    __asm__ volatile(
        "movapd 0(%[img]), %%xmm0\n\t"
        "movapd 16(%[img]), %%xmm1\n\t"
        "movapd 32(%[img]), %%xmm2\n\t"
        "movapd 48(%[img]), %%xmm3\n\t"
        "movapd 64(%[img]), %%xmm4\n\t"
        "movapd 80(%[img]), %%xmm5\n\t"
        "movapd 96(%[img]), %%xmm6\n\t"
        "movapd 112(%[img]), %%xmm7\n\t"
        "movapd 128(%[img]), %%xmm8\n\t"
        "movapd 144(%[img]), %%xmm9\n\t"
        "movapd 160(%[img]), %%xmm10\n\t"
        "movapd 176(%[img]), %%xmm11\n\t"
        "movapd 192(%[img]), %%xmm12\n\t"
        "movapd 208(%[img]), %%xmm13\n\t"
        "movapd 224(%[img]), %%xmm14\n\t"
        "movapd 240(%[img]), %%xmm15\n\t"
        :
        : [img] "r"(image)
        : "memory");
}

static char stage_e_buf[8192];

static uint64_t sse_image_a[32] __attribute__((aligned(16)));
static uint64_t sse_image_b[32] __attribute__((aligned(16)));

/* Runs SSE_ROUNDS stress rounds from `base`; true when every lane of every
 * round came back exactly start + iterations. */
static bool sse_rounds_ok(uint64_t base)
{
    for (uint32_t round = 0; round < SSE_ROUNDS; round++) {
        sse_image_fill(sse_image_a, base);
        sse_lane_stress(sse_image_a, SSE_ITERATIONS);
        base += SSE_ITERATIONS;
        if (!sse_image_matches(sse_image_a, base)) {
            return false;
        }
    }
    return true;
}

/* E1: every forked process inherits the exact XMM register file that was live
 * in its parent at fork(), then all of them run long packed-double loops
 * concurrently on the same CPU so the 1 kHz timer preempts them in the middle
 * of their asm blocks. Any save/restore bug shows up as a wrong lane. The
 * kernel's own fpu stress self-test runs the same loop on all four CPUs. */
static void test_sse_fork_preempt(void)
{
    int64_t pids[SSE_CHILDREN];

    for (int child = 0; child < SSE_CHILDREN; child++) {
        uint64_t seed = 3000000ULL * (uint64_t)(child + 1);

        sse_image_fill(sse_image_b, seed);
        int64_t pid = sse_fork_with_live_xmm(sse_image_b, sse_image_a);

        if (pid < 0) {
            fail(500);
        }
        if (!sse_image_matches(sse_image_a, seed)) {
            if (pid == 0) {
                user_exit(51);
            }
            fail(501);
        }
        if (pid == 0) {
            user_exit(sse_rounds_ok(seed + 100000000ULL) ? 0U : 52U);
        }
        pids[child] = pid;
    }

    if (!sse_rounds_ok(900000000ULL)) {
        fail(502);
    }

    for (int child = 0; child < SSE_CHILDREN; child++) {
        int32_t status = -1;

        if (bsd_wait4((int32_t)pids[child], &status, 0) != pids[child] ||
            status != 0) {
            write_text(2, "init: SSE child status ");
            write_signed(2, status);
            write_text(2, "\n");
            fail(503);
        }
    }

    write_text(1,
               "init: SSE XMM0-15 preserved across fork() and preemptive "
               "switches (4 processes x 4 rounds x 250000 addpd)\n");
}

static int64_t read_whole(const char *path, char *buf, size_t cap)
{
    int64_t fd = bsd_open(path, BSD_O_RDONLY, 0);

    if (fd < 0) {
        return fd;
    }

    size_t total = 0;

    while (total + 1 < cap) {
        int64_t bytes = bsd_read((int32_t)fd, buf + total, cap - 1 - total);

        if (bytes < 0) {
            (void)bsd_close((int32_t)fd);
            return bytes;
        }
        if (bytes == 0) {
            break;
        }
        total += (size_t)bytes;
    }

    buf[total] = '\0';
    (void)bsd_close((int32_t)fd);
    return (int64_t)total;
}

static const char *find_text(const char *haystack, const char *needle)
{
    size_t length = text_len(needle);

    for (const char *cursor = haystack; *cursor != '\0'; cursor++) {
        if (str_eq_len(cursor, needle, length)) {
            return cursor;
        }
    }
    return NULL;
}

static size_t count_text(const char *haystack, const char *needle)
{
    size_t count = 0;
    size_t length = text_len(needle);

    for (const char *cursor = haystack; *cursor != '\0'; cursor++) {
        if (str_eq_len(cursor, needle, length)) {
            count++;
        }
    }
    return count;
}

/* Prints the line of `text` that starts with `key`. */
static void echo_line(const char *prefix, const char *text, const char *key)
{
    const char *line = find_text(text, key);

    if (line == NULL) {
        return;
    }

    size_t length = 0;
    while (line[length] != '\0' && line[length] != '\n') {
        length++;
    }

    write_text(1, prefix);
    write_all(1, line, length);
    write_all(1, "\n", 1);
}

/* E4: the Limine framebuffer and its read-only description arrive as
 * capabilities 1 and 2. Map both, check the geometry, draw into the
 * framebuffer, and confirm the rights the kernel attached. */
static void test_framebuffer_caps(void)
{
    native_result_t r = native_call(SYS_VSPACE_MAP, DISPLAY_BOOT_CAP_INFO,
                                    STAGE_E_INFO_VA, STAGE_E_PROT_WRITE, 0, 0);
    if (r.status != SYS_ERR_CAP_RIGHTS_USER) {
        fail(510);
    }

    r = native_call(SYS_VSPACE_MAP, DISPLAY_BOOT_CAP_INFO, STAGE_E_INFO_VA, 0,
                    0, 0);
    if (r.status != 0) {
        fail(511);
    }

    const volatile display_info_t *info =
        (const volatile display_info_t *)(uintptr_t)STAGE_E_INFO_VA;

    if (info->magic != DISPLAY_INFO_MAGIC ||
        info->version != DISPLAY_INFO_VERSION || info->width == 0 ||
        info->height == 0 || info->bpp == 0 ||
        info->pitch < info->width * (info->bpp / 8U) ||
        info->mapped_bytes < info->size_bytes) {
        fail(512);
    }

    uint64_t width = info->width;
    uint64_t height = info->height;
    uint64_t pitch = info->pitch;
    uint64_t bpp = info->bpp;
    uint64_t mapped = info->mapped_bytes;

    r = native_call(SYS_VSPACE_MAP, DISPLAY_BOOT_CAP_FRAMEBUFFER,
                    STAGE_E_FB_VA, STAGE_E_PROT_WRITE, 0, 0);
    if (r.status != 0) {
        fail(513);
    }

    /* Zero copy: Ring 3 writes land directly in the device framebuffer. */
    if (bpp == 32) {
        volatile uint32_t *row = (volatile uint32_t *)(uintptr_t)STAGE_E_FB_VA;
        volatile uint32_t *last =
            (volatile uint32_t *)(uintptr_t)(STAGE_E_FB_VA +
                                             (height - 1U) * pitch);

        for (uint32_t x = 0; x < 64U; x++) {
            row[x] = 0x00FF7F00U | x;
            last[x] = 0x00007FFFU;
        }
        for (uint32_t x = 0; x < 64U; x++) {
            if (row[x] != (0x00FF7F00U | x) || last[x] != 0x00007FFFU) {
                fail(514);
            }
        }
    }

    if (read_whole("/proc/self/maps", stage_e_buf, sizeof(stage_e_buf)) <= 0 ||
        find_text(stage_e_buf, "rw-s 00000000 00:00 0 [device]") == NULL ||
        find_text(stage_e_buf, "r--p") == NULL) {
        fail(515);
    }

    if (native_call(SYS_VSPACE_UNMAP, STAGE_E_FB_VA, mapped, 0, 0, 0).status !=
            0 ||
        native_call(SYS_VSPACE_UNMAP, STAGE_E_INFO_VA, PAGE_BYTES, 0, 0, 0)
                .status != 0) {
        fail(516);
    }

    write_text(1, "init: framebuffer CAP_OBJ_FRAME mapped in Ring 3: ");
    write_unsigned(1, width);
    write_text(1, "x");
    write_unsigned(1, height);
    write_text(1, "x");
    write_unsigned(1, bpp);
    write_text(1, " pitch ");
    write_unsigned(1, pitch);
    write_text(1, ", display-info frame read-only, pixels written\n");
}

/* E2 + E4: QoS syscall, IPC priority inheritance and cross-task capability
 * grant. The child (a separate task) receives an attenuated endpoint and the
 * display-info frame through SYS_CAP_GRANT, drops itself to BACKGROUND and
 * serves one IPC from this INTERACTIVE parent: while it owes the reply its
 * effective QoS must be INTERACTIVE, and BACKGROUND again afterwards. */
static void test_qos_ipc_grant(void)
{
    native_result_t r = native_call(SYS_THREAD_SET_QOS, 0, SYS_QOS_QUERY, 0, 0,
                                    0);
    if (r.status != 0 || r.rdx != SYS_QOS_DEFAULT ||
        r.r8 != SYS_QOS_DEFAULT) {
        fail(520);
    }
    if (native_call(SYS_THREAD_SET_QOS, 0, 3, 0, 0, 0).status !=
        SYS_ERR_INVAL_USER) {
        fail(521);
    }
    r = native_call(SYS_THREAD_SET_QOS, 0, SYS_QOS_INTERACTIVE, 0, 0, 0);
    if (r.status != 0 || r.rdx != SYS_QOS_INTERACTIVE ||
        r.r8 != SYS_QOS_INTERACTIVE) {
        fail(522);
    }

    r = native_call(SYS_ENDPOINT_CREATE, 0, 0, 0, 0, 0);
    if (r.status != 0 || r.rdx == 0) {
        fail(523);
    }
    uint64_t endpoint = r.rdx;

    int32_t to_parent[2];
    int32_t to_child[2];

    if (bsd_pipe(to_parent) != 0 || bsd_pipe(to_child) != 0) {
        fail(524);
    }

    int64_t pid = bsd_fork();

    if (pid < 0) {
        fail(525);
    }

    if (pid == 0) {
        /* The child inherited its creator's class. */
        r = native_call(SYS_THREAD_SET_QOS, 0, SYS_QOS_QUERY, 0, 0, 0);
        if (r.rdx != SYS_QOS_INTERACTIVE) {
            user_exit(60);
        }
        if (native_call(SYS_THREAD_SET_QOS, 0, SYS_QOS_BACKGROUND, 0, 0, 0)
                .status != 0) {
            user_exit(61);
        }

        uint64_t task_id = native_call(SYS_TASK_ID, 0, 0, 0, 0, 0).rdx;
        uint64_t handles[2] = { 0, 0 };

        if (bsd_write(to_parent[1], &task_id, sizeof(task_id)) != 8 ||
            bsd_read(to_child[0], handles, sizeof(handles)) != 16) {
            user_exit(62);
        }

        /* The granted display-info frame is READ-only and cannot be
         * re-granted (no CAP_RIGHT_GRANT). */
        if (native_call(SYS_VSPACE_MAP, handles[1], STAGE_E_INFO_VA,
                        STAGE_E_PROT_WRITE, 0, 0).status !=
                SYS_ERR_CAP_RIGHTS_USER ||
            native_call(SYS_CAP_GRANT, handles[1], SYS_CAP_GRANT_SELF,
                        SYS_CAP_RIGHT_READ, 0, 0).status !=
                SYS_ERR_CAP_RIGHTS_USER ||
            native_call(SYS_VSPACE_MAP, handles[1], STAGE_E_INFO_VA, 0, 0, 0)
                    .status != 0) {
            user_exit(63);
        }
        const volatile display_info_t *info =
            (const volatile display_info_t *)(uintptr_t)STAGE_E_INFO_VA;
        if (info->magic != DISPLAY_INFO_MAGIC || info->width == 0) {
            user_exit(64);
        }

        r = native_call(SYS_IPC_RECV, handles[0], 0, 0, 0, 0);
        if (r.status != 0 || r.rsi != 0x5155455354454E44ULL) {
            user_exit(65);
        }

        native_result_t during =
            native_call(SYS_THREAD_SET_QOS, 0, SYS_QOS_QUERY, 0, 0, 0);

        if (native_call(SYS_IPC_REPLY, during.rdx, during.r8, info->width,
                        ~r.rsi, 0).status != 0) {
            user_exit(66);
        }

        native_result_t after =
            native_call(SYS_THREAD_SET_QOS, 0, SYS_QOS_QUERY, 0, 0, 0);
        if (after.rdx != SYS_QOS_BACKGROUND ||
            after.r8 != SYS_QOS_BACKGROUND) {
            user_exit(67);
        }
        user_exit(0);
    }

    uint64_t child_task = 0;

    if (bsd_read(to_parent[0], &child_task, sizeof(child_task)) != 8 ||
        child_task == 0) {
        fail(526);
    }

    /* A grant to a task id that does not exist is rejected; real grants are
     * attenuated to the rights the child needs. */
    if (native_call(SYS_CAP_GRANT, endpoint, 0x7FFFFFFFULL, SYS_CAP_RIGHT_ALL,
                    0, 0).status != SYS_ERR_INVAL_USER) {
        fail(527);
    }
    native_result_t ep_grant = native_call(SYS_CAP_GRANT, endpoint, child_task,
                                           SYS_CAP_RIGHT_READ, 0, 0);
    native_result_t info_grant =
        native_call(SYS_CAP_GRANT, DISPLAY_BOOT_CAP_INFO, child_task,
                    SYS_CAP_RIGHT_READ, 0, 0);
    if (ep_grant.status != 0 || info_grant.status != 0) {
        fail(528);
    }

    uint64_t handles[2] = { ep_grant.rdx, info_grant.rdx };
    if (bsd_write(to_child[1], handles, sizeof(handles)) != 16) {
        fail(529);
    }

    r = native_call(SYS_IPC_CALL, endpoint, 0x5155455354454E44ULL, 0, 0, 0);
    if (r.status != 0) {
        fail(530);
    }
    /* reply: m0 = server base QoS, m1 = server effective QoS during the call,
     * m2 = display width seen by the child, m3 = ~request. */
    if (r.rsi != SYS_QOS_BACKGROUND || r.r8 != SYS_QOS_INTERACTIVE ||
        r.r9 == 0 || r.r10 != ~0x5155455354454E44ULL) {
        write_text(2, "init: PI reply base/effective = ");
        write_unsigned(2, r.rsi);
        write_text(2, "/");
        write_unsigned(2, r.r8);
        write_text(2, "\n");
        fail(531);
    }

    int32_t status = -1;
    if (bsd_wait4((int32_t)pid, &status, 0) != pid || status != 0) {
        write_text(2, "init: QoS child status ");
        write_signed(2, status);
        write_text(2, "\n");
        fail(532);
    }

    (void)bsd_close(to_parent[0]);
    (void)bsd_close(to_parent[1]);
    (void)bsd_close(to_child[0]);
    (void)bsd_close(to_child[1]);

    if (native_call(SYS_CAP_DELETE, endpoint, 0, 0, 0, 0).status != 0 ||
        native_call(SYS_THREAD_SET_QOS, 0, SYS_QOS_DEFAULT, 0, 0, 0).status !=
            0) {
        fail(533);
    }

    write_text(1,
               "init: SYS_THREAD_SET_QOS + IPC priority inheritance verified "
               "(background server ran interactive until SYS_IPC_REPLY)\n");
    write_text(1,
               "init: SYS_CAP_GRANT moved an attenuated endpoint and the "
               "display-info frame into another task\n");
}

/* E3: /proc and /sys from Ring 3. */
static void test_proc_sys(void)
{
    if (read_whole("/proc/meminfo", stage_e_buf, sizeof(stage_e_buf)) <= 0 ||
        find_text(stage_e_buf, "MemTotal:") == NULL ||
        find_text(stage_e_buf, "PmmFreeFrames:") == NULL) {
        fail(540);
    }
    echo_line("init: /proc/meminfo ", stage_e_buf, "MemFree:");

    if (read_whole("/proc/cpuinfo", stage_e_buf, sizeof(stage_e_buf)) <= 0 ||
        find_text(stage_e_buf, "vendor_id") == NULL) {
        fail(541);
    }

    size_t processors = count_text(stage_e_buf, "processor\t: ");
    const char *cores = find_text(stage_e_buf, "cpu cores\t: ");
    int64_t reported = 0;

    if (cores != NULL) {
        char digits[8];
        size_t n = 0;

        cores += text_len("cpu cores\t: ");
        while (n + 1 < sizeof(digits) && cores[n] >= '0' && cores[n] <= '9') {
            digits[n] = cores[n];
            n++;
        }
        digits[n] = '\0';
        (void)parse_int(digits, &reported);
    }
    if (processors == 0 || (int64_t)processors != reported) {
        fail(542);
    }
    write_text(1, "init: /proc/cpuinfo lists ");
    write_unsigned(1, processors);
    write_text(1, " online CPU(s)\n");

    if (read_whole("/proc/self/maps", stage_e_buf, sizeof(stage_e_buf)) <= 0 ||
        find_text(stage_e_buf, "r-xp") == NULL ||
        find_text(stage_e_buf, "rw-p 00000000 00:00 0 [stack]") == NULL) {
        fail(543);
    }

    if (read_whole("/proc/self/status", stage_e_buf, sizeof(stage_e_buf)) <=
            0 ||
        find_text(stage_e_buf, "Pid:\t1\n") == NULL ||
        find_text(stage_e_buf, "Name:\tinit") == NULL ||
        read_whole("/proc/1/status", stage_e_buf, sizeof(stage_e_buf)) <= 0 ||
        find_text(stage_e_buf, "PPid:\t0\n") == NULL ||
        bsd_open("/proc/4242/status", BSD_O_RDONLY, 0) != -BSD_ENOENT) {
        fail(544);
    }

    int64_t fd = bsd_open("/proc", BSD_O_RDONLY, 0);
    bool saw_self = false;
    bool saw_init = false;

    if (fd < 0) {
        fail(545);
    }
    for (;;) {
        bsd_dirent_t entries[4];
        int64_t bytes = bsd_getdents((int32_t)fd, entries, sizeof(entries));

        if (bytes <= 0) {
            break;
        }
        for (size_t i = 0; i < (size_t)bytes / sizeof(bsd_dirent_t); i++) {
            saw_self = saw_self || str_eq(entries[i].d_name, "self");
            saw_init = saw_init || str_eq(entries[i].d_name, "1");
        }
    }
    (void)bsd_close((int32_t)fd);
    if (!saw_self || !saw_init) {
        fail(546);
    }

    if (read_whole("/sys/bus/pci/devices", stage_e_buf, sizeof(stage_e_buf)) <=
            0) {
        fail(547);
    }
    echo_line("init: /sys/bus/pci/devices ", stage_e_buf, "00:03.0");

    write_text(1,
               "init: /proc/meminfo, /proc/cpuinfo, /proc/self/{maps,status}, "
               "/proc/<pid> and /sys/bus/pci/devices verified\n");
}

static const uint64_t exec_garbage[32] __attribute__((aligned(16))) = {
    0xDEADBEEFDEADBEEFULL, 0xDEADBEEFDEADBEEFULL, 0xDEADBEEFDEADBEEFULL,
    0xDEADBEEFDEADBEEFULL, 0xDEADBEEFDEADBEEFULL, 0xDEADBEEFDEADBEEFULL,
    0xDEADBEEFDEADBEEFULL, 0xDEADBEEFDEADBEEFULL, 0xDEADBEEFDEADBEEFULL,
    0xDEADBEEFDEADBEEFULL, 0xDEADBEEFDEADBEEFULL, 0xDEADBEEFDEADBEEFULL,
    0xDEADBEEFDEADBEEFULL, 0xDEADBEEFDEADBEEFULL, 0xDEADBEEFDEADBEEFULL,
    0xDEADBEEFDEADBEEFULL, 0xDEADBEEFDEADBEEFULL, 0xDEADBEEFDEADBEEFULL,
    0xDEADBEEFDEADBEEFULL, 0xDEADBEEFDEADBEEFULL, 0xDEADBEEFDEADBEEFULL,
    0xDEADBEEFDEADBEEFULL, 0xDEADBEEFDEADBEEFULL, 0xDEADBEEFDEADBEEFULL,
    0xDEADBEEFDEADBEEFULL, 0xDEADBEEFDEADBEEFULL, 0xDEADBEEFDEADBEEFULL,
    0xDEADBEEFDEADBEEFULL, 0xDEADBEEFDEADBEEFULL, 0xDEADBEEFDEADBEEFULL,
    0xDEADBEEFDEADBEEFULL, 0xDEADBEEFDEADBEEFULL
};

/* E3: execve() carries envp to the new image (checked by the re-executed init
 * in envtest_main) and resets the FPU/SSE state the old image left behind. */
static void test_exec_envp(void)
{
    int64_t pid = bsd_fork();

    if (pid < 0) {
        fail(550);
    }
    if (pid == 0) {
        char *argv[3] = { "init", "--envtest", NULL };
        char *envp[4] = { "LUCYK_STAGE=E", "HOME=/", "PATH=/bin:/sbin", NULL };

        sse_load_all(exec_garbage);
        (void)bsd_execve("/bin/init", argv, envp);
        user_exit(70);
    }

    int32_t status = -1;
    if (bsd_wait4((int32_t)pid, &status, 0) != pid || status != 0) {
        write_text(2, "init: envtest status ");
        write_signed(2, status);
        write_text(2, "\n");
        fail(551);
    }

    write_text(1,
               "init: execve envp propagated (argc/argv/NULL/envp/NULL, "
               "16-byte aligned) and FPU/SSE state reset\n");
}

static uint64_t envtest_xmm[32] __attribute__((aligned(16)));
static uint32_t envtest_mxcsr;

/* Entry of "/bin/init --envtest": validates the System V initial stack the
 * kernel built and the clean FPU image, then exits with a diagnostic code. */
__attribute__((noreturn)) static void envtest_main(int argc, char **argv,
                                                   char **envp)
{
    for (size_t i = 0; i < 32; i++) {
        if (envtest_xmm[i] != 0) {
            user_exit(80);
        }
    }
    if (envtest_mxcsr != 0x1F80U) {
        user_exit(81);
    }

    uint64_t *base = (uint64_t *)(void *)argv - 1;

    if (((uintptr_t)base & 15U) != 0 || base[0] != (uint64_t)argc ||
        argv[argc] != NULL || envp != argv + argc + 1) {
        user_exit(82);
    }

    static const char *const expected[3] = {
        "LUCYK_STAGE=E", "HOME=/", "PATH=/bin:/sbin"
    };

    for (int i = 0; i < 3; i++) {
        if (envp[i] == NULL || !str_eq(envp[i], expected[i])) {
            user_exit(83);
        }
    }
    if (envp[3] != NULL) {
        user_exit(84);
    }

    user_exit(0);
}

#define SYS_VSPACE_PROT_WRITE 0x1ULL

/* Stage F: contiguous DMA + physical address lookup. */
static void run_stage_f_tests(void)
{
    /* --- SYS_DMA_ALLOC + SYS_FRAME_PHYS_ADDR + SYS_VSPACE_MAP --- */
    native_result_t dma = native_call(SYS_DMA_ALLOC, 2ULL, SYS_CAP_RIGHT_ALL,
                                      0, 0, 0);
    if (dma.status != 0 || dma.rdx == 0) {
        fail(600);
    }
    uint64_t cap = dma.rdx;

    /* Contiguous pages must be page-aligned physical + reject EXEC map. */
    native_result_t phys = native_call(SYS_FRAME_PHYS_ADDR, cap, 0, 0, 0, 0);
    if (phys.status != 0 || (phys.rdx & 0xFFFULL) != 0) {
        fail(601);
    }

    uint64_t va = 0x008000000000ULL; /* scratch user VA */
    native_result_t mapw = native_call(SYS_VSPACE_MAP, cap, va,
                                       SYS_VSPACE_PROT_WRITE, 0, 0);
    if (mapw.status != 0) {
        fail(602);
    }
    /* PTE_EXEC never allowed on DMA. */
    native_result_t mapx = native_call(SYS_VSPACE_MAP, cap, va + 0x2000ULL,
                                       0x2ULL, 0, 0);
    if (mapx.status == 0) {
        fail(603);
    }

    /* Non-DMA frames rejected by SYS_FRAME_PHYS_ADDR. */
    native_result_t bad = native_call(SYS_FRAME_PHYS_ADDR, DISPLAY_BOOT_CAP_INFO,
                                      0, 0, 0, 0);
    if (bad.status == 0) {
        fail(604);
    }

    (void)native_call(SYS_VSPACE_UNMAP, va, 0x2000ULL, 0, 0, 0);
    (void)native_call(SYS_CAP_DELETE, cap, 0, 0, 0, 0);

    write_text(1, "init: Stage F SYS_DMA_ALLOC contiguous + "
                   "SYS_FRAME_PHYS_ADDR (DMA-only) verified\n");
}

static void run_stage_e_tests(void)
{
    write_text(1, "init: Stage E tests\n");
    test_sse_fork_preempt();
    test_framebuffer_caps();
    test_qos_ipc_grant();
    test_proc_sys();
    test_exec_envp();
    write_text(1, "init: Stage E automated tests passed\n");
}

static void print_motd(void)
{
    int64_t fd = bsd_open("/etc/motd", BSD_O_RDONLY, 0);

    if (fd < 0) {
        write_text(1, "init: cannot open /etc/motd\n");
        return;
    }

    for (;;) {
        int64_t bytes = bsd_read((int32_t)fd, iobuf, sizeof(iobuf));

        if (bytes <= 0) {
            break;
        }
        write_all(1, iobuf, (size_t)bytes);
    }

    (void)bsd_close((int32_t)fd);
    write_all(1, "\n", 1);
}

static void spawn_shell(void)
{
    int64_t pid = bsd_fork();

    if (pid < 0) {
        write_text(1, "init: fork for /bin/sh failed\n");
        return;
    }

    if (pid == 0) {
        char *argv[2];

        argv[0] = "sh";
        argv[1] = NULL;

        (void)bsd_execve("/bin/sh", argv, NULL);
        write_text(1, "init: cannot exec /bin/sh\n");
        user_exit(127);
    }

    /* PID 1 reaps the shell and any orphan reparented to it. */
    for (;;) {
        int32_t status = 0;
        int64_t reaped = bsd_wait4(-1, &status, 0);

        if (reaped < 0) {
            raw_sleep_ms(50);
        }
    }
}

void user_main(int argc, char **argv, char **envp)
{
    /* Captured before anything else: the --envtest image must observe the
     * clean FPU/SSE state execve() installs. */
    sse_store_all(envtest_xmm, &envtest_mxcsr);

    /* PID 1 is entered with argc == 0; only a re-executed init has args. */
    if (argc == 2 && argv != NULL && argv[1] != NULL &&
        str_eq(argv[1], "--envtest")) {
        envtest_main(argc, argv, envp);
    }

    write_text(1, "init: PID 1 online (Kernel Stage C)\n");

    if (bsd_getpid() != 1) {
        user_exit(1);
    }
    if (bsd_getppid() != 0) {
        user_exit(2);
    }

    test_pipe_fork();
    test_exec_echo();
    test_exec_failure();
    test_wait_nohang();
    test_exec_bad_pointer();
    test_exec_oversized();
    test_wait_bad_status();
    test_orphan_adoption();
    test_cow_fork();
    test_fork_storm();

    write_text(1, "init: Phase 3 automated tests passed\n");

    /* Stage C tests. */
    test_sigaction_basic();
    test_pgid_sigint();
    test_arch_prctl_tls();
    test_mprotect();

    write_text(1, "init: Stage C automated tests passed\n");

    if (root_writable()) {
        write_text(1, "init: Stage D2 ext2 tests\n");
        test_ext2_motd();
        test_ext2_indirect();
        test_ext2_dirs();
        test_ext2_trunc_append();
        test_ext2_rmdir_nonempty();
        test_ext2_unlink_open();
        test_ext2_concurrent();
        write_text(1, "init: Stage D2 ext2 persistence tests passed\n");
    } else {
        write_text(1, "init: Stage D2 ext2 tests skipped: / is read-only "
                        "(initrd fallback)\n");
    }

    run_stage_e_tests();
    run_stage_f_tests();

    print_motd();
    spawn_shell();

    for (;;) {
        raw_sleep_ms(1000);
    }
}
