#include <cpu.h>
#include <io.h>
#include <kalloc.h>
#include <klog.h>
#include <lapic.h>
#include <limine.h>
#include <pmm.h>
#include <sched.h>
#include <smp.h>
#include <spinlock.h>
#include <string.h>
#include <task.h>
#include <types.h>
#include <vspace.h>

extern volatile struct limine_hhdm_request hhdm_request;
extern volatile struct limine_memmap_request memmap_request;

#define IA32_EFER 0xC0000080U
#define EFER_NXE  (1ULL << 11)

#define PTE_PRESENT  (1ULL << 0)
#define PTE_WRITABLE (1ULL << 1)
#define PTE_USER     (1ULL << 2)
#define PTE_PWT      (1ULL << 3)
#define PTE_PCD      (1ULL << 4)
#define PTE_ACCESSED (1ULL << 5)
#define PTE_DIRTY    (1ULL << 6)
#define PTE_HUGE     (1ULL << 7)
#define PTE_COW      (1ULL << 9)  /* Software bit: copy-on-write page */
#define PTE_OWNED    (1ULL << 10) /* Software bit: vspace owns this leaf frame */
#define PTE_DEVICE   (1ULL << 11) /* Software bit: non-PMM device memory */
#define PTE_PAT_4K   (1ULL << 7)  /* PAT selector bit of a 4 KiB leaf */
#define PTE_NX       (1ULL << 63)

#define PTE_ADDR_MASK 0x000FFFFFFFFFF000ULL

#define PML4_INDEX(v) (((v) >> 39) & 0x1FFULL)
#define PDPT_INDEX(v) (((v) >> 30) & 0x1FFULL)
#define PD_INDEX(v)   (((v) >> 21) & 0x1FFULL)
#define PT_INDEX(v)   (((v) >> 12) & 0x1FFULL)

#define KERNEL_HALF_BASE 0xFFFF800000000000ULL
#define USER_MAX_ADDR    0x00007FFFFFFFFFFFULL

#define KERNEL_PML4_FIRST 256

static vspace_t kernel_vspace;

/* Serialises every page-table and PTE mutation. The hierarchy is
 * vspace -> PMM: nothing takes the PMM lock before this one, and this lock is
 * never held while an IPI acknowledgement is awaited. */
static spinlock_t vspace_global_lock;

/* Software bits occupy the address mask's low bits? No: the mask keeps only
 * bits 12..51, so PTE_COW (bit 9), PTE_OWNED (bit 10) and PTE_DEVICE (bit 11)
 * never alias a frame address. */

static void tlb_invalidate(vaddr_t vaddr)
{
    __asm__ volatile("invlpg (%0)" : : "r"(vaddr) : "memory");
}

uint64_t vspace_lock_acquire(void)
{
    return spin_lock_irqsave(&vspace_global_lock);
}

void vspace_lock_release(uint64_t flags)
{
    spin_unlock_irqrestore(&vspace_global_lock, flags);
}

void vspace_note_cpu_enter(vspace_t *vs, uint32_t cpu_index)
{
    if (vs == NULL || cpu_index >= 64U) {
        return;
    }

    uint64_t flags = vspace_lock_acquire();
    vs->active_cpus |= (1ULL << cpu_index);
    vspace_lock_release(flags);
}

void vspace_note_cpu_exit(vspace_t *vs, uint32_t cpu_index)
{
    if (vs == NULL || cpu_index >= 64U) {
        return;
    }

    uint64_t flags = vspace_lock_acquire();
    vs->active_cpus &= ~(1ULL << cpu_index);
    vspace_lock_release(flags);
}

/* Flushes one address on every other online CPU. Must be called with no
 * vspace or PMM lock held: a peer that needs the vspace lock keeps its
 * interrupts disabled and can only answer after that lock is released. */
static void tlb_shootdown(vaddr_t vaddr, uint64_t cpu_mask)
{
    tlb_shootdown_mask(cpu_mask, vaddr);
}

static uint64_t mask_without_self(uint64_t mask)
{
    uint32_t self = this_cpu_index();

    if (self >= 64U) {
        return mask;
    }
    return mask & ~(1ULL << self);
}

/* Installs a leaf PTE with the vspace lock already held. Forward-declared
 * because vspace_clone() uses it before its definition. */
static sys_status_t map_locked(vspace_t *vs, vaddr_t vaddr, paddr_t paddr,
                               uint64_t flags, bool user,
                               paddr_t *release_phys, bool *replaced);

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

static bool cpu_supports_nx(void)
{
    uint32_t eax;
    uint32_t ebx;
    uint32_t ecx;
    uint32_t edx;

    __asm__ volatile("cpuid"
                     : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                     : "a"(0x80000000U), "c"(0U));
    UNUSED(ebx);
    UNUSED(ecx);

    if (eax < 0x80000001U) {
        return false;
    }

    __asm__ volatile("cpuid"
                     : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                     : "a"(0x80000001U), "c"(0U));
    UNUSED(eax);
    UNUSED(ebx);
    UNUSED(ecx);

    return (edx & (1U << 20)) != 0;
}

static void cpu_enable_nxe(void)
{
    uint64_t efer = read_msr(IA32_EFER);
    write_msr(IA32_EFER, efer | EFER_NXE);
}

static void cpu_enable_write_protect(void)
{
    uint64_t cr0;

    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 |= (1ULL << 16);
    __asm__ volatile("mov %0, %%cr0" : : "r"(cr0) : "memory");
}

/* True when `vaddr` already has a kernel-half translation, including one from
 * a huge ancestor (Limine maps the HHDM with large pages; walk() refuses
 * those). Only called from vspace_init(), before the vspace is shared. */
static bool kernel_mapping_present(vaddr_t vaddr)
{
    uint64_t entry = kernel_vspace.pml4[PML4_INDEX(vaddr)];

    if ((entry & PTE_PRESENT) == 0) {
        return false;
    }
    if ((entry & PTE_HUGE) != 0) {
        return true;
    }

    uint64_t *pdpt = (uint64_t *)pmm_phys_to_virt(entry & PTE_ADDR_MASK);
    entry = pdpt[PDPT_INDEX(vaddr)];
    if ((entry & PTE_PRESENT) == 0) {
        return false;
    }
    if ((entry & PTE_HUGE) != 0) {
        return true;
    }

    uint64_t *pd = (uint64_t *)pmm_phys_to_virt(entry & PTE_ADDR_MASK);
    entry = pd[PD_INDEX(vaddr)];
    if ((entry & PTE_PRESENT) == 0) {
        return false;
    }
    if ((entry & PTE_HUGE) != 0) {
        return true;
    }

    uint64_t *pt = (uint64_t *)pmm_phys_to_virt(entry & PTE_ADDR_MASK);
    return (pt[PT_INDEX(vaddr)] & PTE_PRESENT) != 0;
}

