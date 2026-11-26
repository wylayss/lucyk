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

#define CPUID_LEAF_BASIC   1U
#define CPUID_EDX_FPU   (1U << 0)
#define CPUID_EDX_PAT   (1U << 16)
#define CPUID_EDX_FXSR  (1U << 24)
#define CPUID_EDX_SSE   (1U << 25)
#define CPUID_EDX_SSE2  (1U << 26)

#define CR0_MP (1ULL << 1)
#define CR0_EM (1ULL << 2)
#define CR0_TS (1ULL << 3)
#define CR0_NE (1ULL << 5)

#define CR4_OSFXSR     (1ULL << 9)
#define CR4_OSXMMEXCPT (1ULL << 10)

#define FPU_DEFAULT_MXCSR 0x1F80U

/* Write-combining for device framebuffers comes from a PAT entry in the upper
 * half (selected by PTE.PAT=1). An entry the firmware/bootloader already set
 * to WC is reused; otherwise entry 7 is reprogrammed. Entries 0-3 (selected by
 * PCD/PWT alone) are never touched, so existing UC/WT mappings keep their
 * meaning. The BSP decides the final IA32_PAT value and every AP loads the
 * identical value, as the architecture requires. */
#define IA32_PAT       0x277U
#define PAT_TYPE_WC    0x01ULL
#define PAT_WC_FALLBACK_ENTRY 7U

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

static uint64_t read_cr0(void)
{
    uint64_t cr0;

    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    return cr0;
}

static void write_cr0(uint64_t cr0)
{
    __asm__ volatile("mov %0, %%cr0" : : "r"(cr0) : "memory");
}

static uint64_t read_msr(uint32_t msr)
{
    uint32_t low;
    uint32_t high;

    __asm__ volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
    return ((uint64_t)high << 32) | low;
}

static uint32_t cpuid_basic_edx(void)
{
    uint32_t eax;
    uint32_t ebx;
    uint32_t ecx;
    uint32_t edx;

    __asm__ volatile("cpuid"
                     : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                     : "a"(CPUID_LEAF_BASIC), "c"(0U));
    UNUSED(eax);
    UNUSED(ebx);
    UNUSED(ecx);
    return edx;
}

static uint32_t cpu_pat_wc_entry; /* 0 = unavailable, else 4..7 */
static uint64_t cpu_pat_value;

uint32_t cpu_write_combining_pat_entry(void)
{
    return cpu_pat_wc_entry;
}

/* Hardware FPU/SSE enablement shared by the BSP and every AP. All FPU
 * instructions live in these asm blocks: the C kernel itself is built with
 * -mno-sse -mno-80387 and never emits them. */
static void cpu_enable_fpu(bool log)
{
    uint32_t edx = cpuid_basic_edx();
    uint32_t required = CPUID_EDX_FPU | CPUID_EDX_FXSR | CPUID_EDX_SSE |
                        CPUID_EDX_SSE2;

    if ((edx & required) != required) {
        kpanic("cpu: x87/FXSR/SSE/SSE2 unsupported (cpuid.1.edx=0x%08lx)",
               (uint64_t)edx);
    }

    /* CR0: MP=1 (WAIT honours TS), EM=0 (no x87 emulation), TS=0 (eager
     * switching, no #NM), NE=1 (native x87 error reporting). */
    uint64_t cr0 = read_cr0();
    cr0 |= CR0_MP | CR0_NE;
    cr0 &= ~(CR0_EM | CR0_TS);
    write_cr0(cr0);

    /* CR4: OSFXSR enables FXSAVE/FXRSTOR and SSE, OSXMMEXCPT routes unmasked
     * SIMD exceptions to #XM. */
    write_cr4(read_cr4() | CR4_OSFXSR | CR4_OSXMMEXCPT);

    uint32_t mxcsr = FPU_DEFAULT_MXCSR;
    __asm__ volatile("fninit\n\tldmxcsr %0" : : "m"(mxcsr) : "memory");

    uint32_t check = 0;
    __asm__ volatile("stmxcsr %0" : "=m"(check) : : "memory");
    if (check != FPU_DEFAULT_MXCSR) {
        kpanic("cpu: MXCSR readback 0x%08lx", (uint64_t)check);
    }

    if ((edx & CPUID_EDX_PAT) != 0) {
        if (log) {
            uint64_t pat = read_msr(IA32_PAT);
            uint32_t entry = 0;

            for (uint32_t i = 4; i < 8U; i++) {
                if (((pat >> (i * 8U)) & 0x7ULL) == PAT_TYPE_WC) {
                    entry = i;
                    break;
                }
            }
            if (entry == 0) {
                uint64_t shift = (uint64_t)PAT_WC_FALLBACK_ENTRY * 8ULL;

                pat &= ~(0xFFULL << shift);
                pat |= PAT_TYPE_WC << shift;
                entry = PAT_WC_FALLBACK_ENTRY;
            }
            cpu_pat_value = pat;
            cpu_pat_wc_entry = entry;
        }
        if (cpu_pat_wc_entry != 0) {
            write_msr(IA32_PAT, cpu_pat_value);
        }
    }

    if (log) {
        klog(KLOG_INFO,
             "CPU FPU/SSE: x87+SSE2 enabled (CR0=0x%016lx CR4=0x%016lx "
             "MXCSR=0x%04lx), FXSAVE64 per-thread context, WC PAT entry %lu",
             read_cr0(), (uint64_t)read_cr4(), (uint64_t)check,
             (uint64_t)cpu_pat_wc_entry);
    }
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
    cpu_enable_fpu(true);

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

    /* Every core needs its own CR0/CR4/MXCSR/PAT setup before its first
     * context switch executes FXRSTOR64. */
    cpu_enable_fpu(false);
}

void cpu_vendor_string(char out[13])
{
    uint32_t eax;
    uint32_t ebx;
    uint32_t ecx;
    uint32_t edx;

    __asm__ volatile("cpuid"
                     : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                     : "a"(0U), "c"(0U));
    UNUSED(eax);

    uint32_t words[3] = { ebx, edx, ecx };

    for (uint32_t i = 0; i < 12U; i++) {
        out[i] = (char)((words[i / 4U] >> ((i % 4U) * 8U)) & 0xFFU);
    }
    out[12] = '\0';
}
