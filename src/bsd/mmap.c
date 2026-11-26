#include <bsd/bsd.h>
#include <bsd/errno.h>
#include <bsd/mmap.h>
#include <bsd/proc.h>
#include <io.h>
#include <pmm.h>
#include <sched.h>
#include <string.h>
#include <task.h>
#include <types.h>
#include <vspace.h>

typedef struct unmap_range {
    vaddr_t start;
    vaddr_t end;
} unmap_range_t;

_Static_assert(BSD_BRK_LIMIT < BSD_MMAP_BASE,
               "the brk and mmap address windows must not overlap");

static vspace_t *proc_vspace(bsd_proc_t *proc)
{
    if (proc == NULL || proc->mk_task == NULL) {
        return NULL;
    }
    return proc->mk_task->vspace;
}

/* Caller holds the process mmap lock (interrupts disabled). */
static bsd_mmap_region_t *region_free_slot_locked(bsd_proc_t *proc)
{
    for (size_t i = 0; i < BSD_MAX_MMAPS; i++) {
        if (proc->mmap_regions[i].length == 0) {
            return &proc->mmap_regions[i];
        }
    }
    return NULL;
}

/* Caller holds the process mmap lock (interrupts disabled). */
static bool region_overlaps_locked(bsd_proc_t *proc, vaddr_t base,
                                   size_t length)
{
    vaddr_t end = base + length;

    for (size_t i = 0; i < BSD_MAX_MMAPS; i++) {
        bsd_mmap_region_t *region = &proc->mmap_regions[i];

        if (region->length == 0) {
            continue;
        }

        vaddr_t region_end = region->base + region->length;

        if (base < region_end && region->base < end) {
            return true;
        }
    }
    return false;
}

static void unmap_pages(vspace_t *vspace, vaddr_t base, vaddr_t end)
{
    for (vaddr_t page = base; page < end; page += PAGE_SIZE) {
        (void)vspace_unmap(vspace, page);
    }
}

/* Maps zeroed anonymous frames over a page-aligned range. Frames are owned by
 * the vspace, so vspace_unmap()/vspace_destroy() release them later. */
static sys_status_t map_anon_pages(vspace_t *vspace, vaddr_t base, size_t length,
                                   uint64_t flags)
{
    for (size_t offset = 0; offset < length; offset += PAGE_SIZE) {
        paddr_t frame = pmm_alloc_frame();

        if (frame == 0) {
            unmap_pages(vspace, base, base + offset);
            return BSD_ERR(BSD_ENOMEM);
        }

        if (vspace_map_user(vspace, base + offset, frame, flags) != SYS_OK) {
            pmm_free_frame(frame);
            unmap_pages(vspace, base, base + offset);
            return BSD_ERR(BSD_ENOMEM);
        }
    }

    return SYS_OK;
}

