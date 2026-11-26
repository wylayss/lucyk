#ifndef LUCYK_USER_LIB_H
#define LUCYK_USER_LIB_H

#include <stdint.h>
#include <syscall_numbers.h>

static inline uint64_t user_syscall(uint64_t number, uint64_t arg1,
                                    uint64_t arg2, uint64_t arg3,
                                    uint64_t arg4, uint64_t arg5)
{
    register uint64_t rdi __asm__("rdi") = arg1;
    register uint64_t rsi __asm__("rsi") = arg2;
    register uint64_t rdx __asm__("rdx") = arg3;
    register uint64_t r10 __asm__("r10") = arg4;
    register uint64_t r8 __asm__("r8") = arg5;
    register uint64_t r9 __asm__("r9") = 0;
    uint64_t result;

    __asm__ volatile("syscall"
                     : "=a"(result)
                     : "a"(number), "r"(rdi), "r"(rsi), "r"(rdx),
                       "r"(r10), "r"(r8), "r"(r9)
                     : "rcx", "r11", "cc", "memory");

    return result;
}

static inline uint64_t user_ipc_call(uint64_t cap, const uint64_t msg[4],
                                     uint64_t reply[4], uint64_t *badge)
{
    register uint64_t rdi __asm__("rdi") = cap;
    register uint64_t rsi __asm__("rsi") = msg[0];
    register uint64_t rdx __asm__("rdx") = msg[1];
    register uint64_t r10 __asm__("r10") = msg[2];
    register uint64_t r8 __asm__("r8") = msg[3];
    register uint64_t r9 __asm__("r9") = 0;
    uint64_t result;

    __asm__ volatile("syscall"
                     : "=a"(result), "+r"(rsi), "+r"(rdx), "+r"(r10),
                       "+r"(r8), "+r"(r9)
                     : "a"((uint64_t)SYS_IPC_CALL), "r"(rdi)
                     : "rcx", "r11", "cc", "memory");

    reply[0] = rsi;
    reply[1] = r8;
    reply[2] = r9;
    reply[3] = r10;
    *badge = rdx;
    return result;
}

#endif /* LUCYK_USER_LIB_H */
