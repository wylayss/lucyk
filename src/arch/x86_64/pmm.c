#include <io.h>
#include <klog.h>
#include <limine.h>
#include <pmm.h>
#include <spinlock.h>
#include <string.h>
#include <types.h>

/* Phase 2 manages the first 4 GiB of physical memory. */
#define PMM_MAX_PHYS   0x100000000ULL
#define PMM_MAX_FRAMES (PMM_MAX_PHYS / PAGE_SIZE)

#define PMM_AP_TRAMPOLINE_PHYS 0x8000ULL

/* 1 bit per 4 KiB frame; 1 = used, 0 = free. */
static uint8_t frame_bitmap[PMM_MAX_FRAMES / 8];

/* Reference count per allocated frame. A frame is returned to the bitmap only
 * when this drops from 1 to 0, so shared pages can outlive any one vspace. */
static uint16_t frame_refcount[PMM_MAX_FRAMES];

/* Leaf-most lock in the hierarchy: no other subsystem lock may be acquired
 * while it is held. */
static spinlock_t pmm_lock;

static uint64_t hhdm_base;
static size_t   total_frames;
static size_t   free_frames;
static size_t   search_hint;

static bool frame_is_used(size_t index)
{
    return (frame_bitmap[index >> 3] & (uint8_t)(1U << (index & 7U))) != 0;
}

static void frame_set_used(size_t index, bool used)
{
    uint8_t mask = (uint8_t)(1U << (index & 7U));

    if (used) {
        frame_bitmap[index >> 3] |= mask;
    } else {
        frame_bitmap[index >> 3] &= (uint8_t)~mask;
    }
}

void *pmm_phys_to_virt(uint64_t phys)
{
    return (void *)(uintptr_t)(phys + hhdm_base);
}

uint64_t pmm_virt_to_phys(const void *virt)
{
    return (uint64_t)(uintptr_t)virt - hhdm_base;
}

size_t pmm_free_frame_count(void)
{
    uint64_t flags = spin_lock_irqsave(&pmm_lock);
    size_t count = free_frames;
    spin_unlock_irqrestore(&pmm_lock, flags);

    return count;
}

size_t pmm_total_frame_count(void)
{
    uint64_t flags = spin_lock_irqsave(&pmm_lock);
    size_t count = total_frames;
    spin_unlock_irqrestore(&pmm_lock, flags);

    return count;
}

void pmm_init(struct limine_memmap_response *memmap, uint64_t hhdm_offset)
{
    if (memmap == NULL) {
        kpanic("pmm_init: no Limine memory map");
    }

    spinlock_init(&pmm_lock);

    hhdm_base = hhdm_offset;
    total_frames = 0;
    free_frames = 0;
    search_hint = 1;

    /* Reserve everything, then release the usable ranges below 4 GiB. */
    memset(frame_bitmap, 0xFF, sizeof(frame_bitmap));
    memset(frame_refcount, 0, sizeof(frame_refcount));

    uint64_t highest_end = 0;
    for (uint64_t i = 0; i < memmap->entry_count; i++) {
        struct limine_memmap_entry *entry = memmap->entries[i];
        uint64_t end = entry->base + entry->length;

        if (end > highest_end) {
            highest_end = end;
        }
    }

    if (highest_end > PMM_MAX_PHYS) {
        highest_end = PMM_MAX_PHYS;
    }
    total_frames = (size_t)(highest_end / PAGE_SIZE);

    for (uint64_t i = 0; i < memmap->entry_count; i++) {
        struct limine_memmap_entry *entry = memmap->entries[i];

        if (entry->type != LIMINE_MEMMAP_USABLE) {
            continue;
        }

        uint64_t start = PAGE_ALIGN_UP(entry->base);
        uint64_t end = PAGE_ALIGN_DOWN(entry->base + entry->length);

        if (end > PMM_MAX_PHYS) {
            end = PAGE_ALIGN_DOWN(PMM_MAX_PHYS);
        }
        if (start >= end) {
            continue;
        }

        for (uint64_t addr = start; addr < end; addr += PAGE_SIZE) {
            size_t index = (size_t)(addr / PAGE_SIZE);

            if (index < total_frames && frame_is_used(index)) {
                frame_set_used(index, false);
                free_frames++;
            }
        }
    }

    /* Never hand out the real-mode IVT/BDA frame. */
    if (total_frames > 0 && !frame_is_used(0)) {
        frame_set_used(0, true);
        free_frames--;
    }

    /* The AP bootstrap trampoline lives below 1 MiB, which is outside the
     * bitmap's usable ranges but must never be handed out as general RAM. */
    size_t trampoline = (size_t)(PMM_AP_TRAMPOLINE_PHYS / PAGE_SIZE);

    if (trampoline < total_frames && !frame_is_used(trampoline)) {
        frame_set_used(trampoline, true);
        free_frames--;
    }
}

uint64_t pmm_ap_trampoline_page(void)
{
    return PMM_AP_TRAMPOLINE_PHYS;
}

/* Caller must hold pmm_lock. */
static void frame_release_locked(uint64_t phys)
{
    if ((phys & PAGE_MASK) != 0) {
        kpanic("pmm_free_frame: unaligned address 0x%016lx", phys);
    }

    size_t index = (size_t)(phys / PAGE_SIZE);
    if (index >= total_frames) {
        kpanic("pmm_free_frame: address 0x%016lx is not managed", phys);
    }
    if (!frame_is_used(index)) {
        kpanic("pmm_free_frame: double free at 0x%016lx", phys);
    }
    if (frame_refcount[index] == 0) {
        kpanic("pmm_free_frame: zero reference count at 0x%016lx", phys);
    }

    if (frame_refcount[index] > 1) {
        frame_refcount[index]--;
        return;
    }

    frame_refcount[index] = 0;
    frame_set_used(index, false);
    free_frames++;

    if (index < search_hint) {
        search_hint = index;
    }
}