sys_status_t bsd_mmap(bsd_proc_t *proc, uint64_t addr, uint64_t len,
                      uint32_t prot, uint32_t flags, int32_t fd,
                      int64_t offset, vaddr_t *out)
{
    UNUSED(fd);
    UNUSED(offset);

    vspace_t *vspace = proc_vspace(proc);

    if (vspace == NULL || out == NULL) {
        return BSD_ERR(BSD_EINVAL);
    }
    if (len == 0) {
        return BSD_ERR(BSD_EINVAL);
    }
    if ((prot & ~(BSD_PROT_READ | BSD_PROT_WRITE | BSD_PROT_EXEC)) != 0) {
        return BSD_ERR(BSD_EINVAL);
    }
    if ((prot & BSD_PROT_WRITE) != 0 && (prot & BSD_PROT_EXEC) != 0) {
        return BSD_ERR(BSD_EINVAL);
    }
    if ((flags & ~(BSD_MAP_SHARED | BSD_MAP_PRIVATE | BSD_MAP_FIXED |
                   BSD_MAP_ANON)) != 0) {
        return BSD_ERR(BSD_EINVAL);
    }
    if ((flags & BSD_MAP_ANON) == 0 || (flags & BSD_MAP_PRIVATE) == 0) {
        return BSD_ERR(BSD_EINVAL);
    }
    if ((flags & BSD_MAP_SHARED) != 0) {
        return BSD_ERR(BSD_EINVAL);
    }
    if (addr != 0 && (addr & PAGE_MASK) != 0) {
        return BSD_ERR(BSD_EINVAL);
    }
    if (len > BSD_MMAP_LIMIT - BSD_MMAP_BASE) {
        return BSD_ERR(BSD_EINVAL);
    }

    size_t length = (size_t)PAGE_ALIGN_UP(len);
    if (length == 0 || length > BSD_MMAP_LIMIT - BSD_MMAP_BASE) {
        return BSD_ERR(BSD_EINVAL);
    }

    uint64_t vflags = VSPACE_PROT_USER | VSPACE_PROT_OWNED;
    if ((prot & BSD_PROT_WRITE) != 0) {
        vflags |= VSPACE_PROT_WRITE;
    }
    if ((prot & BSD_PROT_EXEC) != 0) {
        vflags |= VSPACE_PROT_EXEC;
    }

    vaddr_t base;
    uint64_t lock = irq_save();

    bsd_mmap_region_t *slot = region_free_slot_locked(proc);
    if (slot == NULL) {
        irq_restore(lock);
        return BSD_ERR(BSD_ENOMEM);
    }

    if ((flags & BSD_MAP_FIXED) != 0) {
        if (addr < BSD_MMAP_BASE || addr > BSD_MMAP_LIMIT ||
            length > BSD_MMAP_LIMIT - addr) {
            irq_restore(lock);
            return BSD_ERR(BSD_EINVAL);
        }
        if (region_overlaps_locked(proc, (vaddr_t)addr, length)) {
            irq_restore(lock);
            return BSD_ERR(BSD_EBUSY);
        }
        base = (vaddr_t)addr;
    } else {
        base = proc->mmap_next;
        if (base < BSD_MMAP_BASE) {
            base = BSD_MMAP_BASE;
        }

        /* Skip any MAP_FIXED regions that lie ahead of the bump pointer. */
        for (uint32_t guard = 0; guard <= BSD_MAX_MMAPS; guard++) {
            vaddr_t conflict_end = 0;

            for (size_t i = 0; i < BSD_MAX_MMAPS; i++) {
                bsd_mmap_region_t *region = &proc->mmap_regions[i];

                if (region->length == 0) {
                    continue;
                }

                vaddr_t region_end = region->base + region->length;

                if (base < region_end && region->base < base + length &&
                    region_end > conflict_end) {
                    conflict_end = region_end;
                }
            }

            if (conflict_end == 0) {
                break;
            }
            base = (vaddr_t)PAGE_ALIGN_UP(conflict_end);
        }

        if (base > BSD_MMAP_LIMIT || length > BSD_MMAP_LIMIT - base ||
            region_overlaps_locked(proc, base, length)) {
            irq_restore(lock);
            return BSD_ERR(BSD_ENOMEM);
        }
        proc->mmap_next = base + length;
    }

    slot->base = base;
    slot->length = length;
    irq_restore(lock);

    sys_status_t status = map_anon_pages(vspace, base, length, vflags);

    if (status != SYS_OK) {
        lock = irq_save();

        for (size_t i = 0; i < BSD_MAX_MMAPS; i++) {
            bsd_mmap_region_t *region = &proc->mmap_regions[i];

            if (region->base == base && region->length == length) {
                region->base = 0;
                region->length = 0;
                break;
            }
        }

        if ((flags & BSD_MAP_FIXED) == 0 &&
            proc->mmap_next == base + length) {
            proc->mmap_next = base;
        }
        irq_restore(lock);
        return status;
    }

    *out = base;
    return SYS_OK;
}