/* Limine's HHDM omits ACPI RECLAIMABLE/NVS regions, so the RSDP, XSDT and
 * MADT can fault when they live in one. Map every page of those regions at
 * `hhdm + base` RW/NX, leaving already-mapped (large-page) ranges alone. The
 * frames stay reserved: no OWNED flag, pmm_init() never frees them. */
static void map_acpi_hhdm_ranges(void)
{
    struct limine_memmap_response *memmap = memmap_request.response;

    if (memmap == NULL || hhdm_request.response == NULL) {
        return;
    }

    uint64_t hhdm = hhdm_request.response->offset;
    uint64_t mapped = 0;
    uint64_t failed = 0;

    for (uint64_t i = 0; i < memmap->entry_count; i++) {
        struct limine_memmap_entry *entry = memmap->entries[i];

        if (entry->type != LIMINE_MEMMAP_ACPI_RECLAIMABLE &&
            entry->type != LIMINE_MEMMAP_ACPI_NVS) {
            continue;
        }

        uint64_t end = entry->base + entry->length;
        if (entry->length == 0 || end < entry->base) {
            continue;
        }

        for (paddr_t phys = PAGE_ALIGN_DOWN(entry->base);
             phys < PAGE_ALIGN_UP(end); phys += PAGE_SIZE) {
            vaddr_t vaddr = hhdm + phys;

            if (kernel_mapping_present(vaddr)) {
                continue;
            }
            if (vspace_map_kernel(vaddr, phys, VSPACE_PROT_WRITE) != SYS_OK) {
                failed++;
                continue;
            }
            mapped++;
        }
    }

    klog(KLOG_DEBUG, "vspace: mapped %lu ACPI/NVS HHDM pages", mapped);
    if (failed != 0) {
        klog(KLOG_WARN, "vspace: %lu ACPI/NVS HHDM pages unmapped", failed);
    }
}

void vspace_init(void)
{
    uint64_t cr3;
    size_t preallocated = 0;

    spinlock_init(&vspace_global_lock);

    if (!cpu_supports_nx()) {
        kpanic("vspace_init: CPU does not support NX");
    }
    cpu_enable_nxe();
    cpu_enable_write_protect();

    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));

    kernel_vspace.pml4_phys = cr3 & PTE_ADDR_MASK;
    kernel_vspace.pml4 = (uint64_t *)pmm_phys_to_virt(kernel_vspace.pml4_phys);
    kernel_vspace.ref_count = 1;
    kernel_vspace.active_cpus = 1;

    /* Give every kernel PML4 slot a PDPT now. User vspaces copy PML4[256..511]
     * by value, so sharing the tables (not just the entries) makes every later
     * vspace_map_kernel() visible across all tasks without PML4 resync. */
    for (size_t i = KERNEL_PML4_FIRST; i < 512; i++) {
        if ((kernel_vspace.pml4[i] & PTE_PRESENT) != 0) {
            continue;
        }

        uint64_t phys = pmm_alloc_frame();
        if (phys == 0) {
            kpanic("vspace_init: cannot pre-allocate kernel PDPT %lu", i);
        }

        kernel_vspace.pml4[i] = phys | PTE_PRESENT | PTE_WRITABLE;
        preallocated++;
    }

    klog(KLOG_DEBUG, "vspace: pre-allocated %lu kernel PDPTs", preallocated);

    map_acpi_hhdm_ranges();
}

vspace_t *vspace_kernel(void)
{
    return &kernel_vspace;
}

void vspace_ref(vspace_t *vspace)
{
    if (vspace == NULL) {
        return;
    }

    uint64_t flags = vspace_lock_acquire();
    vspace->ref_count++;
    vspace_lock_release(flags);
}

/* Returns the next-level table, optionally allocating it. Intermediate entries
 * only carry PTE_USER for user mappings; kernel mappings must stay Ring 0.
 * Caller must hold the vspace lock. */
static uint64_t *next_table(uint64_t *table, size_t index, bool allocate,
                            bool user)
{
    uint64_t entry = table[index];

    if ((entry & PTE_PRESENT) == 0) {
        if (!allocate) {
            return NULL;
        }

        uint64_t phys = pmm_alloc_frame();
        if (phys == 0) {
            return NULL;
        }

        entry = phys | PTE_PRESENT | PTE_WRITABLE;
        if (user) {
            entry |= PTE_USER;
        }

        table[index] = entry;
        return (uint64_t *)pmm_phys_to_virt(phys);
    }

    if ((entry & PTE_HUGE) != 0) {
        return NULL;
    }

    return (uint64_t *)pmm_phys_to_virt(entry & PTE_ADDR_MASK);
}

/* Caller must hold the vspace lock. */
static uint64_t *walk(vspace_t *vs, vaddr_t vaddr, bool allocate, bool user)
{
    uint64_t *pdpt = next_table(vs->pml4, PML4_INDEX(vaddr), allocate, user);
    if (pdpt == NULL) {
        return NULL;
    }

    uint64_t *pd = next_table(pdpt, PDPT_INDEX(vaddr), allocate, user);
    if (pd == NULL) {
        return NULL;
    }

    uint64_t *pt = next_table(pd, PD_INDEX(vaddr), allocate, user);
    if (pt == NULL) {
        return NULL;
    }

    return &pt[PT_INDEX(vaddr)];
}

vspace_t *vspace_create(void)
{
    uint64_t pml4_phys = pmm_alloc_frame();
    if (pml4_phys == 0) {
        return NULL;
    }

    vspace_t *vs = (vspace_t *)kalloc(sizeof(vspace_t));
    if (vs == NULL) {
        pmm_free_frame(pml4_phys);
        return NULL;
    }

    uint64_t *pml4 = (uint64_t *)pmm_phys_to_virt(pml4_phys);

    /* Lower half stays empty (fresh frame); share the kernel upper half. */
    for (size_t i = KERNEL_PML4_FIRST; i < 512; i++) {
        pml4[i] = kernel_vspace.pml4[i];
    }

    vs->pml4 = pml4;
    vs->pml4_phys = pml4_phys;
    vs->ref_count = 1;
    vs->active_cpus = 0;

    return vs;
}