sys_status_t pmm_frame_ref(paddr_t phys)
{
    if ((phys & PAGE_MASK) != 0) {
        return SYS_ERR_INVAL;
    }

    size_t index = (size_t)(phys / PAGE_SIZE);
    if (index >= total_frames) {
        return SYS_ERR_INVAL;
    }

    uint64_t flags = spin_lock_irqsave(&pmm_lock);

    if (!frame_is_used(index) || frame_refcount[index] == 0) {
        spin_unlock_irqrestore(&pmm_lock, flags);
        return SYS_ERR_INVAL;
    }
    if (frame_refcount[index] == UINT16_MAX) {
        spin_unlock_irqrestore(&pmm_lock, flags);
        return SYS_ERR_BUSY;
    }

    frame_refcount[index]++;
    spin_unlock_irqrestore(&pmm_lock, flags);
    return SYS_OK;
}

void pmm_frame_unref(paddr_t phys)
{
    uint64_t flags = spin_lock_irqsave(&pmm_lock);

    frame_release_locked(phys);
    spin_unlock_irqrestore(&pmm_lock, flags);
}

uint16_t pmm_frame_refcount(paddr_t phys)
{
    if ((phys & PAGE_MASK) != 0) {
        return 0;
    }

    size_t index = (size_t)(phys / PAGE_SIZE);
    if (index >= total_frames) {
        return 0;
    }

    uint64_t flags = spin_lock_irqsave(&pmm_lock);
    uint16_t count =
        frame_is_used(index) ? frame_refcount[index] : (uint16_t)0;
    spin_unlock_irqrestore(&pmm_lock, flags);

    return count;
}

uint64_t pmm_alloc_frame(void)
{
    uint64_t phys = 0;

    {
        uint64_t flags = spin_lock_irqsave(&pmm_lock);

        if (free_frames != 0 && total_frames != 0) {
            size_t index = search_hint;
            size_t scanned = 0;

            while (scanned < total_frames) {
                if (index >= total_frames) {
                    index = 0;
                }
                if (!frame_is_used(index)) {
                    frame_set_used(index, true);
                    frame_refcount[index] = 1;
                    free_frames--;

                    search_hint = index + 1;
                    if (search_hint >= total_frames) {
                        search_hint = 0;
                    }

                    phys = (uint64_t)index * PAGE_SIZE;
                    break;
                }

                index++;
                scanned++;
            }
        }

        spin_unlock_irqrestore(&pmm_lock, flags);
    }

    if (phys != 0) {
        /* Zero outside the lock: the frame is already reserved, so no other
         * CPU can observe it before this store completes. */
        memset(pmm_phys_to_virt(phys), 0, PAGE_SIZE);
    }

    return phys;
}

uint64_t pmm_alloc_frames(size_t count)
{
    uint64_t phys = 0;

    {
        uint64_t flags = spin_lock_irqsave(&pmm_lock);

        if (count != 0 && count <= free_frames && count <= total_frames) {
            for (size_t base = 0; base + count <= total_frames; base++) {
                size_t run = 0;

                while (run < count && !frame_is_used(base + run)) {
                    run++;
                }

                if (run != count) {
                    base += run;
                    continue;
                }

                for (size_t i = 0; i < count; i++) {
                    frame_set_used(base + i, true);
                    frame_refcount[base + i] = 1;
                }
                free_frames -= count;

                search_hint = base + count;
                if (search_hint >= total_frames) {
                    search_hint = 0;
                }

                phys = (uint64_t)base * PAGE_SIZE;
                break;
            }
        }

        spin_unlock_irqrestore(&pmm_lock, flags);
    }

    if (phys != 0) {
        for (size_t i = 0; i < count; i++) {
            memset(pmm_phys_to_virt(phys + (i * PAGE_SIZE)), 0, PAGE_SIZE);
        }
    }

    return phys;
}

void pmm_free_frame(uint64_t phys)
{
    uint64_t flags = spin_lock_irqsave(&pmm_lock);

    frame_release_locked(phys);
    spin_unlock_irqrestore(&pmm_lock, flags);
}

void pmm_free_frames(uint64_t phys, size_t count)
{
    uint64_t flags = spin_lock_irqsave(&pmm_lock);

    for (size_t i = 0; i < count; i++) {
        frame_release_locked(phys + (i * PAGE_SIZE));
    }

    spin_unlock_irqrestore(&pmm_lock, flags);
}

uint64_t pmm_alloc_contiguous(size_t page_count)
{
    if (page_count == 0 || page_count > PMM_CONTIGUOUS_MAX_PAGES) {
        return 0;
    }

    return pmm_alloc_frames(page_count);
}

void pmm_free_contiguous(uintptr_t phys, size_t page_count)
{
    if (page_count == 0 || page_count > PMM_CONTIGUOUS_MAX_PAGES ||
        (phys & (uintptr_t)(PAGE_SIZE - 1U)) != 0) {
        return;
    }

    pmm_free_frames((uint64_t)phys, page_count);
}
