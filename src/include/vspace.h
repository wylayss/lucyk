#ifndef LUCYK_VSPACE_H
#define LUCYK_VSPACE_H

#include <gdt.h>
#include <types.h>

#define VSPACE_PROT_WRITE (1U << 0)
#define VSPACE_PROT_EXEC  (1U << 1)
#define VSPACE_PROT_USER  (1U << 2)
#define VSPACE_PROT_PCD   (1U << 3) /* Page-level cache disable */
#define VSPACE_PROT_PWT   (1U << 4) /* Page-level write-through */
#define VSPACE_PROT_OWNED (1U << 5) /* Internal: the vspace owns the leaf frame */
#define VSPACE_PROT_COW   (1U << 6) /* Software: copy-on-write, mapped read-only */
/* Device/MMIO memory outside PMM management (e.g. a linear framebuffer): never
 * OWNED, never executable, never reference counted or freed, shared (not
 * copied) by vspace_clone(), and refused by copy_from_user()/copy_to_user(). */
#define VSPACE_PROT_DEVICE (1U << 7)
/* Write-combining cache type through the PAT entry chosen by cpu.c; falls
 * back to uncached (PCD|PWT) when the CPU has no PAT. Device mappings only. */
#define VSPACE_PROT_WC     (1U << 8)

/* Dedicated higher-half window for guarded per-thread kernel stacks. Each slot
 * is five 4 KiB pages: one unmapped guard page followed by 16 KiB of RW/NX
 * stack. The window ends exactly where the LAPIC MMIO mapping begins. */
#define KSTACK_BASE        0xFFFFB00000000000ULL
#define KSTACK_LIMIT       0xFFFFC00000000000ULL
#define KSTACK_PAGE_COUNT  4U
#define KSTACK_GUARD_PAGES 1U
#define KSTACK_SLOT_PAGES  (KSTACK_PAGE_COUNT + KSTACK_GUARD_PAGES)
#define KSTACK_SLOT_SIZE   (KSTACK_SLOT_PAGES * PAGE_SIZE)

typedef struct vspace {
    uint64_t *pml4;      /* HHDM pointer to the top-level table */
    uint64_t  pml4_phys; /* Physical address programmed into CR3 */
    uint64_t  ref_count;
    uint64_t  active_cpus; /* Bitmask of CPUs that may hold cached TLB entries */
} vspace_t;

/* Acquires the global page-table lock. All lower-half page-table and PTE
 * mutation is serialised by it, which keeps TLB shootdown targets coherent.
 * Hierarchy: vspace -> PMM (never the reverse). */
uint64_t vspace_lock_acquire(void);
void vspace_lock_release(uint64_t flags);

/* Registers/retires the calling CPU as a potential TLB user of vs. Called by
 * the scheduler around CR3 switches. */
void vspace_note_cpu_enter(vspace_t *vs, uint32_t cpu_index);
void vspace_note_cpu_exit(vspace_t *vs, uint32_t cpu_index);

/* Number of completed remote flush cycles per CPU. Every TLB shootdown IPI
 * increments the receiving CPU's counter after its invalidation, so a waiter
 * can require a strictly newer cycle than the one it observed before sending
 * and can never be satisfied by a stale acknowledgement. */
extern volatile uint64_t tlb_flush_sequence[MAX_CPUS];

/* Called from the TLB shootdown IPI handler. Invalidates the announced address
 * (or the whole TLB when the address is zero) and publishes the new cycle. */
void tlb_shootdown_service(void);

/* Sends the shootdown IPI to every CPU in mask that is online and waits until
 * each has completed a flush cycle newer than the one observed at entry. Must
 * be called with no vspace or PMM lock held. */
void tlb_shootdown_mask(uint64_t cpu_mask, vaddr_t vaddr);

/* Captures the current kernel page tables, enables NXE and CR0.WP,
 * pre-allocates a PDPT for every kernel PML4 slot (256..511) so later kernel
 * mappings are shared by every vspace, and maps the ACPI RECLAIMABLE/NVS
 * memory-map regions into the HHDM as reserved RW/NX data pages. */
void vspace_init(void);

/* Allocates a zeroed PML4 and shares the kernel upper half (PML4[256..511]). */
vspace_t *vspace_create(void);

/* Clones src's lower-half user mappings. Owned writable pages become
 * read-only copy-on-write in both address spaces; owned read-only pages are
 * shared directly. Non-owned leaves are copied eagerly. The kernel upper half
 * is shared. Returns NULL on failure after releasing every partial reference. */
vspace_t *vspace_clone(vspace_t *src);

/* Retains one reference to an existing address space. */
void vspace_ref(vspace_t *vspace);