/* Clones every present PTE_USER leaf in src's lower half. Owned writable pages
 * become read-only COW in both address spaces, owned read-only pages are shared
 * through an extra frame reference, and non-owned leaves are copied eagerly.
 * The source PTE conversion makes the source address space take a COW fault on
 * its next write, so no user page is duplicated at clone time. */
vspace_t *vspace_clone(vspace_t *src)
{
    if (src == NULL) {
        return NULL;
    }

    vspace_t *dst = vspace_create();
    if (dst == NULL) {
        return NULL;
    }

    uint64_t lock = vspace_lock_acquire();

    for (uint64_t pml4_index = 0; pml4_index < KERNEL_PML4_FIRST;
         pml4_index++) {
        uint64_t pml4_entry = src->pml4[pml4_index];

        if ((pml4_entry & PTE_PRESENT) == 0) {
            continue;
        }
        if ((pml4_entry & PTE_HUGE) != 0) {
            goto fail;
        }

        uint64_t *pdpt =
            (uint64_t *)pmm_phys_to_virt(pml4_entry & PTE_ADDR_MASK);

        for (size_t pdpt_index = 0; pdpt_index < 512; pdpt_index++) {
            uint64_t pdpt_entry = pdpt[pdpt_index];

            if ((pdpt_entry & PTE_PRESENT) == 0) {
                continue;
            }
            if ((pdpt_entry & PTE_HUGE) != 0) {
                goto fail;
            }

            uint64_t *pd =
                (uint64_t *)pmm_phys_to_virt(pdpt_entry & PTE_ADDR_MASK);

            for (size_t pd_index = 0; pd_index < 512; pd_index++) {
                uint64_t pd_entry = pd[pd_index];

                if ((pd_entry & PTE_PRESENT) == 0) {
                    continue;
                }
                if ((pd_entry & PTE_HUGE) != 0) {
                    goto fail;
                }

                uint64_t *pt =
                    (uint64_t *)pmm_phys_to_virt(pd_entry & PTE_ADDR_MASK);

                for (size_t pt_index = 0; pt_index < 512; pt_index++) {
                    uint64_t pte = pt[pt_index];

                    if ((pte & PTE_PRESENT) == 0 ||
                        (pte & PTE_USER) == 0) {
                        continue;
                    }

                    /* A writable, executable leaf would violate W^X. */
                    if ((pte & PTE_WRITABLE) != 0 &&
                        (pte & PTE_NX) == 0) {
                        goto fail;
                    }

                    vaddr_t vaddr = (pml4_index << 39) |
                                    (pdpt_index << 30) |
                                    (pd_index << 21) |
                                    (pt_index << 12);

                    if ((pte & PTE_OWNED) != 0) {
                        paddr_t frame = pte & PTE_ADDR_MASK;

                        if (pmm_frame_ref(frame) != SYS_OK) {
                            goto fail;
                        }

                        uint64_t map_flags = VSPACE_PROT_OWNED;

                        if ((pte & PTE_WRITABLE) != 0) {
                            /* Both address spaces observe the page read-only
                             * until either one writes to it. */
                            uint64_t parent_pte = pte & ~PTE_WRITABLE;
                            parent_pte |= PTE_COW;
                            pt[pt_index] = parent_pte;
                            tlb_invalidate(vaddr);
                            map_flags |= VSPACE_PROT_COW;
                        } else if ((pte & PTE_COW) != 0) {
                            map_flags |= VSPACE_PROT_COW;
                        } else if ((pte & PTE_NX) == 0) {
                            map_flags |= VSPACE_PROT_EXEC;
                        }

                        paddr_t replaced_phys = 0;
                        bool replaced = false;
                        if (map_locked(dst, vaddr, frame, map_flags, true,
                                       &replaced_phys, &replaced) != SYS_OK) {
                            pmm_frame_unref(frame);
                            goto fail;
                        }
                        if (replaced_phys != 0) {
                            pmm_frame_unref(replaced_phys);
                        }
                    } else if ((pte & PTE_DEVICE) != 0) {
                        /* Device memory is shared, never copied: the clone
                         * aliases the same MMIO range with identical cache
                         * attributes and takes no frame reference. */
                        uint64_t *dst_slot = walk(dst, vaddr, true, true);

                        if (dst_slot == NULL) {
                            goto fail;
                        }
                        *dst_slot = pte & ~(PTE_ACCESSED | PTE_DIRTY);
                    } else {
                        paddr_t frame = pmm_alloc_frame();
                        if (frame == 0) {
                            goto fail;
                        }

                        memcpy(pmm_phys_to_virt(frame),
                               pmm_phys_to_virt(pte & PTE_ADDR_MASK),
                               PAGE_SIZE);

                        uint64_t map_flags = VSPACE_PROT_OWNED;
                        if ((pte & PTE_WRITABLE) != 0) {
                            map_flags |= VSPACE_PROT_WRITE;
                        } else if ((pte & PTE_NX) == 0) {
                            map_flags |= VSPACE_PROT_EXEC;
                        }

                        paddr_t replaced_phys = 0;
                        bool replaced = false;
                        if (map_locked(dst, vaddr, frame, map_flags, true,
                                       &replaced_phys, &replaced) != SYS_OK) {
                            pmm_frame_unref(frame);
                            goto fail;
                        }
                        if (replaced_phys != 0) {
                            pmm_frame_unref(replaced_phys);
                        }
                    }
                }
            }
        }
    }

    vspace_lock_release(lock);

    /* The source gained read-only COW entries that peers may have cached as
     * writable. Flush them before any of those peers can write to the page. */
    tlb_shootdown_mask(mask_without_self(src->active_cpus), 0);

    return dst;

fail:
    vspace_lock_release(lock);
    vspace_destroy(dst);
    return NULL;
}

