#ifndef LUCYK_ISR_H
#define LUCYK_ISR_H

#include <stdint.h>

/* Layout produced by src/arch/x86_64/isr.asm: isr_common pushes the general
 * purpose registers after the stub has pushed vector and error code. */
typedef struct interrupt_frame {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t vector;
    uint64_t error;
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
    uint64_t rsp;
    uint64_t ss;
} interrupt_frame_t;

_Static_assert(sizeof(interrupt_frame_t) == 22 * sizeof(uint64_t),
               "interrupt_frame_t must match the isr.asm push order");

void isr_dispatch(interrupt_frame_t *frame);

#endif /* LUCYK_ISR_H */
