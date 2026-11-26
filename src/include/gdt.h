#ifndef LUCYK_GDT_H
#define LUCYK_GDT_H

#include <stddef.h>
#include <stdint.h>
#include <types.h>

/* Per-CPU count. Every CPU needs its own TSS because the hardware sets the
 * TSS Busy bit (0x8B) in the descriptor on LTR, which makes one descriptor
 * unusable by a second CPU. */
#define MAX_CPUS 16

/* 64-bit Task State Segment (IA-32e TSS). */
typedef struct PACKED tss64 {
    uint32_t reserved0;
    uint64_t rsp0;
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist[7];
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
} tss64_t;

/* SYSRET-compatible GDT prefix: 0x00 Null, 0x08 KCode, 0x10 KData,
 * 0x18 UData, 0x20 UCode. Per-CPU TSS descriptors follow at 0x28 + 16 * cpu. */
void gdt_init(void);

/* Installs the TSS descriptor for cpu_index and executes LTR on the calling
 * CPU. Each CPU must call this once with its own descriptor. */
void gdt_load_tss_for_cpu(uint32_t cpu_index);

/* Selector of the TSS descriptor owned by cpu_index. */
uint16_t gdt_tss_selector_for_cpu(uint32_t cpu_index);
uint16_t gdt_tss_selector(void);

/* Fill a 10-byte descriptor table pointer for AP trampoline handoff. */
void gdt_descriptor(uint64_t *limit_out, uint64_t *base_out);

void gdt_set_kernel_stack(uint64_t rsp0);

uint16_t gdt_kernel_code_selector(void);
uint16_t gdt_kernel_data_selector(void);
uint16_t gdt_user_code_selector(void);
uint16_t gdt_user_data_selector(void);

#endif /* LUCYK_GDT_H */