static bool build_leaf_flags(paddr_t paddr, uint64_t flags, bool user,
                             uint64_t *pte_out)
{
    if ((flags & VSPACE_PROT_WRITE) != 0 && (flags & VSPACE_PROT_EXEC) != 0) {
        return false;
    }
    /* COW is strictly a read-only data mapping. Allowing WRITE or EXEC here
     * would create a PTE that bypasses the CoW fault path. */
    if ((flags & VSPACE_PROT_COW) != 0 &&
        (flags & (VSPACE_PROT_WRITE | VSPACE_PROT_EXEC)) != 0) {
        return false;
    }
    /* Device memory is never executable, never owned by a vspace and never
     * copy-on-write; WC is only meaningful for it. */
    if ((flags & VSPACE_PROT_DEVICE) != 0 &&
        (flags & (VSPACE_PROT_EXEC | VSPACE_PROT_OWNED | VSPACE_PROT_COW)) !=
            0) {
        return false;
    }
    if ((flags & VSPACE_PROT_WC) != 0 && (flags & VSPACE_PROT_DEVICE) == 0) {
        return false;
    }

    uint64_t pte = paddr | PTE_PRESENT;

    if ((flags & VSPACE_PROT_DEVICE) != 0) {
        pte |= PTE_DEVICE | PTE_NX;

        if ((flags & VSPACE_PROT_WC) != 0) {
            uint32_t entry = cpu_write_combining_pat_entry();

            if (entry != 0) {
                pte |= PTE_PAT_4K;
                if ((entry & 2U) != 0) {
                    pte |= PTE_PCD;
                }
                if ((entry & 1U) != 0) {
                    pte |= PTE_PWT;
                }
            } else {
                pte |= PTE_PCD | PTE_PWT;
            }
        }
    }

    if (user) {
        pte |= PTE_USER;
    }
    if ((flags & VSPACE_PROT_PCD) != 0) {
        pte |= PTE_PCD;
    }
    if ((flags & VSPACE_PROT_PWT) != 0) {
        pte |= PTE_PWT;
    }
    if ((flags & VSPACE_PROT_OWNED) != 0) {
        pte |= PTE_OWNED;
    }
    if ((flags & VSPACE_PROT_COW) != 0) {
        pte |= PTE_COW;
    }

    if ((flags & VSPACE_PROT_WRITE) != 0) {
        /* Writable implies non-executable: W^X. */
        pte |= PTE_WRITABLE | PTE_NX;
    } else if ((flags & VSPACE_PROT_EXEC) == 0) {
        /* Plain data mapping: readable and non-executable. */
        pte |= PTE_NX;
    }

    *pte_out = pte;
    return true;
}

/* Installs a leaf PTE. Caller must hold the vspace lock. Returns any frame
 * whose last reference must be released by the caller after TLB flushes. */
static sys_status_t map_locked(vspace_t *vs, vaddr_t vaddr, paddr_t paddr,
                               uint64_t flags, bool user,
                               paddr_t *release_phys, bool *replaced)
{
    uint64_t pte;

    if (!build_leaf_flags(paddr, flags, user, &pte)) {
        return SYS_ERR_INVAL;
    }

    uint64_t *slot = walk(vs, vaddr, true, user);
    if (slot == NULL) {
        return SYS_ERR_NOMEM;
    }

    uint64_t old_pte = *slot;
    bool old_owned = (old_pte & (PTE_PRESENT | PTE_OWNED)) ==
                     (PTE_PRESENT | PTE_OWNED);
    paddr_t old_phys = old_pte & PTE_ADDR_MASK;

    *replaced = (old_pte & PTE_PRESENT) != 0;

    /* A writable remap of the same frame is only safe when this vspace holds
     * the sole reference; CoW and read-only shared pages must be copied or
     * resolved first. */
    if (old_owned && old_phys == paddr && (pte & PTE_WRITABLE) != 0 &&
        pmm_frame_refcount(old_phys) > 1) {
        return SYS_ERR_BUSY;
    }

    bool transfer_ref = old_owned && old_phys == paddr &&
                        (pte & PTE_OWNED) != 0;

    *slot = pte;
    tlb_invalidate(vaddr);

    *release_phys = 0;
    if (old_owned && !transfer_ref) {
        *release_phys = old_phys;
    }

    return SYS_OK;
}

sys_status_t vspace_map_user(vspace_t *vs, vaddr_t vaddr, paddr_t paddr,
                             uint64_t flags)
{
    if (vs == NULL || (vaddr & PAGE_MASK) != 0 || (paddr & PAGE_MASK) != 0) {
        return SYS_ERR_INVAL;
    }
    if (vaddr > USER_MAX_ADDR) {
        return SYS_ERR_INVAL;
    }

    paddr_t release_phys = 0;
    bool replaced = false;
    uint64_t mask = 0;

    uint64_t lock = vspace_lock_acquire();
    sys_status_t status =
        map_locked(vs, vaddr, paddr, flags, true, &release_phys, &replaced);

    if (status == SYS_OK && replaced) {
        mask = mask_without_self(vs->active_cpus);
    }
    vspace_lock_release(lock);

    if (status != SYS_OK) {
        return status;
    }

    /* Peers that may have the previous translation cached must drop it before
     * the old frame can be recycled. */
    if (replaced) {
        tlb_shootdown(vaddr, mask);
    }
    if (release_phys != 0) {
        pmm_frame_unref(release_phys);
    }

    return SYS_OK;
}

sys_status_t vspace_map_kernel(vaddr_t vaddr, paddr_t paddr, uint64_t flags)
{
    if ((vaddr & PAGE_MASK) != 0 || (paddr & PAGE_MASK) != 0) {
        return SYS_ERR_INVAL;
    }
    if (vaddr < KERNEL_HALF_BASE) {
        return SYS_ERR_INVAL;
    }
    if ((flags & (VSPACE_PROT_USER | VSPACE_PROT_COW)) != 0) {
        return SYS_ERR_INVAL;
    }

    paddr_t release_phys = 0;
    bool replaced = false;

    uint64_t lock = vspace_lock_acquire();
    sys_status_t status =
        map_locked(&kernel_vspace, vaddr, paddr, flags, false, &release_phys,
                   &replaced);
    uint64_t mask = replaced ? mask_without_self(0xFFFFFFFFFFFFFFFFULL) : 0;
    vspace_lock_release(lock);

    if (status != SYS_OK) {
        return status;
    }

    if (replaced) {
        tlb_shootdown(vaddr, mask);
    }
    if (release_phys != 0) {
        pmm_frame_unref(release_phys);
    }

    return SYS_OK;
}

/* Low-half identity window used exclusively while an Application Processor is
 * executing its real-mode bootstrap page. Dedicated intermediate tables are
 * allocated so no kernel-half or HHDM table is ever shared or mutated, and the
 * entry that occupied PML4[0] beforehand is restored on teardown. */
static uint64_t identity_saved_pml4;
static paddr_t identity_pdpt_phys;
static paddr_t identity_pd_phys;
static paddr_t identity_pt_phys;
static bool identity_installed;

