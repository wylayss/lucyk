#include "bsd_user.h"

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

void user_main(void)
{
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

    write_text(1, "init: Stage D2 ext2 tests\n");
    test_ext2_motd();
    test_ext2_indirect();
    test_ext2_dirs();
    test_ext2_trunc_append();
    test_ext2_rmdir_nonempty();
    test_ext2_unlink_open();
    test_ext2_concurrent();
    write_text(1, "init: Stage D2 ext2 persistence tests passed\n");

    print_motd();
    spawn_shell();

    for (;;) {
        raw_sleep_ms(1000);
    }
}
