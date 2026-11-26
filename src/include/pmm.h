#ifndef LUCYK_PMM_H
#define LUCYK_PMM_H

#include <types.h>

struct limine_memmap_response;

/* Builds the bitmap from the bootloader memory map. Frames covering
 * LIMINE_MEMMAP_USABLE are released; everything else stays reserved. */
void pmm_init(struct limine_memmap_response *memmap, uint64_t hhdm_offset);

/* Single zeroed frame, used by the general kernel allocators. */
uint64_t pmm_alloc_frame(void);

/* Physically contiguous zeroed run, reserved for future DMA buffers.
 * The kernel heap deliberately uses single non-contiguous frames. */
uint64_t pmm_alloc_frames(size_t count);

/* Reserved low page (0x8000) holding the AP bootstrap trampoline. */
uint64_t pmm_ap_trampoline_page(void);

/* Allocated frames carry one reference. free/unref drop one reference and
 * return the frame to the bitmap only when the count reaches zero. */
sys_status_t pmm_frame_ref(paddr_t phys);
void pmm_frame_unref(paddr_t phys);
uint16_t pmm_frame_refcount(paddr_t phys);

void pmm_free_frame(uint64_t phys);
void pmm_free_frames(uint64_t phys, size_t count);

size_t pmm_free_frame_count(void);
size_t pmm_total_frame_count(void);

void *pmm_phys_to_virt(uint64_t phys);
uint64_t pmm_virt_to_phys(const void *virt);

#endif /* LUCYK_PMM_H */