sys_status_t vspace_identity_map(paddr_t phys)
{
    /* The window is a single 4 KiB leaf inside PDPT[0]/PD[0], which covers the
     * first 2 MiB of the physical address space. */
    if ((phys & PAGE_MASK) != 0 || phys >= 0x200000ULL) {
        return SYS_ERR_INVAL;
    }

    uint64_t lock = vspace_lock_acquire();

    if (identity_installed) {
        vspace_lock_release(lock);
        return SYS_ERR_BUSY;
    }

    paddr_t pdpt_phys = pmm_alloc_frame();
    paddr_t pd_phys = pmm_alloc_frame();
    paddr_t pt_phys = pmm_alloc_frame();

    if (pdpt_phys == 0 || pd_phys == 0 || pt_phys == 0) {
        if (pdpt_phys != 0) {
            pmm_free_frame(pdpt_phys);
        }
        if (pd_phys != 0) {
            pmm_free_frame(pd_phys);
        }
        if (pt_phys != 0) {
            pmm_free_frame(pt_phys);
        }
        vspace_lock_release(lock);
        return SYS_ERR_NOMEM;
    }

    uint64_t *pdpt = (uint64_t *)pmm_phys_to_virt(pdpt_phys);
    uint64_t *pd = (uint64_t *)pmm_phys_to_virt(pd_phys);
    uint64_t *pt = (uint64_t *)pmm_phys_to_virt(pt_phys);

    /* Intermediate levels are writable; the leaf itself is read-only and
     * executable because the trampoline only fetches instructions and reads
     * its parameter block. */
    pdpt[0] = pd_phys | PTE_PRESENT | PTE_WRITABLE;
    pd[0] = pt_phys | PTE_PRESENT | PTE_WRITABLE;
    pt[PT_INDEX(phys)] = phys | PTE_PRESENT;

    identity_saved_pml4 = kernel_vspace.pml4[0];
    kernel_vspace.pml4[0] = pdpt_phys | PTE_PRESENT | PTE_WRITABLE;

    identity_pdpt_phys = pdpt_phys;
    identity_pd_phys = pd_phys;
    identity_pt_phys = pt_phys;
    identity_installed = true;

    tlb_invalidate(phys);
    vspace_lock_release(lock);

    return SYS_OK;
}

void vspace_identity_unmap(paddr_t phys)
{
    uint64_t lock = vspace_lock_acquire();

    if (!identity_installed ||
        (kernel_vspace.pml4[0] & PTE_ADDR_MASK) != identity_pdpt_phys) {
        vspace_lock_release(lock);
        return;
    }

    kernel_vspace.pml4[0] = identity_saved_pml4;
    tlb_invalidate(phys);

    paddr_t pdpt_phys = identity_pdpt_phys;
    paddr_t pd_phys = identity_pd_phys;
    paddr_t pt_phys = identity_pt_phys;

    identity_pdpt_phys = 0;
    identity_pd_phys = 0;
    identity_pt_phys = 0;
    identity_saved_pml4 = 0;
    identity_installed = false;

    vspace_lock_release(lock);

    /* No CPU may keep a cached low-half translation of a hierarchy that is
     * about to be recycled. The flush runs with no lock held. */
    tlb_shootdown_mask(0xFFFFFFFFFFFFFFFFULL, 0);

    pmm_free_frame(pt_phys);
    pmm_free_frame(pd_phys);
    pmm_free_frame(pdpt_phys);
}

sys_status_t vspace_unmap(vspace_t *vs, vaddr_t vaddr)
{
    if (vs == NULL || (vaddr & PAGE_MASK) != 0) {
        return SYS_ERR_INVAL;
    }
    if (vaddr > USER_MAX_ADDR) {
        return SYS_ERR_INVAL;
    }

    paddr_t release_phys = 0;
    uint64_t mask;

    uint64_t lock = vspace_lock_acquire();

    uint64_t *slot = walk(vs, vaddr, false, true);
    if (slot == NULL || (*slot & PTE_PRESENT) == 0) {
        vspace_lock_release(lock);
        return SYS_ERR_INVAL;
    }

    uint64_t old_pte = *slot;

    *slot = 0;
    tlb_invalidate(vaddr);

    if ((old_pte & PTE_OWNED) != 0) {
        release_phys = old_pte & PTE_ADDR_MASK;
    }
    mask = mask_without_self(vs->active_cpus);

    vspace_lock_release(lock);

    tlb_shootdown(vaddr, mask);
    if (release_phys != 0) {
        pmm_frame_unref(release_phys);
    }

    return SYS_OK;
}

sys_status_t vspace_unmap_kernel(vaddr_t vaddr)
{
    if ((vaddr & PAGE_MASK) != 0 || vaddr < KERNEL_HALF_BASE) {
        return SYS_ERR_INVAL;
    }

    paddr_t release_phys = 0;

    uint64_t lock = vspace_lock_acquire();

    uint64_t *slot = walk(&kernel_vspace, vaddr, false, false);
    if (slot == NULL || (*slot & PTE_PRESENT) == 0) {
        vspace_lock_release(lock);
        return SYS_ERR_INVAL;
    }

    uint64_t old_pte = *slot;

    *slot = 0;
    tlb_invalidate(vaddr);

    if ((old_pte & PTE_OWNED) != 0) {
        release_phys = old_pte & PTE_ADDR_MASK;
    }

    vspace_lock_release(lock);

    /* Kernel mappings are shared by every vspace, so every online CPU is a
     * potential user of the old translation. */
    tlb_shootdown(vaddr, mask_without_self(0xFFFFFFFFFFFFFFFFULL));

    if (release_phys != 0) {
        pmm_frame_unref(release_phys);
    }

    return SYS_OK;
}

void vspace_switch(vspace_t *vs)
{
    if (vs == NULL) {
        return;
    }

    __asm__ volatile("mov %0, %%cr3" : : "r"(vs->pml4_phys) : "memory");
}

/* Caller must hold the vspace lock. */
static void release_pd(uint64_t pd_entry)
{
    uint64_t *pd = (uint64_t *)pmm_phys_to_virt(pd_entry & PTE_ADDR_MASK);

    for (size_t i = 0; i < 512; i++) {
        uint64_t pt_entry = pd[i];

        if ((pt_entry & PTE_PRESENT) == 0 || (pt_entry & PTE_HUGE) != 0) {
            continue;
        }

        uint64_t *pt = (uint64_t *)pmm_phys_to_virt(pt_entry & PTE_ADDR_MASK);

        for (size_t j = 0; j < 512; j++) {
            uint64_t pte = pt[j];

            /* This is a 4 KiB leaf; bit 7 is PAT here, not PS. Never skip an
             * owned leaf merely because PAT is set. */
            if ((pte & PTE_PRESENT) != 0 && (pte & PTE_OWNED) != 0) {
                pmm_frame_unref(pte & PTE_ADDR_MASK);
            }
        }

        pmm_free_frame(pt_entry & PTE_ADDR_MASK);
    }

    pmm_free_frame(pd_entry & PTE_ADDR_MASK);
}