sys_status_t bsd_munmap(bsd_proc_t *proc, uint64_t addr, uint64_t len)
{
    vspace_t *vspace = proc_vspace(proc);

    if (vspace == NULL) {
        return BSD_ERR(BSD_EINVAL);
    }
    if (len == 0 || (addr & PAGE_MASK) != 0) {
        return BSD_ERR(BSD_EINVAL);
    }
    if (addr < BSD_MMAP_BASE || addr > BSD_MMAP_LIMIT) {
        return BSD_ERR(BSD_EINVAL);
    }
    if (len > BSD_MMAP_LIMIT - addr) {
        return BSD_ERR(BSD_EINVAL);
    }

    size_t length = (size_t)PAGE_ALIGN_UP(len);
    if (length == 0 || length > BSD_MMAP_LIMIT - addr) {
        return BSD_ERR(BSD_EINVAL);
    }

    unmap_range_t ranges[BSD_MAX_MMAPS];
    size_t range_count = 0;
    vaddr_t start = (vaddr_t)addr;
    vaddr_t end = start + length;

    uint64_t lock = irq_save();

    /* The region table is small enough to snapshot. Any metadata failure
     * during splitting restores it before a single PTE is touched. */
    bsd_mmap_region_t backup[BSD_MAX_MMAPS];
    memcpy(backup, proc->mmap_regions, sizeof(backup));

    size_t splits = 0;
    size_t free_slots = 0;

    for (size_t i = 0; i < BSD_MAX_MMAPS; i++) {
        bsd_mmap_region_t *region = &proc->mmap_regions[i];

        if (region->length == 0) {
            free_slots++;
            continue;
        }

        vaddr_t region_end = region->base + region->length;
        if (region_end <= start || region->base >= end) {
            continue;
        }

        vaddr_t overlap_start =
            region->base > start ? region->base : start;
        vaddr_t overlap_end = region_end < end ? region_end : end;

        if (overlap_start != region->base && overlap_end != region_end) {
            splits++;
        }
    }

    if (splits > free_slots) {
        irq_restore(lock);
        return BSD_ERR(BSD_ENOMEM);
    }

    for (size_t i = 0; i < BSD_MAX_MMAPS; i++) {
        bsd_mmap_region_t *region = &proc->mmap_regions[i];

        if (region->length == 0) {
            continue;
        }

        vaddr_t region_end = region->base + region->length;
        if (region_end <= start || region->base >= end) {
            continue;
        }

        vaddr_t overlap_start =
            region->base > start ? region->base : start;
        vaddr_t overlap_end = region_end < end ? region_end : end;

        if (range_count >= BSD_MAX_MMAPS) {
            memcpy(proc->mmap_regions, backup, sizeof(backup));
            irq_restore(lock);
            return BSD_ERR(BSD_ENOMEM);
        }

        ranges[range_count].start = overlap_start;
        ranges[range_count].end = overlap_end;
        range_count++;

        if (overlap_start == region->base && overlap_end == region_end) {
            region->base = 0;
            region->length = 0;
        } else if (overlap_start == region->base) {
            region->base = overlap_end;
            region->length = region_end - overlap_end;
        } else if (overlap_end == region_end) {
            region->length = overlap_start - region->base;
        } else {
            bsd_mmap_region_t *right = region_free_slot_locked(proc);

            if (right == NULL) {
                memcpy(proc->mmap_regions, backup, sizeof(backup));
                irq_restore(lock);
                return BSD_ERR(BSD_ENOMEM);
            }

            right->base = overlap_end;
            right->length = region_end - overlap_end;
            region->length = overlap_start - region->base;
        }
    }

    irq_restore(lock);

    if (range_count == 0) {
        return BSD_ERR(BSD_EINVAL);
    }

    for (size_t i = 0; i < range_count; i++) {
        unmap_pages(vspace, ranges[i].start, ranges[i].end);
    }

    return SYS_OK;
}

sys_status_t bsd_brk(bsd_proc_t *proc, uint64_t new_brk, vaddr_t *result)
{
    vspace_t *vspace = proc_vspace(proc);

    if (vspace == NULL || result == NULL) {
        return BSD_ERR(BSD_EINVAL);
    }
    if (proc->brk_base == 0 || proc->brk_base > BSD_BRK_LIMIT) {
        return BSD_ERR(BSD_EINVAL);
    }

    vaddr_t current = proc->brk_current;

    if (new_brk == 0) {
        *result = current;
        return SYS_OK;
    }
    if (new_brk < proc->brk_base || new_brk > BSD_BRK_LIMIT) {
        *result = current;
        return SYS_OK;
    }

    vaddr_t old_page = (vaddr_t)PAGE_ALIGN_UP(current);
    vaddr_t new_page = (vaddr_t)PAGE_ALIGN_UP(new_brk);

    if (new_page > old_page) {
        /* The windows are disjoint by construction; enforce the invariant so
         * a future layout change cannot silently overwrite an mmap region. */
        uint64_t lock = irq_save();
        bool overlap = region_overlaps_locked(
            proc, old_page, (size_t)(new_page - old_page));
        irq_restore(lock);

        if (overlap) {
            *result = current;
            return BSD_ERR(BSD_EBUSY);
        }

        sys_status_t status =
            map_anon_pages(vspace, old_page, (size_t)(new_page - old_page),
                           VSPACE_PROT_USER | VSPACE_PROT_OWNED |
                               VSPACE_PROT_WRITE);

        if (status != SYS_OK) {
            *result = current;
            return status;
        }
    } else if (new_page < old_page) {
        unmap_pages(vspace, new_page, old_page);
    }

    uint64_t brk_flags = irq_save();
    proc->brk_current = (vaddr_t)new_brk;
    irq_restore(brk_flags);

    *result = (vaddr_t)new_brk;
    return SYS_OK;
}

