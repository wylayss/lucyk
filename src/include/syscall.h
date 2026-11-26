#ifndef LUCYK_SYSCALL_H
#define LUCYK_SYSCALL_H

#include <syscall_numbers.h>
#include <types.h>

/* Per-CPU state is defined in cpu.h as cpu_local_t (the full per-CPU block,
 * with kernel_rsp at offset 0 and user_rsp_scratch at offset 8 as required by
 * syscall.asm and switch.asm). */
#include <cpu.h>

/* Frame built by syscall.asm before calling syscall_dispatch(). The trailing
 * fields are the callee-saved registers syscall.asm pushes ahead of the frame;
 * they let a personality layer clone a complete Ring 3 context. */
typedef struct syscall_frame {
    uint64_t rdi;
    uint64_t rsi;
    uint64_t rdx;
    uint64_t r10;
    uint64_t r8;
    uint64_t r9;
    uint64_t rax;
    uint64_t rcx;
    uint64_t r11;
    uint64_t rsp;
    uint64_t r15;
    uint64_t r14;
    uint64_t r13;
    uint64_t r12;
    uint64_t rbx;
    uint64_t rbp;
} syscall_frame_t;

_Static_assert(sizeof(syscall_frame_t) == 16 * sizeof(uint64_t),
               "syscall_frame_t must match the syscall.asm stack layout");

void syscall_init(void);

/* Programs the SYSCALL MSRs on an Application Processor. The MSRs are
 * per-CPU; GS binding is done separately through cpu_local_bind(). */
void syscall_init_ap(void);
void syscall_dispatch(syscall_frame_t *frame);

/* Drops to Ring 3 through an IRETQ frame. Never returns. */
NORETURN void enter_usermode(uint64_t entry_rip, uint64_t user_rsp);

/* Restores every GPR from frame, sanitizes user RFLAGS, executes swapgs and
 * returns to Ring 3 through IRETQ. Never returns. */
NORETURN void user_frame_resume(const syscall_frame_t *frame);

/* Terminates the current thread (and, for the single-thread test tasks, the
 * task). The task-exit hook fires when the last thread drops to zero. */
NORETURN void task_terminate(void);

#endif /* LUCYK_SYSCALL_H */