/* Caller must hold the vspace lock. */
static void release_pdpt(uint64_t pdpt_entry)
{
    uint64_t *pdpt = (uint64_t *)pmm_phys_to_virt(pdpt_entry & PTE_ADDR_MASK);

    for (size_t i = 0; i < 512; i++) {
        uint64_t pd_entry = pdpt[i];

        if ((pd_entry & PTE_PRESENT) == 0 || (pd_entry & PTE_HUGE) != 0) {
            continue;
        }

        release_pd(pd_entry);
    }

    pmm_free_frame(pdpt_entry & PTE_ADDR_MASK);
}

void vspace_destroy(vspace_t *vs)
{
    if (vs == NULL || vs == &kernel_vspace) {
        return;
    }

    uint64_t lock = vspace_lock_acquire();

    if (vs->ref_count > 1) {
        vs->ref_count--;
        vspace_lock_release(lock);
        return;
    }

    /* No CPU may still hold a cached translation of an address space whose
     * page tables are about to be recycled. Every CPU that ran in this space
     * must flush first. */
    uint64_t mask = mask_without_self(vs->active_cpus);
    uint64_t pml4_phys = vs->pml4_phys;

    vspace_lock_release(lock);

    if (mask != 0) {
        tlb_shootdown_mask(mask, 0);
    }

    lock = vspace_lock_acquire();

    for (size_t i = 0; i < KERNEL_PML4_FIRST; i++) {
        uint64_t pml4_entry = vs->pml4[i];

        if ((pml4_entry & PTE_PRESENT) == 0 || (pml4_entry & PTE_HUGE) != 0) {
            continue;
        }

        release_pdpt(pml4_entry);
    }

    pmm_free_frame(pml4_phys);
    vspace_lock_release(lock);

    kfree(vs);
}

/* The CPU updates A/D (and a leaf's bit 7 is PAT, not PS), so those bits must
 * be ignored when comparing a PTE snapshot with its current value. */
static bool pte_same_cow_mapping(uint64_t a, uint64_t b)
{
    uint64_t mask = ~(PTE_ACCESSED | PTE_DIRTY | PTE_HUGE);

    return (a & mask) == (b & mask);
}

static bool pte_is_present_user_writable(uint64_t pte)
{
    uint64_t want = PTE_PRESENT | PTE_USER | PTE_WRITABLE;

    return (pte & (want | PTE_COW)) == want;
}

/* Caller must hold the vspace lock. */
static bool cow_fault_locked(vspace_t *vs, vaddr_t vaddr)
{
    uint64_t *slot = walk(vs, vaddr, false, false);

    if (slot == NULL) {
        return false;
    }

    uint64_t pte = *slot;

    /* Another thread may already have resolved this exact CoW page while this
     * CPU still holds a read-only TLB entry. */
    if (pte_is_present_user_writable(pte)) {
        tlb_invalidate(vaddr);
        return true;
    }

    uint64_t required = PTE_PRESENT | PTE_USER | PTE_COW | PTE_OWNED;

    if ((pte & required) != required) {
        return false;
    }

    paddr_t old_phys = pte & PTE_ADDR_MASK;
    uint16_t refs = pmm_frame_refcount(old_phys);

    if (refs == 0) {
        return false;
    }

    if (refs == 1) {
        uint64_t current = *slot;

        if (!pte_same_cow_mapping(current, pte)) {
            bool resolved = pte_is_present_user_writable(current);

            if (resolved) {
                tlb_invalidate(vaddr);
            }
            return resolved;
        }

        *slot = (pte & ~PTE_COW) | PTE_WRITABLE | PTE_NX;
        tlb_invalidate(vaddr);
        return true;
    }

    paddr_t new_phys = pmm_alloc_frame();
    if (new_phys == 0) {
        return false;
    }

    uint64_t current = *slot;

    if (!pte_same_cow_mapping(current, pte)) {
        bool resolved = pte_is_present_user_writable(current);

        pmm_frame_unref(new_phys);
        if (resolved) {
            tlb_invalidate(vaddr);
        }
        return resolved;
    }

    memcpy(pmm_phys_to_virt(new_phys), pmm_phys_to_virt(old_phys), PAGE_SIZE);

    *slot = (pte & ~(PTE_ADDR_MASK | PTE_COW)) | new_phys | PTE_WRITABLE |
            PTE_NX;
    tlb_invalidate(vaddr);

    /* The old frame keeps the other sharer's reference. */
    pmm_frame_unref(old_phys);
    return true;
}

bool vspace_handle_cow_fault(vspace_t *vs, vaddr_t fault_addr)
{
    if (vs == NULL) {
        return false;
    }

    vaddr_t vaddr = (vaddr_t)PAGE_ALIGN_DOWN(fault_addr);
    if (vaddr < PAGE_SIZE || vaddr > USER_MAX_ADDR) {
        return false;
    }

    /* Runs in fault context. Only this vspace's own threads can observe the
     * promoted mapping, and a stale read-only entry merely refaults into the
     * already-resolved path above, so no remote shootdown is required and no
     * acknowledgement wait is ever performed here. */
    uint64_t lock = vspace_lock_acquire();
    bool resolved = cow_fault_locked(vs, vaddr);
    vspace_lock_release(lock);

    return resolved;
}

#define KSTACK_FREE_SLOTS 1024U

static vaddr_t kstack_next_slot = KSTACK_BASE;
static vaddr_t kstack_free_slots[KSTACK_FREE_SLOTS];
static size_t kstack_free_count;
static spinlock_t kstack_lock;

static vaddr_t kstack_page(vaddr_t slot_base, size_t page)
{
    return slot_base +
           (vaddr_t)(page + KSTACK_GUARD_PAGES) * (vaddr_t)PAGE_SIZE;
}

