#include <stdint.h>
#include "user_lib.h"

#define USER_ENDPOINT_CAP 1ULL
#define USER_FRAME_CAP    2ULL
#define USER_FRAME_VADDR  0x0000000000500000ULL
#define USER_MAP_PROT_WRITE 1ULL
#define USER_ERR_INVAL    ((uint64_t)-1)
#define USER_BADGE 0x5555555555555555ULL
#define PERSONALITY_TEST 0x1234ULL
#define PERSONALITY_SPAWN_FAULT 0x1001ULL

void user_main(void);
__attribute__((noreturn)) void user_exit(uint64_t code);

static const char exec_hello[] = "exec: Ring 3 syscall path online";
static const char exec_done[] = "exec: init_test complete";

static int is_complement(uint64_t value, uint64_t original)
{
    return value == ~original;
}

static void expect_invalid_log(uint64_t pointer)
{
    uint64_t status = user_syscall(SYS_DEBUG_LOG, pointer, 8, 0, 0, 0);

    if (status != USER_ERR_INVAL) {
        user_exit(7);
    }
}

void user_main(void)
{
    /* Ring 3 must not be able to make the kernel read NULL, the kernel
     * image, a non-canonical address, or an unmapped user page. */
    expect_invalid_log(0);
    expect_invalid_log(0xFFFFFFFF80000000ULL);
    expect_invalid_log(0x0000800000000000ULL);
    expect_invalid_log(0x0000000000001000ULL);

    uint64_t status = user_syscall(SYS_VSPACE_MAP, USER_FRAME_CAP,
                                   USER_FRAME_VADDR, USER_MAP_PROT_WRITE,
                                   0, 0);
    if (status != 0) {
        user_exit(8);
    }

    volatile uint64_t *mapped =
        (volatile uint64_t *)(uintptr_t)USER_FRAME_VADDR;
    *mapped = 0xFEEDFACECAFEBEEFULL;
    if (*mapped != 0xFEEDFACECAFEBEEFULL) {
        user_exit(9);
    }

    status = user_syscall(SYS_DEBUG_LOG,
                          (uint64_t)(uintptr_t)exec_hello,
                          sizeof(exec_hello) - 1, 0, 0, 0);
    if (status != 0) {
        user_exit(1);
    }

    uint64_t personality = user_syscall(PERSONALITY_TEST, 0x1111, 0x2222,
                                        0, 0, 0);
    if (personality != 0x3333) {
        user_exit(2);
    }

    uint64_t msg[4] = {
        0x1122334455667788ULL,
        0x99AABBCCDDEEFF00ULL,
        0x0123456789ABCDEFULL,
        0xFEDCBA9876543210ULL
    };
    uint64_t reply[4] = { 0, 0, 0, 0 };
    uint64_t badge = 0;

    status = user_ipc_call(USER_ENDPOINT_CAP, msg, reply, &badge);
    if (status != 0) {
        user_exit(3);
    }
    if (badge != USER_BADGE) {
        user_exit(4);
    }
    for (uint64_t i = 0; i < 4; i++) {
        if (!is_complement(reply[i], msg[i])) {
            user_exit(5);
        }
    }

    status = user_syscall(PERSONALITY_SPAWN_FAULT, 0, 0, 0, 0, 0);
    if (status != 0) {
        user_exit(6);
    }

    (void)user_syscall(SYS_DEBUG_LOG, (uint64_t)(uintptr_t)exec_done,
                       sizeof(exec_done) - 1, 0, 0, 0);
    user_exit(0);
}