/* Maps one 4 KiB page into a user vspace. The address must be in the lower
 * canonical half, PTE_USER is always set, and WRITE+EXEC is rejected. */
sys_status_t vspace_map_user(vspace_t *vs, vaddr_t vaddr, paddr_t paddr,
                             uint64_t flags);

/* Maps one 4 KiB page into the shared kernel upper half. The address must be
 * >= 0xFFFF800000000000, PTE_USER is forbidden, WRITE+EXEC is rejected, and
 * VSPACE_PROT_PCD/VSPACE_PROT_PWT are honored for MMIO. */
sys_status_t vspace_map_kernel(vaddr_t vaddr, paddr_t paddr, uint64_t flags);

/* Clears a user leaf PTE, invalidates the TLB entry, and drops the frame
 * reference when the mapping is OWNED. */
sys_status_t vspace_unmap(vspace_t *vs, vaddr_t vaddr);

/* Clears a kernel leaf PTE, invalidates the TLB entry, and drops the frame
 * reference when the mapping is OWNED. Used to reclaim guarded kernel stacks. */
sys_status_t vspace_unmap_kernel(vaddr_t vaddr);

/* Installs a temporary 4 KiB identity leaf for `phys` (must be below 2 MiB) in
 * the kernel PML4[0]. The window owns dedicated PDPT/PD/PT frames, never
 * aliases a kernel-half or HHDM table, and is safe to use while an Application
 * Processor is still running at its physical address. Only one window may be
 * installed at a time; installing a second one returns SYS_ERR_BUSY. */
sys_status_t vspace_identity_map(paddr_t phys);

/* Restores the PML4[0] entry that vspace_identity_map() displaced, broadcasts
 * a full TLB shootdown so no CPU retains the low-half translation, and returns
 * the three intermediate frames to the PMM. A no-op when no window is
 * installed. */
void vspace_identity_unmap(paddr_t phys);

/* Resolves a write fault on a present user PTE_COW page. With one reference
 * the PTE is made writable in place; with multiple references the page is
 * copied into a fresh frame and this vspace gets the private copy. Returns
 * true when the mapping is writable afterwards. */
bool vspace_handle_cow_fault(vspace_t *vs, vaddr_t fault_addr);

/* Allocates a five-page guarded kernel-stack slot and maps the upper four
 * pages RW/NX over fresh frames. Returns the slot base (guard page) or 0. */
vaddr_t kstack_alloc(void);

/* Unmaps the four stack pages, releases their frames, and returns the virtual
 * slot to the reuse list. */
void kstack_free(vaddr_t slot_base);

/* Loads the vspace into CR3. */
void vspace_switch(vspace_t *vs);

/* Frees user-half page tables and the top-level table. OWNED leaf frames have
 * one reference dropped; shared frames survive until their last user exits. */
void vspace_destroy(vspace_t *vs);

bool vspace_translate(vspace_t *vs, vaddr_t vaddr, paddr_t *paddr_out);

/* Reports the raw leaf PTE of a kernel-half address in the kernel vspace. */
bool vspace_probe_flags(vaddr_t vaddr, uint64_t *flags_out);

vspace_t *vspace_kernel(void);

/* Bounded user copies that validate the whole range against the current
 * task's user vspace and copy through the HHDM. */
sys_status_t copy_from_user(void *kdst, const void *usrc, size_t len);
sys_status_t copy_to_user(void *udst, const void *ksrc, size_t len);

/* Changes the protection bits of an existing present user leaf PTE. The page
 * must already be mapped. If the leaf is PTE_COW and the new protection
 * includes WRITE, the CoW is resolved first (page copied if shared). The old
 * PTE flags and frame physical address are reported to the caller. Returns
 * SYS_ERR_INVAL when the page is not present/user. The caller is responsible
 * for TLB shootdown after the PTE mutation. */
sys_status_t vspace_remap_user(vspace_t *vs, vaddr_t vaddr, uint64_t new_flags,
                               paddr_t *old_phys_out, uint64_t *old_pte_out);

/* One maximal run of present user pages sharing the same protection, as
 * reported by vspace_user_ranges(). prot holds VSPACE_PROT_WRITE (writable
 * or copy-on-write), VSPACE_PROT_EXEC and VSPACE_PROT_DEVICE. */
typedef struct vspace_range {
    vaddr_t  start;
    vaddr_t  end;   /* Exclusive */
    uint32_t prot;
} vspace_range_t;

/* Walks the lower-half user mappings of vs in ascending address order and
 * calls fn once per coalesced range; a false return stops the walk. fn runs
 * with the page-table lock held, so it must not block or take other locks. */
void vspace_user_ranges(vspace_t *vs,
                        bool (*fn)(const vspace_range_t *range, void *ctx),
                        void *ctx);

#endif /* LUCYK_VSPACE_H */