vaddr_t kstack_alloc(void)
{
    vaddr_t slot;
    uint64_t flags = spin_lock_irqsave(&kstack_lock);

    if (kstack_free_count > 0) {
        slot = kstack_free_slots[--kstack_free_count];
    } else {
        slot = kstack_next_slot;

        if (slot < KSTACK_BASE || slot >= KSTACK_LIMIT ||
            (uint64_t)KSTACK_SLOT_SIZE > (KSTACK_LIMIT - slot)) {
            spin_unlock_irqrestore(&kstack_lock, flags);
            return 0;
        }

        kstack_next_slot = slot + KSTACK_SLOT_SIZE;
    }

    spin_unlock_irqrestore(&kstack_lock, flags);

    for (size_t page = 0; page < KSTACK_PAGE_COUNT; page++) {
        paddr_t frame = pmm_alloc_frame();

        if (frame == 0) {
            goto fail;
        }

        if (vspace_map_kernel(kstack_page(slot, page), frame,
                              VSPACE_PROT_WRITE | VSPACE_PROT_OWNED) !=
            SYS_OK) {
            pmm_frame_unref(frame);
            goto fail;
        }
    }

    return slot;

fail:
    for (size_t page = 0; page < KSTACK_PAGE_COUNT; page++) {
        (void)vspace_unmap_kernel(kstack_page(slot, page));
    }

    flags = spin_lock_irqsave(&kstack_lock);
    if (kstack_free_count < KSTACK_FREE_SLOTS) {
        kstack_free_slots[kstack_free_count++] = slot;
    }
    spin_unlock_irqrestore(&kstack_lock, flags);
    return 0;
}

void kstack_free(vaddr_t slot_base)
{
    if ((slot_base & PAGE_MASK) != 0 || slot_base < KSTACK_BASE ||
        slot_base >= KSTACK_LIMIT ||
        (uint64_t)KSTACK_SLOT_SIZE > (KSTACK_LIMIT - slot_base)) {
        return;
    }

    /* vspace_unmap_kernel() invalidates the TLB entry and only releases the
     * frame after every peer has flushed it, so a reused slot can never
     * alias a stale translation. */
    for (size_t page = 0; page < KSTACK_PAGE_COUNT; page++) {
        (void)vspace_unmap_kernel(kstack_page(slot_base, page));
    }

    uint64_t flags = spin_lock_irqsave(&kstack_lock);
    if (kstack_free_count < KSTACK_FREE_SLOTS) {
        kstack_free_slots[kstack_free_count++] = slot_base;
    }
    spin_unlock_irqrestore(&kstack_lock, flags);
}

bool vspace_probe_flags(vaddr_t vaddr, uint64_t *flags_out)
{
    uint64_t lock = vspace_lock_acquire();
    uint64_t *slot = walk(&kernel_vspace, vaddr, false, false);

    if (slot == NULL || (*slot & PTE_PRESENT) == 0) {
        vspace_lock_release(lock);
        return false;
    }

    if (flags_out != NULL) {
        *flags_out = *slot;
    }
    vspace_lock_release(lock);
    return true;
}

bool vspace_translate(vspace_t *vs, vaddr_t vaddr, paddr_t *paddr_out)
{
    if (vs == NULL || (vaddr & PAGE_MASK) != 0) {
        return false;
    }

    uint64_t lock = vspace_lock_acquire();
    uint64_t *slot = walk(vs, vaddr, false, false);

    if (slot == NULL || (*slot & PTE_PRESENT) == 0) {
        vspace_lock_release(lock);
        return false;
    }

    if (paddr_out != NULL) {
        *paddr_out = *slot & PTE_ADDR_MASK;
    }

    vspace_lock_release(lock);
    return true;
}

static sys_status_t copy_user_range(void *kernel_buf, uintptr_t user_addr,
                                    size_t len, bool to_user)
{
    if (len == 0) {
        return SYS_OK;
    }
    if (user_addr < 0x1000 || user_addr > USER_MAX_ADDR) {
        return SYS_ERR_INVAL;
    }
    if (len > (size_t)((USER_MAX_ADDR - user_addr) + 1)) {
        return SYS_ERR_INVAL;
    }

    thread_t *current = sched_current_thread();
    if (current == NULL || current->owner_task == NULL ||
        current->owner_task->vspace == NULL) {
        return SYS_ERR_INVAL;
    }

    vspace_t *vs = current->owner_task->vspace;
    uint8_t *kernel = (uint8_t *)kernel_buf;
    uint64_t addr = user_addr;
    size_t remaining = len;

    uint64_t lock = vspace_lock_acquire();

    while (remaining > 0) {
        uint64_t *slot = walk(vs, addr, false, false);

        if (slot == NULL || (*slot & PTE_PRESENT) == 0 ||
            (*slot & PTE_USER) == 0) {
            vspace_lock_release(lock);
            return SYS_ERR_INVAL;
        }

        /* Device memory is not reachable through the HHDM and must never be
         * touched by the kernel on a user's behalf. */
        if ((*slot & PTE_DEVICE) != 0) {
            vspace_lock_release(lock);
            return SYS_ERR_INVAL;
        }

        /* A kernel write to a CoW page must break the sharing exactly like a
         * user write fault would; the PTE is replaced, so re-walk it. */
        if (to_user && ((*slot & PTE_WRITABLE) == 0 ||
                        (*slot & PTE_COW) != 0)) {
            if ((*slot & PTE_COW) == 0 ||
                !cow_fault_locked(vs, (vaddr_t)PAGE_ALIGN_DOWN(addr))) {
                vspace_lock_release(lock);
                return SYS_ERR_INVAL;
            }

            slot = walk(vs, addr, false, false);
            if (slot == NULL || (*slot & PTE_PRESENT) == 0 ||
                (*slot & PTE_USER) == 0 ||
                (*slot & PTE_WRITABLE) == 0 ||
                (*slot & PTE_COW) != 0) {
                vspace_lock_release(lock);
                return SYS_ERR_INVAL;
            }
        }

        paddr_t phys = *slot & PTE_ADDR_MASK;
        size_t page_offset = (size_t)(addr & PAGE_MASK);
        size_t chunk = (size_t)PAGE_SIZE - page_offset;

        if (chunk > remaining) {
            chunk = remaining;
        }

        uint8_t *page = (uint8_t *)pmm_phys_to_virt(phys);

        if (to_user) {
            memcpy(page + page_offset, kernel, chunk);
        } else {
            memcpy(kernel, page + page_offset, chunk);
        }

        kernel += chunk;
        addr += chunk;
        remaining -= chunk;
    }

    vspace_lock_release(lock);
    return SYS_OK;
}

sys_status_t copy_from_user(void *kdst, const void *usrc, size_t len)
{
    if (kdst == NULL && len != 0) {
        return SYS_ERR_INVAL;
    }

    return copy_user_range(kdst, (uintptr_t)usrc, len, false);
}

sys_status_t copy_to_user(void *udst, const void *ksrc, size_t len)
{
    if (ksrc == NULL && len != 0) {
        return SYS_ERR_INVAL;
    }

    return copy_user_range((void *)(uintptr_t)ksrc, (uintptr_t)udst, len, true);
}

