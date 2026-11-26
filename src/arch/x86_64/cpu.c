#include <cpu.h>
#include <gdt.h>
#include <klog.h>
#include <spinlock.h>
#include <types.h>

#define IA32_EFER   0xC0000080U
#define EFER_NXE    (1ULL << 11)

#define CPUID_LEAF_FEATURES 7U
#define CPUID_EBX_SMEP (1U << 7)
#define CPUID_EBX_SMAP (1U << 20)

#define CR4_SMEP (1ULL << 20)
#define CR4_SMAP (1ULL << 21)

/* Indexed by cpu_index; the BSP is always index 0. */
cpu_local_t cpu_locals[MAX_CPUS];
uint32_t cpu_online_count;

/* Byte-sized so the NASM ISR entry can test it with a direct memory compare. */
uint8_t cpu_has_smap;

/* One TSS per CPU: LTR marks the descriptor Busy, so sharing one descriptor
 * between CPUs is architecturally invalid. */
static tss64_t cpu_tss_storage[MAX_CPUS] __attribute__((aligned(16)));

/* Kernel virtual address of each CPU's TSS, published for switch.asm, which
 * updates TSS.rsp0 on every context switch without a C call. */
void *const gdt_tss_bases[MAX_CPUS] = {
    &cpu_tss_storage[0],  &cpu_tss_storage[1],  &cpu_tss_storage[2],
    &cpu_tss_storage[3],  &cpu_tss_storage[4],  &cpu_tss_storage[5],
    &cpu_tss_storage[6],  &cpu_tss_storage[7],  &cpu_tss_storage[8],
    &cpu_tss_storage[9],  &cpu_tss_storage[10], &cpu_tss_storage[11],
    &cpu_tss_storage[12], &cpu_tss_storage[13], &cpu_tss_storage[14],
    &cpu_tss_storage[15],
};

static uint32_t read_cr4(void)
{
    uint64_t cr4;

    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    return (uint32_t)cr4;
}

static void write_cr4(uint64_t cr4)
{
    __asm__ volatile("mov %0, %%cr4" : : "r"(cr4) : "memory");
}

static void write_msr(uint32_t msr, uint64_t value)
{
    __asm__ volatile("wrmsr"
                     :
                     : "c"(msr), "a"((uint32_t)value), "d"((uint32_t)(value >> 32))
                     : "memory");
}

void cpu_local_bind(uint32_t cpu_index)
{
    if (cpu_index >= MAX_CPUS) {
        kpanic("cpu_local_bind: cpu %u out of range", cpu_index);
    }

    cpu_local_t *cpu = &cpu_locals[cpu_index];

    cpu->cpu_index = cpu_index;
    cpu->self = (uint64_t)(uintptr_t)cpu;

    /* Kernel invariant: GS_BASE points at the per-CPU block in Ring 0, while
     * KERNEL_GS_BASE holds the user value (zero for kernel-only threads). */
    write_msr(0xC0000101U, (uint64_t)(uintptr_t)cpu);
    write_msr(0xC0000102U, 0);
}

cpu_local_t *cpu_local_for(uint32_t cpu_index)
{
    if (cpu_index >= MAX_CPUS) {
        return NULL;
    }
    return &cpu_locals[cpu_index];
}

tss64_t *cpu_tss(uint32_t cpu_index)
{
    if (cpu_index >= MAX_CPUS) {
        return NULL;
    }
    return &cpu_tss_storage[cpu_index];
}

/* Enables NXE and CR0.WP. Safe to call on every CPU. */
static void cpu_enable_paging_features(void)
{
    uint32_t low;
    uint32_t high;

    __asm__ volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(IA32_EFER));
    uint64_t efer = ((uint64_t)high << 32) | low;

    write_msr(IA32_EFER, efer | EFER_NXE);

    uint64_t cr0;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 |= (1ULL << 16);
    __asm__ volatile("mov %0, %%cr0" : : "r"(cr0) : "memory");
}

static void cpu_enable_ss_features(bool smep, bool smap)
{
    uint64_t cr4 = read_cr4();
    uint64_t updated = cr4;

    if (smep) {
        updated |= CR4_SMEP;
    }
    if (smap) {
        updated |= CR4_SMAP;
    }

    if (updated != cr4) {
        write_cr4(updated);
    }
}

static void cpu_detect_security_features(bool *smep_out, bool *smap_out)
{
    uint32_t eax;
    uint32_t ebx;
    uint32_t ecx;
    uint32_t edx;

    __asm__ volatile("cpuid"
                     : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                     : "a"(0U), "c"(0U));
    UNUSED(ebx);
    UNUSED(ecx);
    UNUSED(edx);

    bool smep = false;
    bool smap = false;

    if (eax >= CPUID_LEAF_FEATURES) {
        __asm__ volatile("cpuid"
                         : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                         : "a"(CPUID_LEAF_FEATURES), "c"(0U));
        UNUSED(eax);
        UNUSED(ecx);
        UNUSED(edx);

        smep = (ebx & CPUID_EBX_SMEP) != 0;
        smap = (ebx & CPUID_EBX_SMAP) != 0;
    }

    *smep_out = smep;
    *smap_out = smap;
}

void cpu_init_security_features(void)
{
    bool smep;
    bool smap;

    cpu_detect_security_features(&smep, &smap);

    cpu_enable_paging_features();

    /* Publish before enabling CR4.SMAP so the ISR entry always agrees with the
     * control register about whether CLAC is legal and required. */
    cpu_has_smap = smap ? 1U : 0U;

    cpu_enable_ss_features(smep, smap);

    klog(KLOG_INFO,
         "CPU security: SMEP=%s SMAP=%s (CR4=0x%016lx)",
         smep ? "enabled" : "unsupported",
         smap ? "enabled" : "unsupported",
         (uint64_t)read_cr4());
}

void cpu_init_ap_features(void)
{
    bool smep;
    bool smap;

    cpu_detect_security_features(&smep, &smap);
    cpu_enable_paging_features();

    /* cpu_has_smap is process-wide and already set by the BSP; SMP hardware is
     * homogeneous, so this only re-asserts the same decision on this core. */
    cpu_enable_ss_features(smep, smap);
}
