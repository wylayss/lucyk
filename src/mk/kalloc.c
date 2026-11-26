#include <io.h>
#include <kalloc.h>
#include <klog.h>
#include <pmm.h>
#include <spinlock.h>
#include <types.h>
#include <vspace.h>

#define KALLOC_DEFAULT_CHUNK (64ULL * 1024ULL)
#define KALLOC_ALIGN         16ULL

#define BLOCK_MAGIC_FREE 0x4B4D454D46524545ULL /* "KMEMFREE" */
#define BLOCK_MAGIC_USED 0x4B4D454D55534544ULL /* "KMEMUSED" */

typedef struct kheap_chunk kheap_chunk_t;
typedef struct kheap_block kheap_block_t;

struct kheap_chunk {
    kheap_chunk_t *next;
    size_t size; /* total virtual chunk size in bytes, page multiple */
};

/* Header is 48 bytes so payloads stay 16-byte aligned. */
struct kheap_block {
    uint64_t magic;
    uint64_t size; /* payload bytes */
    kheap_block_t *prev;
    kheap_block_t *next;
    kheap_chunk_t *chunk;
    uint64_t reserved;
};

_Static_assert(sizeof(kheap_block_t) == 48, "kheap_block_t must be 48 bytes");
_Static_assert(sizeof(kheap_chunk_t) == 16, "kheap_chunk_t must be 16 bytes");

/* Held only for block-list metadata. Frame allocation and page-table
 * population happen outside this lock so the hierarchy stays
 * heap -> vspace -> PMM with no cycle. */
static spinlock_t heap_lock;

static kheap_chunk_t *chunk_list;
static vaddr_t kheap_next_vaddr;

static kheap_block_t *chunk_first_block(kheap_chunk_t *chunk)
{
    return (kheap_block_t *)((uint8_t *)chunk + sizeof(kheap_chunk_t));
}

/* Caller must hold heap_lock. */
static kheap_block_t *find_free_block(size_t size)
{
    for (kheap_chunk_t *chunk = chunk_list; chunk != NULL; chunk = chunk->next) {
        for (kheap_block_t *block = chunk_first_block(chunk);
             block != NULL;
             block = block->next) {
            if (block->magic == BLOCK_MAGIC_FREE && block->size >= size) {
                return block;
            }
        }
    }
    return NULL;
}

/* Reserves the next unused virtual range under heap_lock and maps it with no
 * lock held. Keeping the page-table mapping out of every heap critical
 * section preserves the acyclic hierarchy heap -> vspace -> PMM. Returns the
 * chunk size actually mapped, or 0. */
static size_t chunk_map(size_t total, vaddr_t *base_out)
{
    size_t frames = total / PAGE_SIZE;
    vaddr_t base;

    {
        uint64_t flags = spin_lock_irqsave(&heap_lock);

        base = kheap_next_vaddr;

        if (base < KHEAP_BASE || base >= KHEAP_LIMIT ||
            total > (size_t)(KHEAP_LIMIT - base)) {
            spin_unlock_irqrestore(&heap_lock, flags);
            return 0;
        }

        /* Reserve the range immediately so no peer cell reuses it, then drop
         * the lock before touching page tables or the PMM. */
        kheap_next_vaddr = base + total;
        spin_unlock_irqrestore(&heap_lock, flags);
    }

    size_t mapped = 0;

    for (; mapped < frames; mapped++) {
        paddr_t phys = pmm_alloc_frame();
        if (phys == 0) {
            break;
        }

        if (vspace_map_kernel(base + (mapped * PAGE_SIZE), phys,
                              VSPACE_PROT_WRITE) != SYS_OK) {
            pmm_frame_unref(phys);
            break;
        }
    }

    if (mapped == 0) {
        return 0;
    }

    *base_out = base;
    return mapped * (size_t)PAGE_SIZE;
}

/* Caller must hold heap_lock. Links an already mapped chunk. */
static kheap_block_t *chunk_link(vaddr_t base, size_t actual_total)
{
    kheap_chunk_t *chunk = (kheap_chunk_t *)(uintptr_t)base;

    chunk->next = chunk_list;
    chunk->size = actual_total;
    chunk_list = chunk;

    kheap_block_t *block = chunk_first_block(chunk);
    block->magic = BLOCK_MAGIC_FREE;
    block->size = actual_total - sizeof(kheap_chunk_t) - sizeof(kheap_block_t);
    block->prev = NULL;
    block->next = NULL;
    block->chunk = chunk;
    block->reserved = 0;

    return block;
}

