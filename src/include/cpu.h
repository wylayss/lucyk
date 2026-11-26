#ifndef LUCYK_CPU_H
#define LUCYK_CPU_H

#include <gdt.h>
#include <stdbool.h>
#include <stdint.h>

#define CPU_INDEX_NONE 0xFFFFFFFFU

/* Bytes of per-CPU state addressed through GS while the kernel is entered from
 * user mode. kernel_rsp and user_rsp_scratch must stay at offsets 0 and 8:
 * syscall.asm and switch.asm use those fixed offsets. self at offset 48 lets
 * this_cpu() recover the block base without touching the fixed slots. */
typedef struct cpu_local {
    uint64_t kernel_rsp;        /* 0  */
    uint64_t user_rsp_scratch;  /* 8  */
    uint32_t cpu_index;         /* 16 */
    uint32_t lapic_id;          /* 20 */
    uint64_t current_thread;    /* 24: thread_t * */
    uint64_t idle_thread;       /* 32: thread_t * */
    uint64_t online;            /* 40: non-zero once the CPU is running */
    uint64_t self;              /* 48: kernel VA of this block */
} cpu_local_t;

_Static_assert(offsetof(cpu_local_t, kernel_rsp) == 0, "kernel_rsp offset");
_Static_assert(offsetof(cpu_local_t, user_rsp_scratch) == 8,
               "user_rsp_scratch offset");
_Static_assert(offsetof(cpu_local_t, cpu_index) == 16, "cpu_index offset");
_Static_assert(offsetof(cpu_local_t, lapic_id) == 20, "lapic_id offset");
_Static_assert(offsetof(cpu_local_t, current_thread) == 24,
               "current_thread offset");
_Static_assert(offsetof(cpu_local_t, self) == 48, "self offset");

/* One block per CPU. The BSP uses index 0. */
extern cpu_local_t cpu_locals[MAX_CPUS];
extern uint32_t cpu_online_count;
extern uint8_t cpu_has_smap;

/* Installs GS_BASE for the calling CPU and clears KERNEL_GS_BASE. */
void cpu_local_bind(uint32_t cpu_index);

cpu_local_t *cpu_local_for(uint32_t cpu_index);
tss64_t *cpu_tss(uint32_t cpu_index);

/* Active CPU index, read through GS. Valid in Ring 0 on every CPU. */
static inline uint32_t this_cpu_index(void)
{
    uint32_t index;

    __asm__ volatile("movl %%gs:16, %0" : "=r"(index));
    return index;
}

/* Active CPU's per-CPU block, read through its GS-resident self pointer. */
static inline cpu_local_t *this_cpu(void)
{
    cpu_local_t *cpu;

    __asm__ volatile("movq %%gs:48, %0" : "=r"(cpu));
    return cpu;
}

/* Detects SMEP/SMAP through CPUID and enables the supported CR4 bits. */
void cpu_init_security_features(void);

/* Enables NXE, CR0.WP, SMEP and SMAP on an Application Processor, plus the
 * same x87/SSE (CR0.MP/EM/TS, CR4.OSFXSR/OSXMMEXCPT, FNINIT, MXCSR 0x1F80)
 * and PAT setup the BSP receives from cpu_init_security_features(). */
void cpu_init_ap_features(void);

/* IA32_PAT entry (4..7) that selects write-combining on every CPU, or 0 when
 * PAT is unavailable and device mappings fall back to uncached. */
uint32_t cpu_write_combining_pat_entry(void);

/* CPUID leaf 0 vendor identification ("GenuineIntel", "AuthenticAMD", ...). */
void cpu_vendor_string(char out[13]);

#endif /* LUCYK_CPU_H */
