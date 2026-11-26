#include <cpu.h>
#include <gdt.h>
#include <string.h>
#include <types.h>

#define GDT_KERNEL_STACK_SIZE 16384
#define EMERGENCY_STACK_SIZE 8192

/* 5 system descriptors + 2 entries per per-CPU TSS. */
#define GDT_ENTRY_COUNT (5 + (MAX_CPUS * 2))

_Static_assert(offsetof(tss64_t, rsp0) == 4, "TSS.rsp0 must be at byte offset 4");
_Static_assert(offsetof(tss64_t, ist) == 36, "TSS.ist must start at byte 36");
_Static_assert(sizeof(tss64_t) == 104, "64-bit TSS must be 104 bytes");

typedef struct PACKED gdtr {
    uint16_t limit;
    uint64_t base;
} gdtr_t;

static uint64_t gdt_entries[GDT_ENTRY_COUNT];
static gdtr_t gdtr_storage;

static uint8_t kernel_stack[GDT_KERNEL_STACK_SIZE] __attribute__((aligned(16)));

/* IST1 services #DF and IST2 services #NMI, so a corrupted or exhausted Ring 0
 * stack still produces a register dump instead of a triple fault. These are
 * shared hardware-exception stacks; normal scheduling never runs on them. */
static uint8_t ist1_stack[EMERGENCY_STACK_SIZE] __attribute__((aligned(16)));
static uint8_t ist2_stack[EMERGENCY_STACK_SIZE] __attribute__((aligned(16)));

_Static_assert(offsetof(tss64_t, ist) == 36, "TSS.ist must start at byte 36");

extern void gdt_load(const gdtr_t *gdtr);
extern void gdt_ltr(uint16_t selector);

static void gdt_set_entry(size_t index, uint32_t base, uint32_t limit,
                          uint8_t access, uint8_t flags)
{
    gdt_entries[index] =
        ((uint64_t)(limit & 0xFFFFU)) |
        (((uint64_t)(base & 0xFFFFFFU)) << 16) |
        ((uint64_t)access << 40) |
        ((uint64_t)((limit >> 16) & 0x0FU) << 48) |
        ((uint64_t)(flags & 0xF0U) << 48) |
        (((uint64_t)((base >> 24) & 0xFFU)) << 56);
}

static void gdt_set_tss(size_t index, uint64_t base, uint32_t limit)
{
    gdt_entries[index] =
        ((uint64_t)(limit & 0xFFFFU)) |
        (((base & 0xFFFFFFULL)) << 16) |
        ((uint64_t)0x89U << 40) |
        ((uint64_t)((limit >> 16) & 0x0FU) << 48) |
        (((base >> 24) & 0xFFULL) << 56);
    gdt_entries[index + 1] = (base >> 32) & 0xFFFFFFFFULL;
}

void gdt_init(void)
{
    memset(gdt_entries, 0, sizeof(gdt_entries));

    /* 0x00 Null */
    gdt_set_entry(0, 0, 0, 0x00, 0x00);
    /* 0x08 Kernel Code (DPL 0, L=1, G=1) */
    gdt_set_entry(1, 0, 0xFFFFFU, 0x9AU, 0xA0);
    /* 0x10 Kernel Data (DPL 0, G=1) */
    gdt_set_entry(2, 0, 0xFFFFFU, 0x92U, 0xC0);
    /* 0x18 User Data (DPL 3, G=1) -> selector 0x1B */
    gdt_set_entry(3, 0, 0xFFFFFU, 0xF2U, 0xC0);
    /* 0x20 User Code (DPL 3, L=1, G=1) -> selector 0x23 */
    gdt_set_entry(4, 0, 0xFFFFFU, 0xFAU, 0xA0);

    size_t tss_entry = 5;

    for (uint32_t cpu = 0; cpu < MAX_CPUS; cpu++) {
        tss64_t *tss = cpu_tss(cpu);

        memset(tss, 0, sizeof(*tss));
        tss->rsp0 = (uint64_t)(uintptr_t)(kernel_stack + sizeof(kernel_stack));
        tss->ist[0] = (uint64_t)(uintptr_t)(ist1_stack + sizeof(ist1_stack));
        tss->ist[1] = (uint64_t)(uintptr_t)(ist2_stack + sizeof(ist2_stack));
        tss->iomap_base = (uint16_t)sizeof(*tss);

        gdt_set_tss(tss_entry, (uint64_t)(uintptr_t)tss,
                    (uint32_t)(sizeof(*tss) - 1));
        tss_entry += 2;
    }

    gdtr_storage.limit = (uint16_t)(sizeof(gdt_entries) - 1);
    gdtr_storage.base = (uint64_t)(uintptr_t)gdt_entries;

    gdt_load(&gdtr_storage);
    gdt_ltr(gdt_tss_selector());
}

uint16_t gdt_tss_selector_for_cpu(uint32_t cpu_index)
{
    if (cpu_index >= MAX_CPUS) {
        return 0;
    }

    return (uint16_t)((5 + (cpu_index * 2)) * sizeof(uint64_t));
}

uint16_t gdt_tss_selector(void)
{
    return gdt_tss_selector_for_cpu(0);
}

void gdt_load_tss_for_cpu(uint32_t cpu_index)
{
    gdt_ltr(gdt_tss_selector_for_cpu(cpu_index));
}

void gdt_descriptor(uint64_t *limit_out, uint64_t *base_out)
{
    if (limit_out != NULL) {
        *limit_out = (uint64_t)gdtr_storage.limit;
    }
    if (base_out != NULL) {
        *base_out = gdtr_storage.base;
    }
}

void gdt_set_kernel_stack(uint64_t rsp0)
{
    cpu_tss(this_cpu_index())->rsp0 = rsp0;
}

uint16_t gdt_kernel_code_selector(void) { return 0x08; }
uint16_t gdt_kernel_data_selector(void) { return 0x10; }
uint16_t gdt_user_code_selector(void)   { return 0x23; }
uint16_t gdt_user_data_selector(void)   { return 0x1B; }
