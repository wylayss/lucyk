#include <cpu.h>
#include <gdt.h>
#include <io.h>
#include <klog.h>
#include <sched.h>
#include <syscall.h>
#include <types.h>

#define IA32_EFER          0xC0000080U
#define IA32_STAR          0xC0000081U
#define IA32_LSTAR         0xC0000082U
#define IA32_FMASK         0xC0000084U
#define IA32_GS_BASE       0xC0000101U
#define IA32_KERNEL_GS_BASE 0xC0000102U

#define EFER_SCE (1ULL << 0)
#define X86_RFLAGS_IF (1ULL << 9)
#define X86_RFLAGS_TF (1ULL << 8)
#define X86_RFLAGS_DF (1ULL << 10)
#define X86_RFLAGS_AC (1ULL << 18)

extern void syscall_entry(void);

static uint64_t read_msr(uint32_t msr)
{
    uint32_t low;
    uint32_t high;

    __asm__ volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
    return ((uint64_t)high << 32) | low;
}

static void write_msr(uint32_t msr, uint64_t value)
{
    __asm__ volatile("wrmsr"
                     :
                     : "c"(msr), "a"((uint32_t)value), "d"((uint32_t)(value >> 32))
                     : "memory");
}

/* Shared by the BSP and every AP: the MSRs are per-CPU hardware state, so each
 * core must program its own STAR/LSTAR/FMASK and GS bases. */
void syscall_init_ap(void)
{
    uint64_t efer = read_msr(IA32_EFER);

    write_msr(IA32_EFER, efer | EFER_SCE);

    /* Kernel CS = 0x08, kernel SS = 0x10. The SYSRET base 0x10 makes the
     * hardware produce SS 0x18|3 = 0x1B and CS 0x20|3 = 0x23. */
    write_msr(IA32_STAR,
              ((uint64_t)0x0010U << 48) | ((uint64_t)0x0008U << 32));
    write_msr(IA32_LSTAR, (uint64_t)(uintptr_t)syscall_entry);
    write_msr(IA32_FMASK, X86_RFLAGS_IF | X86_RFLAGS_TF | X86_RFLAGS_DF |
                              X86_RFLAGS_AC);
}

void syscall_init(void)
{
    syscall_init_ap();

    /* Kernel invariant: while executing in Ring 0, GS_BASE is this CPU's
     * cpu_local block; while executing in Ring 3 it is zero and
     * KERNEL_GS_BASE holds the block. SYSCALL/ISR entry and IRETQ/SYSRETQ exit
     * preserve the invariant. */
    cpu_local_bind(this_cpu_index());

    uint64_t boot_stack_top = sched_boot_stack_top();
    gdt_set_kernel_stack(boot_stack_top);

    cpu_local_for(this_cpu_index())->kernel_rsp = boot_stack_top;
    cpu_local_for(this_cpu_index())->user_rsp_scratch = 0;
}

NORETURN void enter_usermode(uint64_t entry_rip, uint64_t user_rsp)
{
    /* Keep interrupts disabled across swapgs and IRETQ so no ISR can observe
     * a Ring 0 execution context with the user GS base installed. */
    __asm__ volatile("cli; swapgs" : : : "memory");

    __asm__ volatile(
        "pushq $0x1B\n"
        "pushq %0\n"
        "pushq $0x202\n"
        "pushq $0x23\n"
        "pushq %1\n"
        /* A fresh image starts with argc/argv/envp registers cleared, so a
         * program entered this way (no exec vector) observes argc == 0. */
        "xorl %%edi, %%edi\n"
        "xorl %%esi, %%esi\n"
        "xorl %%edx, %%edx\n"
        "iretq\n"
        :
        : "r"(user_rsp), "r"(entry_rip)
        : "rdi", "rsi", "rdx", "memory");

    __builtin_unreachable();
}