sys_status_t vspace_remap_user(vspace_t *vs, vaddr_t vaddr, uint64_t new_flags,
                               paddr_t *old_phys_out, uint64_t *old_pte_out)
{
    if (vs == NULL || (vaddr & PAGE_MASK) != 0 || vaddr > USER_MAX_ADDR) {
        return SYS_ERR_INVAL;
    }

    /* Reject W^X. */
    if ((new_flags & VSPACE_PROT_WRITE) != 0 &&
        (new_flags & VSPACE_PROT_EXEC) != 0) {
        return SYS_ERR_INVAL;
    }

    uint64_t lock = vspace_lock_acquire();

    uint64_t *slot = walk(vs, vaddr, false, false);
    if (slot == NULL || (*slot & PTE_PRESENT) == 0 ||
        (*slot & PTE_USER) == 0) {
        vspace_lock_release(lock);
        return SYS_ERR_INVAL;
    }

    uint64_t old_pte = *slot;
    paddr_t frame = old_pte & PTE_ADDR_MASK;

    /* Device mappings keep the protection they were granted with. */
    if ((old_pte & PTE_DEVICE) != 0) {
        vspace_lock_release(lock);
        return SYS_ERR_INVAL;
    }

    if (old_pte_out != NULL) {
        *old_pte_out = old_pte;
    }
    if (old_phys_out != NULL) {
        *old_phys_out = frame;
    }

    /* If requesting WRITE on a COW page, resolve the CoW first. */
    if ((new_flags & VSPACE_PROT_WRITE) != 0 &&
        (old_pte & PTE_COW) != 0 && (old_pte & PTE_OWNED) != 0) {
        if (!cow_fault_locked(vs, vaddr)) {
            vspace_lock_release(lock);
            return SYS_ERR_BUSY;
        }
        /* Re-read: cow_fault_locked may have replaced the PTE. */
        slot = walk(vs, vaddr, false, false);
        if (slot == NULL || (*slot & PTE_PRESENT) == 0) {
            vspace_lock_release(lock);
            return SYS_ERR_INVAL;
        }
        old_pte = *slot;
        frame = old_pte & PTE_ADDR_MASK;
    }

    /* Build the new PTE preserving PRESENT, USER, OWNED, address. */
    uint64_t pte = frame | PTE_PRESENT | PTE_USER;
    if ((old_pte & PTE_OWNED) != 0) {
        pte |= PTE_OWNED;
    }

    /* Preserve CoW semantics: if the page is still shared (COW) and the
     * caller is not requesting WRITE, keep the COW marker so a future write
     * fault still triggers copy resolution. WRITE remaps already resolved
     * COW above, so PTE_COW is never combined with PTE_WRITABLE. */
    if ((old_pte & PTE_COW) != 0 && (new_flags & VSPACE_PROT_WRITE) == 0) {
        pte |= PTE_COW;
    }

    if ((new_flags & VSPACE_PROT_WRITE) != 0) {
        pte |= PTE_WRITABLE | PTE_NX;
    } else if ((new_flags & VSPACE_PROT_EXEC) != 0) {
        /* Executable, non-writable: W^X is enforced above. */
    } else {
        /* Read-only data. */
        pte |= PTE_NX;
    }

    *slot = pte;
    tlb_invalidate(vaddr);

    vspace_lock_release(lock);
    return SYS_OK;
}

static uint32_t leaf_range_prot(uint64_t pte)
{
    uint32_t prot = 0;

    if ((pte & (PTE_WRITABLE | PTE_COW)) != 0) {
        prot |= VSPACE_PROT_WRITE;
    }
    if ((pte & PTE_NX) == 0) {
        prot |= VSPACE_PROT_EXEC;
    }
    if ((pte & PTE_DEVICE) != 0) {
        prot |= VSPACE_PROT_DEVICE;
    }
    return prot;
}

void vspace_user_ranges(vspace_t *vs,
                        bool (*fn)(const vspace_range_t *range, void *ctx),
                        void *ctx)
{
    if (vs == NULL || fn == NULL || vs == &kernel_vspace) {
        return;
    }

    vspace_range_t run = { 0, 0, 0 };
    bool open_run = false;
    bool keep_going = true;
    uint64_t lock = vspace_lock_acquire();

    for (uint64_t pml4_index = 0;
         keep_going && pml4_index < KERNEL_PML4_FIRST; pml4_index++) {
        uint64_t pml4_entry = vs->pml4[pml4_index];

        if ((pml4_entry & PTE_PRESENT) == 0 || (pml4_entry & PTE_HUGE) != 0) {
            continue;
        }

        uint64_t *pdpt =
            (uint64_t *)pmm_phys_to_virt(pml4_entry & PTE_ADDR_MASK);

        for (uint64_t pdpt_index = 0; keep_going && pdpt_index < 512;
             pdpt_index++) {
            uint64_t pdpt_entry = pdpt[pdpt_index];

            if ((pdpt_entry & PTE_PRESENT) == 0 ||
                (pdpt_entry & PTE_HUGE) != 0) {
                continue;
            }

            uint64_t *pd =
                (uint64_t *)pmm_phys_to_virt(pdpt_entry & PTE_ADDR_MASK);

            for (uint64_t pd_index = 0; keep_going && pd_index < 512;
                 pd_index++) {
                uint64_t pd_entry = pd[pd_index];

                if ((pd_entry & PTE_PRESENT) == 0 ||
                    (pd_entry & PTE_HUGE) != 0) {
                    continue;
                }

                uint64_t *pt =
                    (uint64_t *)pmm_phys_to_virt(pd_entry & PTE_ADDR_MASK);

                for (uint64_t pt_index = 0; keep_going && pt_index < 512;
                     pt_index++) {
                    uint64_t pte = pt[pt_index];

                    if ((pte & PTE_PRESENT) == 0 || (pte & PTE_USER) == 0) {
                        continue;
                    }

                    vaddr_t vaddr = (pml4_index << 39) | (pdpt_index << 30) |
                                    (pd_index << 21) | (pt_index << 12);
                    uint32_t prot = leaf_range_prot(pte);

                    if (open_run && run.end == vaddr && run.prot == prot) {
                        run.end = vaddr + PAGE_SIZE;
                        continue;
                    }
                    if (open_run) {
                        keep_going = fn(&run, ctx);
                    }
                    run.start = vaddr;
                    run.end = vaddr + PAGE_SIZE;
                    run.prot = prot;
                    open_run = true;
                }
            }
        }
    }

    if (keep_going && open_run) {
        (void)fn(&run, ctx);
    }

    vspace_lock_release(lock);
}