void kalloc_init(void)
{
    spinlock_init(&heap_lock);
    chunk_list = NULL;
    kheap_next_vaddr = KHEAP_BASE;
}

void *kalloc(size_t size)
{
    if (size == 0) {
        return NULL;
    }

    size_t aligned = (size_t)ALIGN_UP(size, KALLOC_ALIGN);
    if (aligned < size) {
        return NULL;
    }
    size = aligned;

    size_t minimum = size > KALLOC_DEFAULT_CHUNK ? size : KALLOC_DEFAULT_CHUNK;

    uint64_t flags = spin_lock_irqsave(&heap_lock);
    kheap_block_t *block = find_free_block(size);

    if (block == NULL) {
        spin_unlock_irqrestore(&heap_lock, flags);

        if (minimum >
            (size_t)-1 - (sizeof(kheap_chunk_t) + sizeof(kheap_block_t) +
                          PAGE_SIZE)) {
            return NULL;
        }

        size_t total = (size_t)PAGE_ALIGN_UP(sizeof(kheap_chunk_t) +
                                             sizeof(kheap_block_t) + minimum);

        vaddr_t base = 0;
        size_t actual_total = chunk_map(total, &base);

        if (actual_total == 0) {
            return NULL;
        }

        /* Link and then re-search under one critical section. A block found
         * before a lock release may already belong to another CPU. */
        flags = spin_lock_irqsave(&heap_lock);
        chunk_link(base, actual_total);
        block = find_free_block(size);

        if (block == NULL) {
            spin_unlock_irqrestore(&heap_lock, flags);
            return NULL;
        }
    }

    /* The found block is committed here, still under the same lock. */
    if (block->size >= size + sizeof(kheap_block_t) + KALLOC_ALIGN) {
        kheap_block_t *rest =
            (kheap_block_t *)((uint8_t *)(block + 1) + size);

        rest->magic = BLOCK_MAGIC_FREE;
        rest->size = block->size - size - sizeof(kheap_block_t);
        rest->prev = block;
        rest->next = block->next;
        rest->chunk = block->chunk;
        rest->reserved = 0;

        if (rest->next != NULL) {
            rest->next->prev = rest;
        }
        block->next = rest;
        block->size = size;
    }

    block->magic = BLOCK_MAGIC_USED;

    void *result = (void *)(block + 1);
    spin_unlock_irqrestore(&heap_lock, flags);
    return result;
}

void kfree(void *ptr)
{
    if (ptr == NULL) {
        return;
    }

    uint64_t flags = spin_lock_irqsave(&heap_lock);

    uintptr_t address = (uintptr_t)ptr;
    uintptr_t heap_min = (uintptr_t)KHEAP_BASE +
                         sizeof(kheap_chunk_t) + sizeof(kheap_block_t);

    if (address < heap_min ||
        address >= (uintptr_t)kheap_next_vaddr ||
        (address & (uintptr_t)(KALLOC_ALIGN - 1ULL)) != 0) {
        spin_unlock_irqrestore(&heap_lock, flags);
        kpanic("kfree: pointer %p is outside the kernel heap", ptr);
    }

    kheap_block_t *block =
        (kheap_block_t *)((uint8_t *)ptr - sizeof(kheap_block_t));

    if (block->magic != BLOCK_MAGIC_USED) {
        spin_unlock_irqrestore(&heap_lock, flags);
        kpanic("kfree: invalid or double-freed block at %p", ptr);
    }

    block->magic = BLOCK_MAGIC_FREE;

    if (block->next != NULL && block->next->magic == BLOCK_MAGIC_FREE) {
        kheap_block_t *next = block->next;

        block->size += sizeof(kheap_block_t) + next->size;
        block->next = next->next;
        if (block->next != NULL) {
            block->next->prev = block;
        }
    }

    if (block->prev != NULL && block->prev->magic == BLOCK_MAGIC_FREE) {
        kheap_block_t *prev = block->prev;

        prev->size += sizeof(kheap_block_t) + block->size;
        prev->next = block->next;
        if (prev->next != NULL) {
            prev->next->prev = prev;
        }
    }

    spin_unlock_irqrestore(&heap_lock, flags);
}