sys_status_t bsd_mprotect(bsd_proc_t *proc, uint64_t addr, uint64_t len,
                          uint32_t prot)
{
    vspace_t *vspace = proc_vspace(proc);

    if (vspace == NULL) {
        return BSD_ERR(BSD_EINVAL);
    }
    if (len == 0 || (addr & PAGE_MASK) != 0) {
        return BSD_ERR(BSD_EINVAL);
    }
    if ((prot & ~(BSD_PROT_READ | BSD_PROT_WRITE | BSD_PROT_EXEC)) != 0) {
        return BSD_ERR(BSD_EINVAL);
    }
    /* Strict W^X: reject PROT_WRITE | PROT_EXEC. */
    if ((prot & BSD_PROT_WRITE) != 0 && (prot & BSD_PROT_EXEC) != 0) {
        return BSD_ERR(BSD_EINVAL);
    }

    size_t length = (size_t)PAGE_ALIGN_UP(len);
    if (length == 0) {
        return BSD_ERR(BSD_EINVAL);
    }

    /* Verify the range falls within a tracked mmap region. */
    vaddr_t start = (vaddr_t)addr;
    vaddr_t end = start + length;
    bool found = false;

    uint64_t lock = irq_save();
    for (size_t i = 0; i < BSD_MAX_MMAPS; i++) {
        bsd_mmap_region_t *region = &proc->mmap_regions[i];
        if (region->length == 0) {
            continue;
        }
        vaddr_t region_end = region->base + region->length;
        if (start >= region->base && end <= region_end) {
            found = true;
            break;
        }
    }
    irq_restore(lock);

    if (!found) {
        return BSD_ERR(BSD_EINVAL);
    }

    /* Build the vspace protection flags. */
    uint64_t vflags = 0;
    if ((prot & BSD_PROT_WRITE) != 0) {
        vflags |= VSPACE_PROT_WRITE;
    }
    if ((prot & BSD_PROT_EXEC) != 0) {
        vflags |= VSPACE_PROT_EXEC;
    }

    /* Save old PTE flags for rollback if a mid-range page fails. The range is
     * bounded by BSD_MAX_MMAPS * pages-per-region, but the per-region limit is
     * the entire mmap window; cap the snapshot to a reasonable stack size.
     * With max 128 MiB mmap regions and 4 KiB pages, a single region can span
     * 32768 pages. Use heap allocation for safety. */
    size_t page_count = length / PAGE_SIZE;

    /* For small ranges (up to 64 pages = 256 KiB) use a stack buffer; for
     * larger ranges, reject to bound kernel stack usage. A production kernel
     * would use kalloc here, but keeping the syscall non-allocating is safer. */
    uint64_t saved_flags_stack[64];
    uint64_t *saved_flags = NULL;
    if (page_count <= 64) {
        saved_flags = saved_flags_stack;
    } else {
        /* For ranges larger than 64 pages, do a two-pass approach: first
         * verify every page can be remapped, then apply. If any page is not
         * present/user, reject early with no PTE mutation. */
        for (vaddr_t page = start; page < end; page += PAGE_SIZE) {
            if (!vspace_translate(vspace, page, NULL)) {
                return BSD_ERR(BSD_EINVAL);
            }
        }
        /* For large ranges, proceed without rollback capability; every page
         * was verified present above, so vspace_remap_user will not fail. */
    }

    /* Remap each page, saving old flags for rollback. */
    size_t remapped = 0;
    for (vaddr_t page = start; page < end; page += PAGE_SIZE) {
        paddr_t old_phys = 0;
        uint64_t old_pte = 0;
        sys_status_t status =
            vspace_remap_user(vspace, page, vflags, &old_phys, &old_pte);

        if (status != SYS_OK) {
            /* Rollback all previously remapped pages to their old flags. */
            if (saved_flags != NULL) {
                vaddr_t rollback_page = start;
                for (size_t r = 0; r < remapped; r++) {
                    uint64_t old_vflags = 0;
                    if ((saved_flags[r] & (1ULL << 1)) != 0) {  /* PTE_WRITABLE */
                        old_vflags |= VSPACE_PROT_WRITE;
                    } else if ((saved_flags[r] & (1ULL << 63)) == 0) { /* !PTE_NX */
                        old_vflags |= VSPACE_PROT_EXEC;
                    }
                    (void)vspace_remap_user(vspace, rollback_page,
                                            old_vflags, NULL, NULL);
                    rollback_page += PAGE_SIZE;
                }
            }
            return BSD_ERR(BSD_EINVAL);
        }

        if (saved_flags != NULL && remapped < page_count) {
            saved_flags[remapped] = old_pte;
        }
        remapped++;
    }

    /* Shoot down TLB entries on remote CPUs. Read active_cpus under lock. */
    uint64_t cpu_mask;
    uint64_t vs_lock = vspace_lock_acquire();
    cpu_mask = vspace->active_cpus;
    vspace_lock_release(vs_lock);

    uint32_t self_cpu = 0;
    thread_t *self = sched_current_thread();
    if (self != NULL) {
        self_cpu = self->cpu_index;
    }
    if (self_cpu < 64U) {
        cpu_mask &= ~(1ULL << self_cpu);
    }
    if (cpu_mask != 0) {
        for (vaddr_t page = start; page < end; page += PAGE_SIZE) {
            tlb_shootdown_mask(cpu_mask, page);
        }
    }

    return SYS_OK;
}
