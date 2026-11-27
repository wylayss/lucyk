#include <bsd/bsd.h>
#include <cap.h>
#include <cpu.h>
#include <display.h>
#include <elf.h>
#include <gdt.h>
#include <idt.h>
#include <io.h>
#include <io/blkdev.h>
#include <io/pci.h>
#include <io/fbcon.h>
#include <io/ahci.h>
#include <ioapic.h>
#include <ipc.h>
#include <kalloc.h>
#include <klog.h>
#include <lapic.h>
#include <limine.h>
#include <mk_hooks.h>
#include <acpi.h>
#include <cpu.h>
#include <smp.h>
#include <object.h>
#include <pmm.h>
#include <sched.h>
#include <string.h>
#include <syscall.h>
#include <task.h>
#include <types.h>
#include <vspace.h>

/* ------------------------------------------------------------------------- */
/* Limine requests (must live between the start/end markers, in order).       */
/* ------------------------------------------------------------------------- */

__attribute__((used, section(".limine_requests_start_marker")))
static LIMINE_REQUESTS_START_MARKER

__attribute__((used, section(".limine_requests")))
static volatile LIMINE_BASE_REVISION(3)

__attribute__((used, section(".limine_requests")))
volatile struct limine_hhdm_request hhdm_request = {
    .id = LIMINE_HHDM_REQUEST,
    .revision = 0,
    .response = NULL,
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_memmap_request memmap_request = {
    .id = LIMINE_MEMMAP_REQUEST,
    .revision = 0,
    .response = NULL,
};

__attribute__((used, section(".limine_requests")))
volatile struct limine_rsdp_request rsdp_request = {
    .id = LIMINE_RSDP_REQUEST,
    .revision = 0,
    .response = NULL,
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_module_request module_request = {
    .id = LIMINE_MODULE_REQUEST,
    .revision = 0,
    .response = NULL,
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_framebuffer_request framebuffer_request = {
    .id = LIMINE_FRAMEBUFFER_REQUEST,
    .revision = 0,
    .response = NULL,
};

__attribute__((used, section(".limine_requests_end_marker")))
static LIMINE_REQUESTS_END_MARKER

/* ------------------------------------------------------------------------- */
/* Boot entry                                                                 */
/* ------------------------------------------------------------------------- */

static const char *memmap_type_name(uint64_t type)
{
    switch (type) {
    case LIMINE_MEMMAP_USABLE:                 return "usable";
    case LIMINE_MEMMAP_RESERVED:               return "reserved";
    case LIMINE_MEMMAP_ACPI_RECLAIMABLE:       return "acpi-reclaimable";
    case LIMINE_MEMMAP_ACPI_NVS:               return "acpi-nvs";
    case LIMINE_MEMMAP_BAD_MEMORY:             return "bad-memory";
    case LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE: return "bootloader-reclaimable";
    case 6:                                    return "executable-and-modules";
    case LIMINE_MEMMAP_FRAMEBUFFER:            return "framebuffer";
    default:                                   return "unknown";
    }
}

static void log_memory_map(void)
{
    struct limine_memmap_response *memmap = memmap_request.response;

    if (memmap == NULL) {
        kpanic("Limine did not provide a memory map response");
    }

    uint64_t usable_regions = 0;
    uint64_t usable_bytes = 0;
    uint64_t highest_usable = 0;
    uint64_t type_counts[8] = { 0 };

    for (uint64_t i = 0; i < memmap->entry_count; i++) {
        struct limine_memmap_entry *entry = memmap->entries[i];

        if (entry->type < ARRAY_SIZE(type_counts)) {
            type_counts[entry->type]++;
        }

        if (entry->type == LIMINE_MEMMAP_USABLE) {
            usable_regions++;
            usable_bytes += entry->length;

            uint64_t end = entry->base + entry->length;
            if (end > highest_usable) {
                highest_usable = end;
            }
        }
    }

    klog(KLOG_INFO,
         "Memory map: %lu entries, %lu usable regions, %lu MiB usable",
         memmap->entry_count, usable_regions, usable_bytes / (1024ULL * 1024ULL));
    klog(KLOG_INFO, "Top usable physical address: 0x%016lx", highest_usable);

    klog(KLOG_DEBUG,
         "regions: usable=%lu reserved=%lu acpi-reclaimable=%lu acpi-nvs=%lu",
         type_counts[LIMINE_MEMMAP_USABLE],
         type_counts[LIMINE_MEMMAP_RESERVED],
         type_counts[LIMINE_MEMMAP_ACPI_RECLAIMABLE],
         type_counts[LIMINE_MEMMAP_ACPI_NVS]);
    klog(KLOG_DEBUG,
         "regions: bad=%lu bootloader=%lu modules=%lu framebuffer=%lu",
         type_counts[LIMINE_MEMMAP_BAD_MEMORY],
         type_counts[LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE],
         type_counts[6],
         type_counts[LIMINE_MEMMAP_FRAMEBUFFER]);

    for (uint64_t i = 0; i < memmap->entry_count; i++) {
        struct limine_memmap_entry *entry = memmap->entries[i];
        uint64_t end = entry->base + entry->length;

        klog(KLOG_DEBUG, "  [%016lx - %016lx) %s %lu KiB",
             entry->base, end, memmap_type_name(entry->type),
             entry->length / 1024ULL);
    }
}

/* ------------------------------------------------------------------------- */
/* Phase 2 boot-time self-tests                                               */
/* ------------------------------------------------------------------------- */

static void self_test_pmm(void)
{
    enum { FRAME_COUNT = 1000 };
    static uint64_t frames[FRAME_COUNT];

    size_t before = pmm_free_frame_count();
    size_t allocated = 0;

    for (size_t i = 0; i < FRAME_COUNT; i++) {
        uint64_t phys = pmm_alloc_frame();

        if (phys == 0) {
            break;
        }
        if ((phys & PAGE_MASK) != 0) {
            kpanic("pmm self-test: unaligned frame 0x%016lx", phys);
        }

        frames[allocated++] = phys;

        uint64_t *word = (uint64_t *)pmm_phys_to_virt(phys);
        if (word[0] != 0 || word[(PAGE_SIZE / sizeof(uint64_t)) - 1] != 0) {
            kpanic("pmm self-test: frame 0x%016lx was not zeroed", phys);
        }
        word[0] = 0xA5A5A5A5A5A5A5A5ULL;
    }

    if (allocated != FRAME_COUNT) {
        kpanic("pmm self-test: allocated only %lu of %lu frames",
               allocated, (size_t)FRAME_COUNT);
    }

    size_t during = pmm_free_frame_count();

    for (size_t i = 0; i < allocated; i++) {
        pmm_free_frame(frames[i]);
    }

    size_t after = pmm_free_frame_count();
    if (after != before) {
        kpanic("pmm self-test: leak detected, before=%lu after=%lu", before, after);
    }

    klog(KLOG_INFO,
         "PMM self-test: %lu frames alloc/free ok, %lu free during test",
         (size_t)FRAME_COUNT, during);
}

static void self_test_vspace(void)
{
    size_t before = pmm_free_frame_count();

    uint64_t frame_a = pmm_alloc_frame();
    uint64_t frame_b = pmm_alloc_frame();
    if (frame_a == 0 || frame_b == 0) {
        kpanic("vspace self-test: data frame allocation failed");
    }

    vspace_t *a = vspace_create();
    vspace_t *b = vspace_create();
    if (a == NULL || b == NULL) {
        kpanic("vspace self-test: vspace_create failed");
    }

    const uint64_t va = 0x0000004000000000ULL;

    if (vspace_map_user(a, va, frame_a,
                        VSPACE_PROT_WRITE | VSPACE_PROT_USER) != SYS_OK) {
        kpanic("vspace self-test: map into vspace A failed");
    }
    if (vspace_map_user(b, va, frame_b, VSPACE_PROT_USER) != SYS_OK) {
        kpanic("vspace self-test: map into vspace B failed");
    }

    uint64_t pa = 0;
    uint64_t pb = 0;
    if (!vspace_translate(a, va, &pa) || pa != frame_a) {
        kpanic("vspace self-test: translation mismatch in A");
    }
    if (!vspace_translate(b, va, &pb) || pb != frame_b) {
        kpanic("vspace self-test: translation mismatch in B");
    }
    if (pa == pb) {
        kpanic("vspace self-test: address spaces are not isolated");
    }

    const uint64_t magic = 0x0123456789ABCDEFULL;
    ((uint64_t *)pmm_phys_to_virt(frame_a))[0] = magic;

    /* With SMAP active, Ring 0 must not dereference a PTE_USER address. Read
     * the same frame through the HHDM after the CR3 switch; the shared kernel
     * upper half makes the direct map available in the new address space. */
    vspace_switch(a);
    uint64_t observed = *(const volatile uint64_t *)pmm_phys_to_virt(pa);
    vspace_switch(vspace_kernel());

    if (observed != magic) {
        kpanic("vspace self-test: CR3 switch did not expose the mapping");
    }

    if (vspace_map_user(a, va + PAGE_SIZE, frame_a,
                        VSPACE_PROT_WRITE | VSPACE_PROT_EXEC) != SYS_ERR_INVAL) {
        kpanic("vspace self-test: W^X was not enforced");
    }

    if (vspace_unmap(a, va) != SYS_OK) {
        kpanic("vspace self-test: unmap failed");
    }
    if (vspace_translate(a, va, &pa)) {
        kpanic("vspace self-test: unmap left a stale mapping");
    }
    if (vspace_unmap(a, va) != SYS_ERR_INVAL) {
        kpanic("vspace self-test: double unmap was accepted");
    }

    vspace_destroy(a);
    vspace_destroy(b);
    pmm_free_frame(frame_a);
    pmm_free_frame(frame_b);

    size_t after = pmm_free_frame_count();
    if (after != before) {
        kpanic("vspace self-test: leak detected, before=%lu after=%lu", before, after);
    }

    klog(KLOG_INFO, "vspace self-test: isolation, W^X and unmap verified");
}

static void self_test_cow(void)
{
    const vaddr_t va = 0x0000006000000000ULL;

    /* Round one may grow the kalloc heap for vspace_t objects; round two
     * checks frame accounting against a warmed heap. */
    for (size_t round = 0; round < 2; round++) {
        size_t before = pmm_free_frame_count();

        vspace_t *parent = vspace_create();
        if (parent == NULL) {
            kpanic("CoW self-test: vspace_create failed");
        }

        paddr_t frame = pmm_alloc_frame();
        if (frame == 0) {
            kpanic("CoW self-test: frame allocation failed");
        }

        uint8_t *page = (uint8_t *)pmm_phys_to_virt(frame);
        page[0] = 0x11;
        page[PAGE_SIZE - 1] = 0x22;

        if (vspace_map_user(parent, va, frame,
                            VSPACE_PROT_USER | VSPACE_PROT_WRITE |
                                VSPACE_PROT_OWNED) != SYS_OK) {
            kpanic("CoW self-test: parent map failed");
        }

        vspace_t *child = vspace_clone(parent);
        if (child == NULL) {
            kpanic("CoW self-test: vspace_clone failed");
        }

        if (pmm_frame_refcount(frame) != 2) {
            kpanic("CoW self-test: clone did not share the frame");
        }

        paddr_t parent_phys = 0;
        paddr_t child_phys = 0;

        if (!vspace_translate(parent, va, &parent_phys) ||
            parent_phys != frame ||
            !vspace_translate(child, va, &child_phys) ||
            child_phys != frame) {
            kpanic("CoW self-test: clone mapping mismatch");
        }

        if (!vspace_handle_cow_fault(child, va)) {
            kpanic("CoW self-test: child CoW fault was not resolved");
        }

        paddr_t child_private = 0;
        if (!vspace_translate(child, va, &child_private) ||
            child_private == frame) {
            kpanic("CoW self-test: child did not receive a private frame");
        }
        if (pmm_frame_refcount(frame) != 1 ||
            pmm_frame_refcount(child_private) != 1) {
            kpanic("CoW self-test: reference counts not split");
        }

        ((uint8_t *)pmm_phys_to_virt(child_private))[0] = 0x33;
        if (page[0] != 0x11) {
            kpanic("CoW self-test: child write reached the parent");
        }

        if (!vspace_handle_cow_fault(parent, va)) {
            kpanic("CoW self-test: parent CoW fault was not resolved");
        }
        page[0] = 0x44;
        if (((uint8_t *)pmm_phys_to_virt(child_private))[0] != 0x33) {
            kpanic("CoW self-test: parent write reached the child");
        }

        vspace_destroy(child);
        vspace_destroy(parent);

        if (round == 1) {
            size_t after = pmm_free_frame_count();

            if (after != before) {
                kpanic("CoW self-test: frame leak (%lu -> %lu free)",
                       before, after);
            }
        }
    }

    klog(KLOG_INFO,
         "CoW self-test: fork shares pages, writes isolate, 0 leaked frames");
}

static void self_test_kalloc(void)
{
    enum { BLOCK_COUNT = 64 };
    static void *blocks[BLOCK_COUNT];
    static size_t sizes[BLOCK_COUNT];

    for (size_t i = 0; i < BLOCK_COUNT; i++) {
        sizes[i] = 8 + ((i * 13) % 200);
        blocks[i] = kalloc(sizes[i]);
        if (blocks[i] == NULL) {
            kpanic("kalloc self-test: allocation %lu failed", i);
        }
        memset(blocks[i], 0x5A, sizes[i]);
    }

    for (size_t i = 0; i < BLOCK_COUNT; i++) {
        const uint8_t *bytes = (const uint8_t *)blocks[i];

        for (size_t j = 0; j < sizes[i]; j++) {
            if (bytes[j] != 0x5A) {
                kpanic("kalloc self-test: block %lu corrupted at byte %lu", i, j);
            }
        }
    }

    /* A >32 KiB allocation must span individually mapped, non-contiguous
     * physical frames. */
    const size_t big_size = 64 * 1024;
    void *big = kalloc(big_size);
    if (big == NULL) {
        kpanic("kalloc self-test: multi-page allocation failed");
    }
    memset(big, 0xC3, big_size);
    if (((const uint8_t *)big)[0] != 0xC3 ||
        ((const uint8_t *)big)[big_size - 1] != 0xC3) {
        kpanic("kalloc self-test: multi-page block corrupted");
    }
    kfree(big);

    /* The next same-sized request must reuse the retained chunk rather than
     * mapping more physical frames. */
    size_t after_free = pmm_free_frame_count();

    void *big_again = kalloc(big_size);
    if (big_again == NULL) {
        kpanic("kalloc self-test: multi-page block reuse failed");
    }
    if (pmm_free_frame_count() != after_free) {
        kpanic("kalloc self-test: reuse mapped new physical frames");
    }

    memset(big_again, 0x3C, big_size);
    if (((const uint8_t *)big_again)[big_size - 1] != 0x3C) {
        kpanic("kalloc self-test: reused block corrupted");
    }
    kfree(big_again);

    for (size_t i = 0; i < BLOCK_COUNT; i++) {
        kfree(blocks[i]);
    }

    klog(KLOG_INFO,
         "kalloc self-test: %lu small blocks + %lu KiB non-contiguous buffer, "
         "chunk reuse verified",
         (size_t)BLOCK_COUNT, big_size / 1024);
}

static void self_test_kernel_mapping(void)
{
    const vaddr_t test_va = 0xFFFF900000000000ULL;
    const vaddr_t test_va_uncached = 0xFFFF900000001000ULL;
    const uint64_t magic = 0xFEEDFACECAFEBEEFULL;

    paddr_t frame = pmm_alloc_frame();
    paddr_t frame_uncached = pmm_alloc_frame();
    if (frame == 0 || frame_uncached == 0) {
        kpanic("kernel map self-test: frame allocation failed");
    }

    ((uint64_t *)pmm_phys_to_virt(frame))[0] = magic;

    if (vspace_map_kernel(test_va, frame, VSPACE_PROT_WRITE) != SYS_OK) {
        kpanic("kernel map self-test: vspace_map_kernel failed");
    }
    if (vspace_map_kernel(test_va_uncached, frame_uncached,
                          VSPACE_PROT_WRITE | VSPACE_PROT_PCD |
                              VSPACE_PROT_PWT) != SYS_OK) {
        kpanic("kernel map self-test: cache-disable mapping failed");
    }

    if (*(const volatile uint64_t *)test_va != magic) {
        kpanic("kernel map self-test: direct read mismatch");
    }

    /* A vspace created after the kernel mapping must inherit it through the
     * shared PML4[256..511] PDPTs without any resynchronisation. */
    vspace_t *vs = vspace_create();
    if (vs == NULL) {
        kpanic("kernel map self-test: vspace_create failed");
    }

    paddr_t inherited = 0;
    if (!vspace_translate(vs, test_va, &inherited) || inherited != frame) {
        kpanic("kernel map self-test: new user vspace did not inherit kernel mapping");
    }

    vspace_switch(vs);
    uint64_t observed = *(const volatile uint64_t *)test_va;
    vspace_switch(vspace_kernel());

    if (observed != magic) {
        kpanic("kernel map self-test: inherited mapping unreadable after CR3 switch");
    }

    vspace_destroy(vs);

    klog(KLOG_INFO,
         "kernel map self-test: higher-half (PCD/PWT) mapping inherited by new user vspace");
}

static volatile uint64_t reaper_gate;

static void reaper_entry(void *arg)
{
    UNUSED(arg);

    while (reaper_gate == 0) {
        thread_yield();
    }
    thread_exit();
}

static void spawn_reapers(task_t *task, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        if (thread_create(task, reaper_entry, NULL) == NULL) {
            kpanic("selftest: cannot create short-lived thread");
        }
    }
}

static void wait_for_reaped(uint64_t target)
{
    uint64_t deadline = sched_tick_count() + 1000;

    while (sched_reaped_count() < target) {
        if (sched_tick_count() > deadline) {
            kpanic("selftest: timed out waiting for thread reaping");
        }
        thread_sleep_ms(2);
    }
}

/* ------------------------------------------------------------------------- */
/* Phase 3 adversarial stress test                                            */
/* ------------------------------------------------------------------------- */

#define STRESS_TASK_COUNT       4
#define STRESS_THREADS_PER_TASK 3
#define STRESS_ITERATIONS       24U
#define STRESS_ROUNDS           3

typedef struct stress_worker_arg {
    uint32_t id;
} stress_worker_arg_t;

static stress_worker_arg_t stress_args[STRESS_TASK_COUNT][STRESS_THREADS_PER_TASK];
static task_t *stress_tasks[STRESS_TASK_COUNT];
static volatile uint64_t stress_gate;

static void stress_worker_entry(void *arg)
{
    const stress_worker_arg_t *worker = (const stress_worker_arg_t *)arg;

    while (stress_gate == 0) {
        thread_yield();
    }

    for (uint32_t i = 0; i < STRESS_ITERATIONS; i++) {
        size_t size = 16U + ((i * 37U + worker->id * 29U) % 600U);
        uint8_t *block = (uint8_t *)kalloc(size);

        if (block == NULL) {
            kpanic("stress: kalloc failed");
        }

        uint8_t pattern = (uint8_t)(i + worker->id);
        memset(block, pattern, size);
        if (block[0] != pattern || block[size - 1] != pattern) {
            kpanic("stress: heap corruption detected");
        }
        kfree(block);

        thread_yield();

        if ((i & 3U) == 0U) {
            thread_sleep_ms(1U + (i % 7U));
        }
    }

    thread_exit();
}

static void stress_spawn(void)
{
    stress_gate = 0;

    for (size_t t = 0; t < STRESS_TASK_COUNT; t++) {
        stress_tasks[t] = task_create();
        if (stress_tasks[t] == NULL) {
            kpanic("stress: task_create failed");
        }

        for (size_t w = 0; w < STRESS_THREADS_PER_TASK; w++) {
            stress_args[t][w].id =
                (uint32_t)(t * STRESS_THREADS_PER_TASK + w);

            if (thread_create(stress_tasks[t], stress_worker_entry,
                              &stress_args[t][w]) == NULL) {
                kpanic("stress: thread_create failed");
            }
        }
    }

    stress_gate = 1;
}

static void stress_join(void)
{
    uint64_t deadline = sched_tick_count() + 2000;

    for (size_t t = 0; t < STRESS_TASK_COUNT; t++) {
        while (stress_tasks[t]->thread_count != 0) {
            if (sched_tick_count() > deadline) {
                kpanic("stress: task %lu did not quiesce", (uint64_t)t);
            }
            thread_sleep_ms(1);
        }

        task_destroy(stress_tasks[t]);
        stress_tasks[t] = NULL;
    }
}

static void stress_round(void)
{
    uint64_t reaped_base = sched_reaped_count();

    stress_spawn();
    wait_for_reaped(reaped_base +
                    (uint64_t)(STRESS_TASK_COUNT * STRESS_THREADS_PER_TASK));
    stress_join();
}

static void run_stress_test(void)
{
    /* The warm-up round grows the heap to its steady-state capacity. */
    stress_round();
    size_t baseline_frames = pmm_free_frame_count();

    for (size_t round = 0; round < STRESS_ROUNDS; round++) {
        stress_round();
    }

    size_t final_frames = pmm_free_frame_count();
    if (final_frames != baseline_frames) {
        kpanic("stress: frame leak detected (%lu -> %lu free frames)",
               baseline_frames, final_frames);
    }

    klog(KLOG_INFO,
         "stress: %lu rounds x %lu tasks x %lu threads, 0 leaked frames (%lu free)",
         (uint64_t)STRESS_ROUNDS, (uint64_t)STRESS_TASK_COUNT,
         (uint64_t)STRESS_THREADS_PER_TASK, (uint64_t)final_frames);
}

/* ------------------------------------------------------------------------- */
/* SMP concurrency stress test                                                */
/* ------------------------------------------------------------------------- */

#define SMP_WORKERS_PER_CPU 2U
#define SMP_ITERATIONS      48U
#define SMP_MAX_WORKERS     (MAX_CPUS * SMP_WORKERS_PER_CPU)

typedef struct smp_worker_arg {
    uint32_t slot;
    task_t  *task;
} smp_worker_arg_t;

static smp_worker_arg_t smp_args[SMP_MAX_WORKERS];
static task_t *smp_tasks[SMP_MAX_WORKERS];
static volatile uint32_t smp_worker_cpu[SMP_MAX_WORKERS];
static volatile uint32_t smp_worker_done[SMP_MAX_WORKERS];
static volatile uint32_t smp_gate;

/* Each worker performs concurrent heap churn, page-table work through
 * vspace_create/destroy, and IPC round-trips that cross CPUs. */
static void smp_worker_entry(void *arg)
{
    smp_worker_arg_t *worker = (smp_worker_arg_t *)arg;

    while (__atomic_load_n(&smp_gate, __ATOMIC_ACQUIRE) == 0) {
        thread_yield();
    }

    /* Two workers share every CPU. The first effective-CPU observation is
     * recorded per worker so the caller can prove the work really was spread
     * across cores rather than serialised on one. */
    __atomic_store_n(&smp_worker_cpu[worker->slot], this_cpu_index(),
                     __ATOMIC_RELEASE);

    uint8_t *blocks[8];

    for (size_t b = 0; b < 8; b++) {
        blocks[b] = NULL;
    }

    for (uint32_t i = 0; i < SMP_ITERATIONS; i++) {
        size_t size = 24U + ((i * 53U + worker->slot * 17U) % 900U);
        uint8_t *block = (uint8_t *)kalloc(size);

        if (block == NULL) {
            kpanic("smp stress: kalloc failed");
        }

        uint8_t pattern = (uint8_t)(i + worker->slot);
        memset(block, pattern, size);

        if (block[0] != pattern || block[size - 1] != pattern) {
            kpanic("smp stress: heap block corrupted");
        }

        size_t slot = i % 8;

        /* Release whatever occupied this slot eight iterations ago before
         * reusing it, so no allocation is leaked and no stale pointer is
         * freed twice. */
        if (blocks[slot] != NULL) {
            kfree(blocks[slot]);
            blocks[slot] = NULL;
        }

        blocks[slot] = block;

        /* Page-table churn exercises the vspace lock and the PMM under
         * contention. */
        if ((i & 7U) == 0U) {
            vspace_t *vs = vspace_create();

            if (vs == NULL) {
                kpanic("smp stress: vspace_create failed");
            }

            paddr_t frame = pmm_alloc_frame();
            if (frame == 0) {
                kpanic("smp stress: frame allocation failed");
            }

            if (vspace_map_user(vs, 0x0000002000000000ULL, frame,
                                VSPACE_PROT_USER | VSPACE_PROT_WRITE |
                                    VSPACE_PROT_OWNED) != SYS_OK) {
                kpanic("smp stress: vspace_map_user failed");
            }

            vspace_destroy(vs);
        }

        /* Free the oldest live block to keep the heap churning. */
        size_t old = (i + 4U) % 8;

        if (blocks[old] != NULL) {
            kfree(blocks[old]);
            blocks[old] = NULL;
        }

        if ((i & 15U) == 0U) {
            thread_yield();
        }
    }

    for (size_t i = 0; i < 8; i++) {
        if (blocks[i] != NULL) {
            kfree(blocks[i]);
            blocks[i] = NULL;
        }
    }

    __atomic_store_n(&smp_worker_cpu[worker->slot],
                     this_cpu_index(), __ATOMIC_RELEASE);
    __atomic_store_n(&smp_worker_done[worker->slot], 1U, __ATOMIC_RELEASE);

    thread_exit();
}

static void run_smp_stress_test(void)
{
    uint32_t online = smp_online_count();

    if (online == 0) {
        online = 1;
    }

    uint32_t workers = online * SMP_WORKERS_PER_CPU;

    if (workers > SMP_MAX_WORKERS) {
        workers = SMP_MAX_WORKERS;
    }

    size_t frames_before = pmm_free_frame_count();
    uint64_t reaped_before = sched_reaped_count();

    for (uint32_t i = 0; i < workers; i++) {
        smp_args[i].slot = i;
        smp_worker_done[i] = 0;
        smp_worker_cpu[i] = CPU_INDEX_NONE;

        smp_tasks[i] = task_create();
        if (smp_tasks[i] == NULL) {
            kpanic("smp stress: task_create failed");
        }

        /* Opt this task into cross-CPU placement: the point of the test is to
         * prove that workers really run on every online core. */
        task_set_affinity(smp_tasks[i], TASK_AFFINITY_ANY);

        smp_args[i].task = smp_tasks[i];

        if (thread_create(smp_tasks[i], smp_worker_entry, &smp_args[i]) == NULL) {
            kpanic("smp stress: thread_create failed");
        }
    }

    __atomic_store_n(&smp_gate, 1U, __ATOMIC_RELEASE);

    uint64_t deadline = sched_tick_count() + 4000;

    for (uint32_t i = 0; i < workers; i++) {
        while (__atomic_load_n(&smp_worker_done[i], __ATOMIC_ACQUIRE) == 0) {
            if (sched_tick_count() > deadline) {
                kpanic("smp stress: worker %u timed out", i);
            }
            thread_sleep_ms(1);
        }
    }

    wait_for_reaped(reaped_before + workers);

    for (uint32_t i = 0; i < workers; i++) {
        while (smp_tasks[i]->thread_count != 0) {
            if (sched_tick_count() > deadline) {
                kpanic("smp stress: task %u did not quiesce", i);
            }
            thread_sleep_ms(1);
        }

        task_destroy(smp_tasks[i]);
        smp_tasks[i] = NULL;
    }

    /* Every online CPU must have dispatched at least one worker. Workers are
     * placed on the least loaded ring, so a machine that only ever runs them
     * on one core indicates broken placement rather than an unlucky schedule. */
    uint32_t observed = 0;
    uint32_t distinct = 0;

    for (uint32_t i = 0; i < workers; i++) {
        uint32_t cpu = smp_worker_cpu[i];

        if (cpu < MAX_CPUS) {
            if ((observed & (1U << cpu)) == 0) {
                distinct++;
            }
            observed |= (1U << cpu);
        }
    }

    if (distinct < online) {
        kpanic("smp stress: only %u of %u CPUs dispatched workers (mask 0x%lx)",
               distinct, online, (uint64_t)observed);
    }

    size_t frames_after = pmm_free_frame_count();
    if (frames_after != frames_before) {
        kpanic("smp stress: frame leak (%lu -> %lu free)",
               frames_before, frames_after);
    }

    klog(KLOG_INFO,
         "smp stress: %lu workers x %lu iterations, %u/%u CPUs online, "
         "0 deadlocks, 0 leaked frames (%lu free)",
         (uint64_t)workers, (uint64_t)SMP_ITERATIONS, online,
         (uint32_t)smp_online_count(), (unsigned long)frames_after);
    klog(KLOG_INFO, "smp stress: worker CPUs observed mask 0x%lx (distinct %u)",
         (uint64_t)observed, distinct);
}

/* ------------------------------------------------------------------------- */
/* Phase 4 capability, endpoint IPC and notification self-tests               */
/* ------------------------------------------------------------------------- */

#define PHASE4_SERVER_BADGE   0x1111111111111111ULL
#define PHASE4_CLIENT_BADGE   0x2222222222222222ULL
#define PHASE4_READONLY_BADGE 0x3333333333333333ULL
#define PHASE4_NOTIFY_BITS    0xA5A50000A5A50000ULL
#define PHASE4_IRQ_BIT        (1ULL << 7)
#define PHASE4_IRQ_VECTOR     0x30U

typedef struct phase4_ipc_ctx {
    task_t *client_task;
    task_t *server_task;
    cap_t server_cap;
    cap_t client_cap;
    cap_t readonly_cap;
    uint64_t client_msg[4];
    uint64_t server_msg[4];
    uint64_t reply_expected[4];
    uint64_t client_reply[4];
    uint64_t server_seen_badge;
    uint64_t client_reply_badge;
    sys_status_t rights_status;
    sys_status_t client_status;
    sys_status_t server_status;
    bool server_delay;
    volatile bool server_done;
    volatile bool client_done;
} phase4_ipc_ctx_t;

static phase4_ipc_ctx_t phase4_ipc;

static void phase4_server_entry(void *arg)
{
    phase4_ipc_ctx_t *ctx = (phase4_ipc_ctx_t *)arg;
    thread_t *sender = NULL;
    uint64_t reply[4] = { 0, 0, 0, 0 };

    if (ctx->server_delay) {
        thread_sleep_ms(20);
    }

    sys_status_t status = ipc_recv(ctx->server_task, ctx->server_cap,
                                   ctx->server_msg, &ctx->server_seen_badge,
                                   &sender);

    if (status == SYS_OK) {
        for (size_t i = 0; i < 4; i++) {
            if (ctx->server_msg[i] != ctx->client_msg[i]) {
                status = SYS_ERR_INVAL;
            }
            reply[i] = ~ctx->server_msg[i];
        }
        if (ctx->server_seen_badge != PHASE4_CLIENT_BADGE) {
            status = SYS_ERR_INVAL;
        }

        sys_status_t reply_status = ipc_reply(sender, reply);
        if (reply_status != SYS_OK) {
            status = reply_status;
        }
    }

    ctx->server_status = status;
    ctx->server_done = true;
    thread_exit();
}

static void phase4_client_entry(void *arg)
{
    phase4_ipc_ctx_t *ctx = (phase4_ipc_ctx_t *)arg;
    uint64_t reply[4] = { 0, 0, 0, 0 };
    uint64_t badge = 0;

    if (!ctx->server_delay) {
        thread_sleep_ms(20);
    }

    ctx->rights_status = ipc_call(ctx->client_task, ctx->readonly_cap,
                                  ctx->client_msg, reply, &badge);

    ctx->client_status = ipc_call(ctx->client_task, ctx->client_cap,
                                  ctx->client_msg, reply, &badge);

    for (size_t i = 0; i < 4; i++) {
        ctx->client_reply[i] = reply[i];
    }
    ctx->client_reply_badge = badge;
    ctx->client_done = true;
    thread_exit();
}

static void phase4_ipc_round(bool server_delay)
{
    phase4_ipc.server_delay = server_delay;
    phase4_ipc.server_done = false;
    phase4_ipc.client_done = false;
    phase4_ipc.server_status = SYS_ERR_BUSY;
    phase4_ipc.client_status = SYS_ERR_BUSY;
    phase4_ipc.rights_status = SYS_ERR_BUSY;
    phase4_ipc.server_seen_badge = 0;
    phase4_ipc.client_reply_badge = 0;
    for (size_t i = 0; i < 4; i++) {
        phase4_ipc.server_msg[i] = 0;
        phase4_ipc.client_reply[i] = 0;
    }

    uint64_t reaped_base = sched_reaped_count();

    if (thread_create(phase4_ipc.server_task, phase4_server_entry,
                      &phase4_ipc) == NULL ||
        thread_create(phase4_ipc.client_task, phase4_client_entry,
                      &phase4_ipc) == NULL) {
        kpanic("phase4: cannot create IPC worker threads");
    }

    uint64_t deadline = sched_tick_count() + 2000;
    while (!phase4_ipc.server_done || !phase4_ipc.client_done) {
        if (sched_tick_count() > deadline) {
            kpanic("phase4: cross-task IPC timed out");
        }
        thread_sleep_ms(1);
    }

    wait_for_reaped(reaped_base + 2);

    if (phase4_ipc.server_status != SYS_OK) {
        kpanic("phase4: server IPC failed (%ld)", phase4_ipc.server_status);
    }
    if (phase4_ipc.client_status != SYS_OK) {
        kpanic("phase4: client IPC failed (%ld)", phase4_ipc.client_status);
    }
    if (phase4_ipc.rights_status != SYS_ERR_CAP_RIGHTS) {
        kpanic("phase4: READ-only cap was not rejected for CALL (%ld)",
               phase4_ipc.rights_status);
    }
    if (phase4_ipc.server_seen_badge != PHASE4_CLIENT_BADGE) {
        kpanic("phase4: server saw badge 0x%016lx", phase4_ipc.server_seen_badge);
    }
    if (phase4_ipc.client_reply_badge != PHASE4_CLIENT_BADGE) {
        kpanic("phase4: client reply badge 0x%016lx",
               phase4_ipc.client_reply_badge);
    }

    for (size_t i = 0; i < 4; i++) {
        if (phase4_ipc.server_msg[i] != phase4_ipc.client_msg[i]) {
            kpanic("phase4: message word %lu mismatch", i);
        }
        if (phase4_ipc.client_reply[i] != phase4_ipc.reply_expected[i]) {
            kpanic("phase4: reply word %lu mismatch", i);
        }
    }
}

static void phase4_run_ipc(void)
{
    memset(&phase4_ipc, 0, sizeof(phase4_ipc));

    phase4_ipc.client_msg[0] = 0x1122334455667788ULL;
    phase4_ipc.client_msg[1] = 0x99AABBCCDDEEFF00ULL;
    phase4_ipc.client_msg[2] = 0x0123456789ABCDEFULL;
    phase4_ipc.client_msg[3] = 0xFEDCBA9876543210ULL;
    for (size_t i = 0; i < 4; i++) {
        phase4_ipc.reply_expected[i] = ~phase4_ipc.client_msg[i];
    }

    task_t *server = task_create();
    task_t *client = task_create();
    if (server == NULL || client == NULL) {
        kpanic("phase4: task_create failed");
    }

    endpoint_t *endpoint = endpoint_create();
    if (endpoint == NULL) {
        kpanic("phase4: endpoint_create failed");
    }

    cap_t server_cap = CAP_NULL;
    cap_t derived_cap = CAP_NULL;
    cap_t client_cap = CAP_NULL;
    cap_t readonly_cap = CAP_NULL;

    sys_status_t status;
    status = cap_insert(server->caps, CAP_OBJ_ENDPOINT, endpoint,
                        CAP_RIGHT_ALL, PHASE4_SERVER_BADGE, &server_cap);
    if (status != SYS_OK) {
        kpanic("phase4: cap_insert failed (%ld)", status);
    }

    status = cap_derive(server->caps, server_cap, CAP_RIGHT_ALL,
                        PHASE4_CLIENT_BADGE, &derived_cap);
    if (status != SYS_OK) {
        kpanic("phase4: cap_derive failed (%ld)", status);
    }

    status = cap_grant(server->caps, derived_cap, client->caps, &client_cap);
    if (status != SYS_OK) {
        kpanic("phase4: cap_grant failed (%ld)", status);
    }

    status = cap_delete(server->caps, derived_cap);
    if (status != SYS_OK) {
        kpanic("phase4: cap_delete failed (%ld)", status);
    }

    status = cap_derive(client->caps, client_cap, CAP_RIGHT_READ,
                        PHASE4_READONLY_BADGE, &readonly_cap);
    if (status != SYS_OK) {
        kpanic("phase4: read-only derivation failed (%ld)", status);
    }

    /* Drop the creator reference without marking the endpoint destroyed; the
     * capability tables now own the object. */
    mk_object_release(&endpoint->header);

    phase4_ipc.client_task = client;
    phase4_ipc.server_task = server;
    phase4_ipc.server_cap = server_cap;
    phase4_ipc.client_cap = client_cap;
    phase4_ipc.readonly_cap = readonly_cap;

    phase4_ipc_round(false);
    phase4_ipc_round(true);

    void *object = NULL;
    uint64_t badge = 0;

    status = cap_lookup(server->caps, server_cap, CAP_OBJ_ENDPOINT,
                        CAP_RIGHT_CALL, &object, &badge);
    if (status != SYS_OK || badge != PHASE4_SERVER_BADGE) {
        kpanic("phase4: server badge changed or lookup failed");
    }
    status = cap_lookup(client->caps, client_cap, CAP_OBJ_ENDPOINT,
                        CAP_RIGHT_CALL, &object, &badge);
    if (status != SYS_OK || badge != PHASE4_CLIENT_BADGE) {
        kpanic("phase4: client badge changed or lookup failed");
    }
    status = cap_lookup(client->caps, readonly_cap, CAP_OBJ_ENDPOINT,
                        CAP_RIGHT_READ, &object, &badge);
    if (status != SYS_OK || badge != PHASE4_READONLY_BADGE) {
        kpanic("phase4: read-only badge changed or lookup failed");
    }
    status = cap_lookup(client->caps, readonly_cap, CAP_OBJ_ENDPOINT,
                        CAP_RIGHT_CALL, &object, &badge);
    if (status != SYS_ERR_CAP_RIGHTS) {
        kpanic("phase4: read-only cap unexpectedly grants CALL");
    }
    status = cap_lookup(client->caps, CAP_NULL, CAP_OBJ_ENDPOINT,
                        0, &object, &badge);
    if (status != SYS_ERR_CAP_INVALID) {
        kpanic("phase4: CAP_NULL was accepted");
    }

    task_destroy(client);
    task_destroy(server);

    klog(KLOG_INFO,
         "phase4: capability rights/badges and cross-task IPC verified (fast+slow)");
}

typedef struct phase4_notify_ctx {
    task_t *task;
    cap_t cap;
    notification_t *notification;
    sys_status_t waiter_status;
    uint64_t waiter_bits;
    volatile bool waiter_done;
    volatile bool signaler_done;
} phase4_notify_ctx_t;

static phase4_notify_ctx_t phase4_notify;

static void phase4_notify_waiter_entry(void *arg)
{
    phase4_notify_ctx_t *ctx = (phase4_notify_ctx_t *)arg;
    uint64_t bits = 0;

    sys_status_t status = notify_wait(ctx->task, ctx->cap, &bits);
    ctx->waiter_status = status;
    ctx->waiter_bits = bits;
    ctx->waiter_done = true;
    thread_exit();
}

static void phase4_notify_signaler_entry(void *arg)
{
    phase4_notify_ctx_t *ctx = (phase4_notify_ctx_t *)arg;

    thread_sleep_ms(20);
    notify_signal(ctx->notification, PHASE4_NOTIFY_BITS);
    ctx->signaler_done = true;
    thread_exit();
}

static void phase4_notify_reset(void)
{
    phase4_notify.waiter_status = SYS_ERR_BUSY;
    phase4_notify.waiter_bits = 0;
    phase4_notify.waiter_done = false;
    phase4_notify.signaler_done = false;
}

static thread_t *phase4_spawn_waiter(void)
{
    return thread_create(phase4_notify.task, phase4_notify_waiter_entry,
                         &phase4_notify);
}

static void phase4_wait_waiter_blocked(thread_t *waiter)
{
    uint64_t deadline = sched_tick_count() + 500;

    while (waiter->state != THREAD_BLOCKED_NOTIFY) {
        if (sched_tick_count() > deadline) {
            kpanic("phase4: notification waiter did not block");
        }
        thread_sleep_ms(1);
    }
}

static void phase4_wait_for_flag(volatile bool *flag, const char *what)
{
    uint64_t deadline = sched_tick_count() + 500;

    while (!*flag) {
        if (sched_tick_count() > deadline) {
            kpanic("phase4: timeout waiting for %s", what);
        }
        thread_sleep_ms(1);
    }
}

static void phase4_run_notifications(void)
{
    task_t *task = task_create();
    if (task == NULL) {
        kpanic("phase4: notification task_create failed");
    }

    notification_t *notification = notification_create();
    if (notification == NULL) {
        kpanic("phase4: notification_create failed");
    }

    cap_t ntf_cap = CAP_NULL;
    sys_status_t status = cap_insert(task->caps, CAP_OBJ_NOTIFICATION,
                                     notification, CAP_RIGHT_READ, 0, &ntf_cap);
    if (status != SYS_OK) {
        kpanic("phase4: notification cap_insert failed (%ld)", status);
    }
    /* Drop the creator reference; the capability table owns the object. */
    mk_object_release(&notification->header);

    memset(&phase4_notify, 0, sizeof(phase4_notify));
    phase4_notify.task = task;
    phase4_notify.cap = ntf_cap;
    phase4_notify.notification = notification;

    /* (1) pre-signaled, non-blocking. */
    phase4_notify_reset();
    notify_signal(notification, PHASE4_NOTIFY_BITS);

    uint64_t reaped_base = sched_reaped_count();
    thread_t *waiter = phase4_spawn_waiter();
    if (waiter == NULL) {
        kpanic("phase4: cannot create notification waiter");
    }
    phase4_wait_for_flag(&phase4_notify.waiter_done, "pre-signaled waiter");
    wait_for_reaped(reaped_base + 1);
    if (phase4_notify.waiter_status != SYS_OK ||
        phase4_notify.waiter_bits != PHASE4_NOTIFY_BITS) {
        kpanic("phase4: pre-signaled notification mismatch");
    }

    /* (2) blocking waiter woken by another thread. */
    phase4_notify_reset();
    reaped_base = sched_reaped_count();
    waiter = phase4_spawn_waiter();
    if (waiter == NULL) {
        kpanic("phase4: cannot create notification waiter");
    }
    phase4_wait_waiter_blocked(waiter);

    thread_t *signaler = thread_create(task, phase4_notify_signaler_entry,
                                       &phase4_notify);
    if (signaler == NULL) {
        kpanic("phase4: cannot create notification signaler");
    }
    phase4_wait_for_flag(&phase4_notify.waiter_done, "thread-woken waiter");
    phase4_wait_for_flag(&phase4_notify.signaler_done, "notification signaler");
    wait_for_reaped(reaped_base + 2);
    if (phase4_notify.waiter_status != SYS_OK ||
        phase4_notify.waiter_bits != PHASE4_NOTIFY_BITS) {
        kpanic("phase4: thread-woken notification mismatch");
    }

    /* (3) blocking waiter woken by a software-triggered external IRQ. */
    status = irq_bind_notification(PHASE4_IRQ_VECTOR, notification,
                                   PHASE4_IRQ_BIT);
    if (status != SYS_OK) {
        kpanic("phase4: irq_bind_notification failed (%ld)", status);
    }

    cap_table_t *objects = cap_table_create();
    if (objects == NULL) {
        kpanic("phase4: cannot create object cap table");
    }

    mk_object_t *irq_object = cap_irq_object_create(PHASE4_IRQ_VECTOR);
    if (irq_object == NULL) {
        kpanic("phase4: cannot create IRQ capability object");
    }
    cap_t irq_cap = CAP_NULL;
    status = cap_insert(objects, CAP_OBJ_IRQ, irq_object, CAP_RIGHT_READ,
                        0, &irq_cap);
    if (status != SYS_OK) {
        kpanic("phase4: IRQ cap_insert failed (%ld)", status);
    }
    mk_object_release(irq_object);

    paddr_t frame = pmm_alloc_frame();
    if (frame == 0) {
        kpanic("phase4: cannot allocate test frame");
    }
    mk_object_t *frame_object = cap_frame_object_create(frame);
    if (frame_object == NULL) {
        kpanic("phase4: cannot create frame capability object");
    }
    cap_t frame_cap = CAP_NULL;
    status = cap_insert(objects, CAP_OBJ_FRAME, frame_object, CAP_RIGHT_READ,
                        0, &frame_cap);
    if (status != SYS_OK) {
        kpanic("phase4: frame cap_insert failed (%ld)", status);
    }
    mk_object_release(frame_object);

    vspace_t *vspace = vspace_create();
    if (vspace == NULL) {
        kpanic("phase4: cannot create test vspace");
    }
    mk_object_t *vspace_object = cap_vspace_object_create(vspace);
    if (vspace_object == NULL) {
        kpanic("phase4: cannot create vspace capability object");
    }
    cap_t vspace_cap = CAP_NULL;
    status = cap_insert(objects, CAP_OBJ_VSPACE, vspace_object, CAP_RIGHT_READ,
                        0, &vspace_cap);
    if (status != SYS_OK) {
        kpanic("phase4: vspace cap_insert failed (%ld)", status);
    }
    mk_object_release(vspace_object);
    vspace_destroy(vspace);

    phase4_notify_reset();
    reaped_base = sched_reaped_count();
    waiter = phase4_spawn_waiter();
    if (waiter == NULL) {
        kpanic("phase4: cannot create IRQ waiter");
    }
    phase4_wait_waiter_blocked(waiter);

    __asm__ volatile("int $0x30" : : : "memory");

    phase4_wait_for_flag(&phase4_notify.waiter_done, "IRQ-woken waiter");
    wait_for_reaped(reaped_base + 1);
    if (phase4_notify.waiter_status != SYS_OK ||
        phase4_notify.waiter_bits != PHASE4_IRQ_BIT) {
        kpanic("phase4: IRQ notification mismatch");
    }

    cap_table_destroy(objects);
    task_destroy(task);

    klog(KLOG_INFO,
         "phase4: notifications (presignaled/thread/IRQ) and object teardown verified");
}

static void run_phase4_tests(void)
{
    size_t frames_before = pmm_free_frame_count();

    phase4_run_ipc();
    phase4_run_notifications();

    size_t frames_after = pmm_free_frame_count();
    if (frames_after != frames_before) {
        kpanic("phase4: PMM leak detected (%lu -> %lu free frames)",
               frames_before, frames_after);
    }
    if (mk_object_live_count() != 0) {
        kpanic("phase4: leaked %lu kernel objects",
               mk_object_live_count());
    }

    klog(KLOG_INFO,
         "phase4: capability/IPC/notification self-tests passed, 0 leaked PMM frames");
}

/* ------------------------------------------------------------------------- */
/* Phase 5 Ring 3, syscall ABI, ELF loader and personality hooks              */
/* ------------------------------------------------------------------------- */

#define PHASE5_USER_BADGE        0x5555555555555555ULL
#define PHASE5_SERVICE_BADGE     0x6666666666666666ULL
#define PHASE5_PERSONALITY_TEST  0x1234ULL
#define PHASE5_PERSONALITY_FAULT 0x1001ULL

typedef struct phase5_service_ctx {
    task_t *task;
    cap_t cap;
    uint64_t seen_msg[4];
    uint64_t seen_badge;
    sys_status_t status;
    volatile bool done;
} phase5_service_ctx_t;

static phase5_service_ctx_t phase5_service;
static task_t *phase5_user_task;
static task_t *phase5_fault_task;
static const struct limine_file *phase5_init_module;
static const struct limine_file *phase5_fault_module;
static volatile bool phase5_user_exited;
static volatile bool phase5_fault_seen;
static volatile bool phase5_fault_exited;
static int64_t phase5_user_status;

static bool phase5_path_ends_with(const char *path, const char *suffix)
{
    if (path == NULL) {
        return false;
    }

    size_t path_len = strlen(path);
    size_t suffix_len = strlen(suffix);

    if (suffix_len > path_len) {
        return false;
    }

    return strcmp(path + (path_len - suffix_len), suffix) == 0;
}

static const struct limine_file *phase5_find_module(const char *suffix)
{
    struct limine_module_response *response =
        (struct limine_module_response *)module_request.response;

    if (response == NULL) {
        return NULL;
    }

    for (uint64_t i = 0; i < response->module_count; i++) {
        struct limine_file *file = response->modules[i];

        if (file != NULL && phase5_path_ends_with(file->path, suffix)) {
            return file;
        }
    }

    return NULL;
}

static void phase5_user_thread_entry(void *arg)
{
    enter_usermode((uint64_t)(uintptr_t)arg, ELF_USER_STACK_TOP);
}

static void phase5_service_entry(void *arg)
{
    phase5_service_ctx_t *ctx = (phase5_service_ctx_t *)arg;
    thread_t *sender = NULL;
    uint64_t badge = 0;
    uint64_t msg[4] = { 0, 0, 0, 0 };

    sys_status_t status = ipc_recv(ctx->task, ctx->cap, msg, &badge, &sender);

    if (status == SYS_OK) {
        for (size_t i = 0; i < 4; i++) {
            ctx->seen_msg[i] = msg[i];
        }
        ctx->seen_badge = badge;

        uint64_t reply[4] = { ~msg[0], ~msg[1], ~msg[2], ~msg[3] };
        ctx->status = ipc_reply(sender, reply);
    } else {
        ctx->status = status;
    }

    ctx->done = true;
    thread_exit();
}

static task_t *phase5_spawn_user(const struct limine_file *file,
                                 endpoint_t *endpoint, task_t **task_slot,
                                 cap_t *user_cap_out, bool with_frame_cap)
{
    if (file == NULL || task_slot == NULL) {
        return NULL;
    }

    task_t *task = task_create();
    if (task == NULL) {
        return NULL;
    }

    uint64_t entry = 0;
    sys_status_t status = elf64_load(task->vspace, file->address,
                                     (size_t)file->size, &entry);
    if (status != SYS_OK) {
        task_destroy(task);
        return NULL;
    }

    if (endpoint != NULL) {
        cap_t cap = CAP_NULL;

        status = cap_insert(task->caps, CAP_OBJ_ENDPOINT, endpoint,
                            CAP_RIGHT_CALL, PHASE5_USER_BADGE, &cap);
        if (status != SYS_OK || cap != 1) {
            klog(KLOG_ERROR, "phase5: user endpoint cap setup failed (handle %lu)",
                 (uint64_t)cap);
            task_destroy(task);
            return NULL;
        }

        if (user_cap_out != NULL) {
            *user_cap_out = cap;
        }
    }

    if (with_frame_cap) {
        paddr_t frame_phys = pmm_alloc_frame();
        if (frame_phys == 0) {
            task_destroy(task);
            return NULL;
        }

        mk_object_t *frame_object = cap_frame_object_create(frame_phys);
        if (frame_object == NULL) {
            pmm_free_frame(frame_phys);
            task_destroy(task);
            return NULL;
        }

        cap_t frame_cap = CAP_NULL;
        status = cap_insert(task->caps, CAP_OBJ_FRAME, frame_object,
                            CAP_RIGHT_READ | CAP_RIGHT_WRITE, 0,
                            &frame_cap);
        if (status != SYS_OK || frame_cap != 2) {
            klog(KLOG_ERROR, "phase5: frame cap setup failed (handle %lu)",
                 (uint64_t)frame_cap);
            mk_object_release(frame_object);
            task_destroy(task);
            return NULL;
        }

        mk_object_release(frame_object);
    }

    /* Publish the task before its thread can run and fault. */
    *task_slot = task;

    if (thread_create(task, phase5_user_thread_entry,
                      (void *)(uintptr_t)entry) == NULL) {
        *task_slot = NULL;
        task_destroy(task);
        return NULL;
    }

    return task;
}

static sys_status_t phase5_spawn_fault(void)
{
    if (phase5_fault_task != NULL) {
        return SYS_ERR_BUSY;
    }
    if (phase5_fault_module == NULL) {
        return SYS_ERR_NOSYS;
    }

    task_t *task = phase5_spawn_user(phase5_fault_module, NULL,
                                     &phase5_fault_task, NULL, false);

    return task != NULL ? SYS_OK : SYS_ERR_NOMEM;
}

static void phase5_on_personality(thread_t *caller, syscall_frame_t *frame)
{
    UNUSED(caller);

    if (frame->rax == PHASE5_PERSONALITY_TEST) {
        klog(KLOG_INFO, "phase5: personality syscall from Ring 3 (args 0x%lx, 0x%lx)",
             frame->rdi, frame->rsi);
        frame->rax = frame->rdi + frame->rsi;
        return;
    }

    if (frame->rax == PHASE5_PERSONALITY_FAULT) {
        frame->rax = (uint64_t)phase5_spawn_fault();
        return;
    }

    frame->rax = (uint64_t)SYS_ERR_NOSYS;
}

static bool phase5_on_user_exception(thread_t *thread, uint64_t vector,
                                     uint64_t error, uint64_t fault_addr)
{
    bool fault_task =
        (thread != NULL && thread->owner_task == phase5_fault_task);

    klog(KLOG_WARN,
         "phase5: user exception vector=%lu error=0x%lx addr=0x%lx%s",
         vector, error, fault_addr, fault_task ? " (fault-test task)" : "");

    if (fault_task && vector == 14 && fault_addr == 0) {
        phase5_fault_seen = true;
    }

    return false;
}

static void phase5_on_task_exit(task_t *task, int64_t exit_status)
{
    if (task == phase5_fault_task) {
        phase5_fault_exited = true;
        klog(KLOG_INFO, "phase5: fault task terminated (status %ld)",
             exit_status);
    } else if (task == phase5_user_task) {
        phase5_user_exited = true;
        phase5_user_status = exit_status;
        klog(KLOG_INFO, "phase5: Ring 3 init_test exited (status %ld)",
             exit_status);
    }
}

static const mk_hooks_t phase5_hooks = {
    .on_personality_syscall = phase5_on_personality,
    .on_user_exception = phase5_on_user_exception,
    .on_task_exit = phase5_on_task_exit,
};

static void phase5_run_cycle(void)
{
    memset(&phase5_service, 0, sizeof(phase5_service));
    phase5_user_task = NULL;
    phase5_fault_task = NULL;
    phase5_user_exited = false;
    phase5_fault_seen = false;
    phase5_fault_exited = false;
    phase5_user_status = -1;

    uint64_t reaped_base = sched_reaped_count();

    endpoint_t *endpoint = endpoint_create();
    if (endpoint == NULL) {
        kpanic("phase5: endpoint_create failed");
    }

    task_t *service_task = task_create();
    if (service_task == NULL) {
        kpanic("phase5: service task_create failed");
    }

    cap_t service_cap = CAP_NULL;
    if (cap_insert(service_task->caps, CAP_OBJ_ENDPOINT, endpoint,
                   CAP_RIGHT_READ, PHASE5_SERVICE_BADGE, &service_cap) != SYS_OK) {
        kpanic("phase5: service cap_insert failed");
    }

    phase5_service.task = service_task;
    phase5_service.cap = service_cap;

    if (thread_create(service_task, phase5_service_entry,
                      &phase5_service) == NULL) {
        kpanic("phase5: service thread_create failed");
    }

    cap_t user_cap = CAP_NULL;
    if (phase5_spawn_user(phase5_init_module, endpoint, &phase5_user_task,
                          &user_cap, true) == NULL) {
        kpanic("phase5: user task spawn failed");
    }
    if (user_cap != 1) {
        kpanic("phase5: user endpoint cap handle is %lu", (uint64_t)user_cap);
    }

    mk_object_release(&endpoint->header);

    uint64_t deadline = sched_tick_count() + 5000;
    while (!phase5_user_exited) {
        if (sched_tick_count() > deadline) {
            kpanic("phase5: user task timed out");
        }
        thread_sleep_ms(2);
    }

    deadline = sched_tick_count() + 5000;
    while (!phase5_service.done) {
        if (sched_tick_count() > deadline) {
            kpanic("phase5: service task timed out");
        }
        thread_sleep_ms(2);
    }

    deadline = sched_tick_count() + 5000;
    while (phase5_fault_task == NULL) {
        if (sched_tick_count() > deadline) {
            kpanic("phase5: fault task was not spawned");
        }
        thread_sleep_ms(2);
    }

    deadline = sched_tick_count() + 5000;
    while (!phase5_fault_exited) {
        if (sched_tick_count() > deadline) {
            kpanic("phase5: fault task did not terminate");
        }
        thread_sleep_ms(2);
    }

    wait_for_reaped(reaped_base + 3);

    if (phase5_user_status != 0) {
        kpanic("phase5: Ring 3 test exit status %ld",
               (long)phase5_user_status);
    }
    if (!phase5_fault_seen) {
        kpanic("phase5: user exception hook was not invoked");
    }
    if (phase5_service.status != SYS_OK) {
        kpanic("phase5: service reply failed (%ld)", phase5_service.status);
    }
    if (phase5_service.seen_badge != PHASE5_USER_BADGE) {
        kpanic("phase5: service saw badge 0x%016lx",
               phase5_service.seen_badge);
    }

    uint64_t expected_msg[4] = {
        0x1122334455667788ULL,
        0x99AABBCCDDEEFF00ULL,
        0x0123456789ABCDEFULL,
        0xFEDCBA9876543210ULL
    };
    for (size_t i = 0; i < 4; i++) {
        if (phase5_service.seen_msg[i] != expected_msg[i]) {
            kpanic("phase5: service message word %lu mismatch", i);
        }
    }

    task_destroy(phase5_fault_task);
    task_destroy(service_task);
    task_destroy(phase5_user_task);

    phase5_fault_task = NULL;
    phase5_user_task = NULL;
    phase5_service.task = NULL;
    phase5_service.cap = CAP_NULL;
}

static void phase5_victim_recv(void *arg)
{
    UNUSED(arg);
    thread_t *self = sched_current_thread();
    thread_t *sender = NULL;
    uint64_t msg[4] = { 0, 0, 0, 0 };
    uint64_t badge = 0;

    (void)ipc_recv(self->owner_task, (cap_t)1, msg, &badge, &sender);
    thread_exit();
}

static void phase5_victim_send(void *arg)
{
    UNUSED(arg);
    thread_t *self = sched_current_thread();
    uint64_t msg[4] = { 1, 2, 3, 4 };
    uint64_t reply[4] = { 0, 0, 0, 0 };
    uint64_t badge = 0;

    (void)ipc_call(self->owner_task, (cap_t)2, msg, reply, &badge);
    thread_exit();
}

static void phase5_victim_notify(void *arg)
{
    UNUSED(arg);
    thread_t *self = sched_current_thread();
    uint64_t bits = 0;

    (void)notify_wait(self->owner_task, (cap_t)3, &bits);
    thread_exit();
}

static void phase5_victim_killer(void *arg)
{
    UNUSED(arg);
    thread_sleep_ms(50);
    task_terminate();
}

static void phase5_run_termination_test(void)
{
    size_t frames_before = pmm_free_frame_count();
    uint64_t reaped_base = sched_reaped_count();

    endpoint_t *recv_endpoint = endpoint_create();
    endpoint_t *send_endpoint = endpoint_create();
    notification_t *notification = notification_create();
    task_t *victim = task_create();

    if (recv_endpoint == NULL || send_endpoint == NULL ||
        notification == NULL || victim == NULL) {
        kpanic("phase5: termination test allocation failed");
    }

    cap_t recv_cap = CAP_NULL;
    cap_t send_cap = CAP_NULL;
    cap_t notification_cap = CAP_NULL;

    if (cap_insert(victim->caps, CAP_OBJ_ENDPOINT, recv_endpoint,
                   CAP_RIGHT_READ, 0, &recv_cap) != SYS_OK ||
        cap_insert(victim->caps, CAP_OBJ_ENDPOINT, send_endpoint,
                   CAP_RIGHT_CALL, 0, &send_cap) != SYS_OK ||
        cap_insert(victim->caps, CAP_OBJ_NOTIFICATION, notification,
                   CAP_RIGHT_READ, 0, &notification_cap) != SYS_OK) {
        kpanic("phase5: termination test cap_insert failed");
    }
    if (recv_cap != 1 || send_cap != 2 || notification_cap != 3) {
        kpanic("phase5: termination test cap handles are wrong");
    }

    mk_object_release(&recv_endpoint->header);
    mk_object_release(&send_endpoint->header);
    mk_object_release(&notification->header);

    if (thread_create(victim, phase5_victim_recv, NULL) == NULL ||
        thread_create(victim, phase5_victim_send, NULL) == NULL ||
        thread_create(victim, phase5_victim_notify, NULL) == NULL ||
        thread_create(victim, phase5_victim_killer, NULL) == NULL) {
        kpanic("phase5: termination test thread_create failed");
    }

    uint64_t deadline = sched_tick_count() + 2000;
    while (victim->thread_count != 0) {
        if (sched_tick_count() > deadline) {
            kpanic("phase5: victim task did not terminate");
        }
        thread_sleep_ms(1);
    }

    wait_for_reaped(reaped_base + 4);
    task_destroy(victim);

    if (pmm_free_frame_count() != frames_before) {
        kpanic("phase5: termination test frame leak (%lu -> %lu)",
               frames_before, pmm_free_frame_count());
    }
    if (mk_object_live_count() != 0) {
        kpanic("phase5: termination test leaked %lu objects",
               mk_object_live_count());
    }

    klog(KLOG_INFO,
         "phase5: multi-thread termination and wait-queue cleanup verified");
}

static volatile bool phase5_reply_done;
static sys_status_t phase5_reply_status;

static void phase5_reply_sender(void *arg)
{
    UNUSED(arg);
    thread_t *self = sched_current_thread();
    uint64_t msg[4] = { 0xAA, 0xBB, 0xCC, 0xDD };
    uint64_t reply[4] = { 0, 0, 0, 0 };
    uint64_t badge = 0;

    (void)ipc_call(self->owner_task, (cap_t)1, msg, reply, &badge);
    for (;;) {
        thread_sleep_ms(100);
    }
}

static void phase5_reply_receiver(void *arg)
{
    UNUSED(arg);
    thread_t *self = sched_current_thread();
    thread_t *sender = NULL;
    uint64_t msg[4] = { 0, 0, 0, 0 };
    uint64_t badge = 0;
    uint64_t reply[4] = { 0, 0, 0, 0 };

    sys_status_t status = ipc_recv(self->owner_task, (cap_t)1, msg, &badge,
                                    &sender);
    if (status == SYS_OK) {
        thread_sleep_ms(100);
        phase5_reply_status = ipc_reply(sender, reply);
    } else {
        phase5_reply_status = status;
    }

    phase5_reply_done = true;
    thread_exit();
}

static void phase5_reply_killer(void *arg)
{
    UNUSED(arg);
    thread_sleep_ms(60);
    task_terminate();
}

static void phase5_run_reply_termination_test(void)
{
    size_t frames_before = pmm_free_frame_count();
    uint64_t reaped_base = sched_reaped_count();

    endpoint_t *endpoint = endpoint_create();
    task_t *sender_task = task_create();
    task_t *receiver_task = task_create();

    if (endpoint == NULL || sender_task == NULL || receiver_task == NULL) {
        kpanic("phase5: reply termination allocation failed");
    }

    cap_t sender_cap = CAP_NULL;
    cap_t receiver_cap = CAP_NULL;

    if (cap_insert(sender_task->caps, CAP_OBJ_ENDPOINT, endpoint,
                   CAP_RIGHT_CALL, 0, &sender_cap) != SYS_OK ||
        cap_insert(receiver_task->caps, CAP_OBJ_ENDPOINT, endpoint,
                   CAP_RIGHT_READ, 0, &receiver_cap) != SYS_OK) {
        kpanic("phase5: reply termination cap_insert failed");
    }

    mk_object_release(&endpoint->header);

    phase5_reply_done = false;
    phase5_reply_status = SYS_ERR_BUSY;

    if (thread_create(sender_task, phase5_reply_sender, NULL) == NULL ||
        thread_create(sender_task, phase5_reply_killer, NULL) == NULL ||
        thread_create(receiver_task, phase5_reply_receiver, NULL) == NULL) {
        kpanic("phase5: reply termination thread_create failed");
    }

    uint64_t deadline = sched_tick_count() + 3000;
    while (sender_task->thread_count != 0 || !phase5_reply_done) {
        if (sched_tick_count() > deadline) {
            kpanic("phase5: reply termination timed out");
        }
        thread_sleep_ms(2);
    }

    wait_for_reaped(reaped_base + 3);

    if (phase5_reply_status != SYS_ERR_INVAL) {
        kpanic("phase5: stale reply was not rejected (%ld)",
               phase5_reply_status);
    }

    task_destroy(sender_task);
    task_destroy(receiver_task);

    if (pmm_free_frame_count() != frames_before) {
        kpanic("phase5: reply termination frame leak (%lu -> %lu)",
               frames_before, pmm_free_frame_count());
    }
    if (mk_object_live_count() != 0) {
        kpanic("phase5: reply termination leaked %lu objects",
               mk_object_live_count());
    }

    klog(KLOG_INFO,
         "phase5: blocked-reply termination and stale reply rejection verified");
}

static volatile bool phase5_destroy_recv_done;
static volatile bool phase5_destroy_notify_done;
static sys_status_t phase5_destroy_recv_status;
static sys_status_t phase5_destroy_notify_status;

static void phase5_destroy_recv(void *arg)
{
    UNUSED(arg);
    thread_t *self = sched_current_thread();
    thread_t *sender = NULL;
    uint64_t msg[4] = { 0, 0, 0, 0 };
    uint64_t badge = 0;

    phase5_destroy_recv_status =
        ipc_recv(self->owner_task, (cap_t)1, msg, &badge, &sender);
    phase5_destroy_recv_done = true;
    thread_exit();
}

static void phase5_destroy_notify(void *arg)
{
    UNUSED(arg);
    thread_t *self = sched_current_thread();
    uint64_t bits = 0;

    phase5_destroy_notify_status =
        notify_wait(self->owner_task, (cap_t)2, &bits);
    phase5_destroy_notify_done = true;
    thread_exit();
}

static void phase5_run_destroy_wake_test(void)
{
    size_t frames_before = pmm_free_frame_count();
    uint64_t reaped_base = sched_reaped_count();

    task_t *task = task_create();
    endpoint_t *endpoint = endpoint_create();
    notification_t *notification = notification_create();

    if (task == NULL || endpoint == NULL || notification == NULL) {
        kpanic("phase5: destroy-wake allocation failed");
    }

    cap_t endpoint_cap = CAP_NULL;
    cap_t notification_cap = CAP_NULL;

    if (cap_insert(task->caps, CAP_OBJ_ENDPOINT, endpoint,
                   CAP_RIGHT_READ, 0, &endpoint_cap) != SYS_OK ||
        cap_insert(task->caps, CAP_OBJ_NOTIFICATION, notification,
                   CAP_RIGHT_READ, 0, &notification_cap) != SYS_OK) {
        kpanic("phase5: destroy-wake cap_insert failed");
    }

    phase5_destroy_recv_done = false;
    phase5_destroy_notify_done = false;
    phase5_destroy_recv_status = SYS_ERR_BUSY;
    phase5_destroy_notify_status = SYS_ERR_BUSY;

    thread_t *receiver = thread_create(task, phase5_destroy_recv, NULL);
    thread_t *waiter = thread_create(task, phase5_destroy_notify, NULL);

    if (receiver == NULL || waiter == NULL) {
        kpanic("phase5: destroy-wake thread_create failed");
    }

    uint64_t deadline = sched_tick_count() + 500;
    while (receiver->state != THREAD_BLOCKED_RECV ||
           waiter->state != THREAD_BLOCKED_NOTIFY) {
        if (sched_tick_count() > deadline) {
            kpanic("phase5: destroy-wake peers did not block");
        }
        thread_sleep_ms(1);
    }

    endpoint_destroy(endpoint);
    notification_destroy(notification);

    deadline = sched_tick_count() + 500;
    while (!phase5_destroy_recv_done || !phase5_destroy_notify_done) {
        if (sched_tick_count() > deadline) {
            kpanic("phase5: destroy-wake peers did not return");
        }
        thread_sleep_ms(1);
    }

    wait_for_reaped(reaped_base + 2);
    task_destroy(task);

    if (phase5_destroy_recv_status != SYS_ERR_DEAD ||
        phase5_destroy_notify_status != SYS_ERR_DEAD) {
        kpanic("phase5: destroy-wake peers did not observe SYS_ERR_DEAD");
    }
    if (pmm_free_frame_count() != frames_before) {
        kpanic("phase5: destroy-wake frame leak (%lu -> %lu)",
               frames_before, pmm_free_frame_count());
    }
    if (mk_object_live_count() != 0) {
        kpanic("phase5: destroy-wake leaked %lu objects",
               mk_object_live_count());
    }

    klog(KLOG_INFO,
         "phase5: endpoint/notification destroy woke blocked peers with DEAD");
}

static void run_phase5_tests(void)
{
    phase5_init_module = phase5_find_module("init_test.elf");
    phase5_fault_module = phase5_find_module("fault_test.elf");

    if (phase5_init_module == NULL || phase5_fault_module == NULL) {
        kpanic("phase5: Limine user modules not found");
    }

    klog(KLOG_INFO, "phase5: modules init=%lu bytes, fault=%lu bytes",
         (uint64_t)phase5_init_module->size,
         (uint64_t)phase5_fault_module->size);

    mk_register_hooks(&phase5_hooks);

    phase5_run_cycle();
    size_t baseline_frames = pmm_free_frame_count();

    for (size_t cycle = 0; cycle < 2; cycle++) {
        phase5_run_cycle();

        if (pmm_free_frame_count() != baseline_frames) {
            kpanic("phase5: cycle %lu leaked frames (%lu -> %lu)",
                   cycle + 1, baseline_frames, pmm_free_frame_count());
        }
        if (mk_object_live_count() != 0) {
            kpanic("phase5: cycle %lu leaked %lu objects",
                   cycle + 1, mk_object_live_count());
        }
    }

    phase5_run_termination_test();
    phase5_run_reply_termination_test();
    phase5_run_destroy_wake_test();

    klog(KLOG_INFO,
         "phase5: 3 Ring 3 cycles, ELF loader, IPC and personality hooks verified");
}

/* ------------------------------------------------------------------------- */
/* SMP CoW fork + mmap/munmap stress test                                    */
/* ------------------------------------------------------------------------- */

#define COW_STRESS_TASKS     4U
#define COW_STRESS_ITERS     16U
#define COW_STRESS_PAGES     4U
#define COW_STRESS_BASE_VA   0x0000005000000000ULL
#define COW_STRESS_MMAP_VA   0x0000005800000000ULL
#define COW_STRESS_MMAP_PAGES 2U

typedef struct cow_stress_arg {
    uint32_t slot;
} cow_stress_arg_t;

static cow_stress_arg_t cow_stress_args[COW_STRESS_TASKS];
static task_t *cow_stress_tasks[COW_STRESS_TASKS];
static volatile uint32_t cow_stress_done[COW_STRESS_TASKS];
static volatile uint32_t cow_stress_gate;

/* Each worker simulates concurrent fork()/wait4()/mmap()/munmap() at the
 * microkernel primitive level: vspace_clone (fork CoW), vspace_handle_cow_fault
 * (write after fork), vspace_map_user/vspace_unmap (mmap/munmap), and
 * vspace_destroy (process exit).  All workers run on distinct CPUs under
 * TASK_AFFINITY_ANY, so every vspace_global_lock acquisition, PMM frame
 * allocation, and TLB shootdown happens under real multi-core contention. */
static void cow_stress_worker(void *arg)
{
    cow_stress_arg_t *worker = (cow_stress_arg_t *)arg;

    while (__atomic_load_n(&cow_stress_gate, __ATOMIC_ACQUIRE) == 0) {
        thread_yield();
    }

    /* Each slot uses a disjoint VA range so concurrent workers never alias
     * the same user virtual address in different vspaces. */
    vaddr_t slot_base = COW_STRESS_BASE_VA +
                        (vaddr_t)worker->slot * COW_STRESS_PAGES *
                            PAGE_SIZE * 4;
    vaddr_t slot_mmap = COW_STRESS_MMAP_VA +
                        (vaddr_t)worker->slot * COW_STRESS_MMAP_PAGES *
                            PAGE_SIZE * 4;

    for (uint32_t iter = 0; iter < COW_STRESS_ITERS; iter++) {
        /* ---- Phase 1: build a parent vspace with owned writable pages ---- */
        vspace_t *parent = vspace_create();
        if (parent == NULL) {
            kpanic("cow stress: vspace_create failed (slot %u)", worker->slot);
        }

        for (uint32_t p = 0; p < COW_STRESS_PAGES; p++) {
            paddr_t frame = pmm_alloc_frame();
            if (frame == 0) {
                kpanic("cow stress: frame alloc failed");
            }

            vaddr_t va = slot_base + (vaddr_t)p * PAGE_SIZE;
            uint8_t *page = (uint8_t *)pmm_phys_to_virt(frame);
            page[0] = (uint8_t)(iter + p + worker->slot);

            if (vspace_map_user(parent, va, frame,
                                VSPACE_PROT_USER | VSPACE_PROT_WRITE |
                                    VSPACE_PROT_OWNED) != SYS_OK) {
                kpanic("cow stress: parent map failed");
            }
        }

        /* ---- Phase 2: clone (fork) ---- */
        vspace_t *child = vspace_clone(parent);
        if (child == NULL) {
            kpanic("cow stress: vspace_clone failed");
        }

        /* Verify the clone shared frames via CoW. */
        for (uint32_t p = 0; p < COW_STRESS_PAGES; p++) {
            vaddr_t va = slot_base + (vaddr_t)p * PAGE_SIZE;
            paddr_t parent_phys = 0;
            paddr_t child_phys = 0;

            if (!vspace_translate(parent, va, &parent_phys) ||
                !vspace_translate(child, va, &child_phys) ||
                parent_phys != child_phys) {
                kpanic("cow stress: clone did not share the frame");
            }
        }

        /* ---- Phase 3: resolve CoW on child (simulates child writes) ---- */
        for (uint32_t p = 0; p < COW_STRESS_PAGES; p++) {
            vaddr_t va = slot_base + (vaddr_t)p * PAGE_SIZE;

            if (!vspace_handle_cow_fault(child, va)) {
                kpanic("cow stress: child CoW fault failed");
            }

            paddr_t parent_phys = 0;
            paddr_t child_phys = 0;

            if (!vspace_translate(parent, va, &parent_phys) ||
                !vspace_translate(child, va, &child_phys) ||
                parent_phys == child_phys) {
                kpanic("cow stress: CoW did not isolate the frame");
            }
        }

        /* ---- Phase 4: resolve CoW on parent (simulates parent writes) ---- */
        for (uint32_t p = 0; p < COW_STRESS_PAGES; p++) {
            vaddr_t va = slot_base + (vaddr_t)p * PAGE_SIZE;

            if (!vspace_handle_cow_fault(parent, va)) {
                kpanic("cow stress: parent CoW fault failed");
            }
        }

        /* ---- Phase 5: mmap-style anonymous page churn ---- */
        for (uint32_t m = 0; m < COW_STRESS_MMAP_PAGES; m++) {
            paddr_t frame = pmm_alloc_frame();
            if (frame == 0) {
                kpanic("cow stress: mmap frame alloc failed");
            }

            vaddr_t va = slot_mmap + (vaddr_t)m * PAGE_SIZE;

            if (vspace_map_user(child, va, frame,
                                VSPACE_PROT_USER | VSPACE_PROT_WRITE |
                                    VSPACE_PROT_OWNED) != SYS_OK) {
                kpanic("cow stress: mmap-style map failed");
            }
        }

        /* ---- Phase 6: munmap-style teardown of the anonymous pages ---- */
        for (uint32_t m = 0; m < COW_STRESS_MMAP_PAGES; m++) {
            vaddr_t va = slot_mmap + (vaddr_t)m * PAGE_SIZE;

            if (vspace_unmap(child, va) != SYS_OK) {
                kpanic("cow stress: munmap-style unmap failed");
            }
        }

        /* ---- Phase 7: destroy both (simulates exit + wait4 reap) ---- */
        vspace_destroy(child);
        vspace_destroy(parent);

        if ((iter & 3U) == 0U) {
            thread_yield();
        }
    }

    __atomic_store_n(&cow_stress_done[worker->slot], 1U, __ATOMIC_RELEASE);
    thread_exit();
}

static void run_cow_stress_test(void)
{
    uint32_t online = smp_online_count();
    if (online == 0) {
        online = 1;
    }

    uint32_t workers = online;
    if (workers > COW_STRESS_TASKS) {
        workers = COW_STRESS_TASKS;
    }

    size_t frames_before = pmm_free_frame_count();
    uint64_t reaped_before = sched_reaped_count();

    __atomic_store_n(&cow_stress_gate, 0U, __ATOMIC_RELEASE);

    for (uint32_t i = 0; i < workers; i++) {
        cow_stress_args[i].slot = i;
        cow_stress_done[i] = 0;

        cow_stress_tasks[i] = task_create();
        if (cow_stress_tasks[i] == NULL) {
            kpanic("cow stress: task_create failed");
        }

        task_set_affinity(cow_stress_tasks[i], TASK_AFFINITY_ANY);

        if (thread_create(cow_stress_tasks[i], cow_stress_worker,
                          &cow_stress_args[i]) == NULL) {
            kpanic("cow stress: thread_create failed");
        }
    }

    __atomic_store_n(&cow_stress_gate, 1U, __ATOMIC_RELEASE);

    uint64_t deadline = sched_tick_count() + 10000;

    for (uint32_t i = 0; i < workers; i++) {
        while (__atomic_load_n(&cow_stress_done[i], __ATOMIC_ACQUIRE) == 0) {
            if (sched_tick_count() > deadline) {
                kpanic("cow stress: worker %u timed out", i);
            }
            thread_sleep_ms(2);
        }
    }

    wait_for_reaped(reaped_before + workers);

    for (uint32_t i = 0; i < workers; i++) {
        while (cow_stress_tasks[i]->thread_count != 0) {
            if (sched_tick_count() > deadline) {
                kpanic("cow stress: task %u did not quiesce", i);
            }
            thread_sleep_ms(1);
        }

        task_destroy(cow_stress_tasks[i]);
        cow_stress_tasks[i] = NULL;
    }

    size_t frames_after = pmm_free_frame_count();
    if (frames_after != frames_before) {
        kpanic("cow stress: frame leak (%lu -> %lu free)",
               frames_before, frames_after);
    }

    klog(KLOG_INFO,
         "cow stress: %u workers x %u iterations, concurrent CoW "
         "fork/fault/mmap/munmap, 0 deadlocks, 0 leaked frames (%lu free)",
         workers, (uint32_t)COW_STRESS_ITERS,
         (unsigned long)frames_after);
}

/* ------------------------------------------------------------------------- */
/* BSD personality bootstrap                                                 */
/* ------------------------------------------------------------------------- */

#define BSD_BOOT_MODULE_MAX 8

static bsd_module_t bsd_boot_modules[BSD_BOOT_MODULE_MAX];
static size_t bsd_boot_module_count;

static void collect_bsd_boot_modules(void)
{
    struct limine_module_response *response = module_request.response;

    bsd_boot_module_count = 0;

    if (response == NULL) {
        return;
    }

    for (uint64_t i = 0; i < response->module_count &&
                         bsd_boot_module_count < BSD_BOOT_MODULE_MAX;
         i++) {
        struct limine_file *file = response->modules[i];

        if (file == NULL || file->path == NULL || file->address == NULL) {
            continue;
        }

        const char *name = file->path;

        for (const char *cursor = file->path; *cursor != '\0'; cursor++) {
            if (*cursor == '/') {
                name = cursor + 1;
            }
        }

        bsd_boot_modules[bsd_boot_module_count].name = name;
        bsd_boot_modules[bsd_boot_module_count].address = file->address;
        bsd_boot_modules[bsd_boot_module_count].size = (size_t)file->size;
        bsd_boot_module_count++;
    }
}

static void self_test_virtio_blk(void)
{
    if (!blk_init()) {
        klog(KLOG_WARN, "virtio-blk self-test: device unavailable");
        return;
    }

    static uint8_t superblock[1024];

    if (blk_read_sectors(2, 2, superblock) == SYS_OK &&
        (uint16_t)((uint16_t)superblock[56] |
                   ((uint16_t)superblock[57] << 8)) == 0xEF53U) {
        klog(KLOG_INFO,
             "virtio-blk self-test: ext2 image detected; skipping the "
             "destructive sector test (ext2 tests exercise the device)");
        return;
    }

    enum { SECTORS = 8, TRANSFER_BYTES = SECTORS * 512 };
    static uint8_t pattern[TRANSFER_BYTES];
    static uint8_t readback[TRANSFER_BYTES];

    const uint64_t lba = 2048;

    for (size_t i = 0; i < TRANSFER_BYTES; i++) {
        pattern[i] = (uint8_t)((i * 37U + 0xA5U) ^ (i >> 3));
    }
    memset(readback, 0, sizeof(readback));

    size_t frames_before = pmm_free_frame_count();

    if (blk_write_sectors(lba, SECTORS, pattern) != SYS_OK) {
        kpanic("virtio-blk self-test: write failed");
    }
    if (blk_read_sectors(lba, SECTORS, readback) != SYS_OK) {
        kpanic("virtio-blk self-test: read failed");
    }
    if (memcmp(pattern, readback, TRANSFER_BYTES) != 0) {
        kpanic("virtio-blk self-test: data mismatch");
    }

    memset(pattern, 0, sizeof(pattern));
    if (blk_write_sectors(lba, SECTORS, pattern) != SYS_OK) {
        kpanic("virtio-blk self-test: restore failed");
    }

    if (pmm_free_frame_count() != frames_before) {
        kpanic("virtio-blk self-test: leaked PMM frames");
    }

    uint32_t irq_events = blk_irq_event_count();

    if (irq_events == 0) {
        kpanic("virtio-blk self-test: IOAPIC IRQ was never delivered");
    }

    klog(KLOG_INFO,
         "virtio-blk self-test: 4096 bytes at sector 2048 verified, "
         "%lu IRQ event(s), 0 leaked frames, capacity %lu sectors",
         (uint64_t)irq_events, blk_capacity_sectors());
}

/* ------------------------------------------------------------------------- */
/* Stage E: FPU/SSE context, QoS scheduling, capability handoff              */
/* ------------------------------------------------------------------------- */

/* IEEE-754 binary64 bit pattern of an integer below 2^53, computed with
 * integer arithmetic so the -mno-sse kernel can predict SSE results. */
static uint64_t f64_bits_of(uint64_t value)
{
    if (value == 0) {
        return 0;
    }

    uint32_t exponent = 63U - (uint32_t)__builtin_clzll(value);
    uint64_t mantissa = (value << (52U - exponent)) & ((1ULL << 52) - 1ULL);

    return ((uint64_t)(1023U + exponent) << 52) | mantissa;
}

static void sse_image_fill(uint64_t image[32], uint64_t base)
{
    for (uint64_t lane = 0; lane < 32; lane++) {
        image[lane] = f64_bits_of(base + lane);
    }
}

static bool sse_image_matches(const uint64_t image[32], uint64_t base)
{
    for (uint64_t lane = 0; lane < 32; lane++) {
        if (image[lane] != f64_bits_of(base + lane)) {
            return false;
        }
    }
    return true;
}

/* Runs `iterations` rounds of packed double additions over all sixteen XMM
 * registers. image holds 32 doubles (bit patterns, 16-byte aligned) that are
 * loaded into XMM0-XMM15 first and stored back at the end; every round adds
 * 1.0 to every lane. The whole computation lives in one asm block, so the C
 * code around it (built with -mno-sse) never touches the registers: any value
 * other than start + iterations is corruption by a context switch. */
static void sse_lane_stress(uint64_t image[32], uint64_t iterations)
{
    static const uint64_t one_pd[2] __attribute__((aligned(16))) = {
        0x3FF0000000000000ULL, 0x3FF0000000000000ULL
    };

    if (iterations == 0) {
        return;
    }

    __asm__ volatile(
        "movapd 0(%[img]), %%xmm0\n\t"
        "movapd 16(%[img]), %%xmm1\n\t"
        "movapd 32(%[img]), %%xmm2\n\t"
        "movapd 48(%[img]), %%xmm3\n\t"
        "movapd 64(%[img]), %%xmm4\n\t"
        "movapd 80(%[img]), %%xmm5\n\t"
        "movapd 96(%[img]), %%xmm6\n\t"
        "movapd 112(%[img]), %%xmm7\n\t"
        "movapd 128(%[img]), %%xmm8\n\t"
        "movapd 144(%[img]), %%xmm9\n\t"
        "movapd 160(%[img]), %%xmm10\n\t"
        "movapd 176(%[img]), %%xmm11\n\t"
        "movapd 192(%[img]), %%xmm12\n\t"
        "movapd 208(%[img]), %%xmm13\n\t"
        "movapd 224(%[img]), %%xmm14\n\t"
        "movapd 240(%[img]), %%xmm15\n\t"
        "1:\n\t"
        "addpd (%[one]), %%xmm0\n\t"
        "addpd (%[one]), %%xmm1\n\t"
        "addpd (%[one]), %%xmm2\n\t"
        "addpd (%[one]), %%xmm3\n\t"
        "addpd (%[one]), %%xmm4\n\t"
        "addpd (%[one]), %%xmm5\n\t"
        "addpd (%[one]), %%xmm6\n\t"
        "addpd (%[one]), %%xmm7\n\t"
        "addpd (%[one]), %%xmm8\n\t"
        "addpd (%[one]), %%xmm9\n\t"
        "addpd (%[one]), %%xmm10\n\t"
        "addpd (%[one]), %%xmm11\n\t"
        "addpd (%[one]), %%xmm12\n\t"
        "addpd (%[one]), %%xmm13\n\t"
        "addpd (%[one]), %%xmm14\n\t"
        "addpd (%[one]), %%xmm15\n\t"
        "decq %[n]\n\t"
        "jnz 1b\n\t"
        "movapd %%xmm0, 0(%[img])\n\t"
        "movapd %%xmm1, 16(%[img])\n\t"
        "movapd %%xmm2, 32(%[img])\n\t"
        "movapd %%xmm3, 48(%[img])\n\t"
        "movapd %%xmm4, 64(%[img])\n\t"
        "movapd %%xmm5, 80(%[img])\n\t"
        "movapd %%xmm6, 96(%[img])\n\t"
        "movapd %%xmm7, 112(%[img])\n\t"
        "movapd %%xmm8, 128(%[img])\n\t"
        "movapd %%xmm9, 144(%[img])\n\t"
        "movapd %%xmm10, 160(%[img])\n\t"
        "movapd %%xmm11, 176(%[img])\n\t"
        "movapd %%xmm12, 192(%[img])\n\t"
        "movapd %%xmm13, 208(%[img])\n\t"
        "movapd %%xmm14, 224(%[img])\n\t"
        "movapd %%xmm15, 240(%[img])\n\t"
        : [n] "+r"(iterations)
        : [img] "r"(image), [one] "r"(one_pd)
        : "cc", "memory");
}


#define FPU_WORKERS    8U
#define FPU_ROUNDS     6U
#define FPU_ITERATIONS 60000ULL

static volatile uint32_t fpu_gate;
static volatile uint32_t fpu_done[FPU_WORKERS];
static volatile uint32_t fpu_failed[FPU_WORKERS];
static volatile uint32_t fpu_cpu_mask;

static void fpu_worker_entry(void *arg)
{
    uint32_t slot = (uint32_t)(uintptr_t)arg;
    uint64_t image[32] __attribute__((aligned(16)));

    while (__atomic_load_n(&fpu_gate, __ATOMIC_ACQUIRE) == 0) {
        thread_yield();
    }

    /* Distinct per-worker bases keep every lane of every worker unique, so a
     * register image leaking between threads cannot go unnoticed. */
    uint64_t base = 1000000ULL * (uint64_t)(slot + 1U);

    for (uint32_t round = 0; round < FPU_ROUNDS; round++) {
        sse_image_fill(image, base);
        sse_lane_stress(image, FPU_ITERATIONS);
        base += FPU_ITERATIONS;

        if (!sse_image_matches(image, base)) {
            __atomic_store_n(&fpu_failed[slot], round + 1U, __ATOMIC_RELEASE);
        }

        __atomic_fetch_or(&fpu_cpu_mask, 1U << this_cpu_index(),
                          __ATOMIC_ACQ_REL);
        thread_yield();
    }

    __atomic_store_n(&fpu_done[slot], 1U, __ATOMIC_RELEASE);
    thread_exit();
}

static void run_fpu_smp_test(void)
{
    task_t *tasks[FPU_WORKERS / 2U];
    uint64_t reaped_base = sched_reaped_count();
    size_t frames_before = pmm_free_frame_count();

    fpu_gate = 0;
    fpu_cpu_mask = 0;

    for (uint32_t t = 0; t < FPU_WORKERS / 2U; t++) {
        tasks[t] = task_create();
        if (tasks[t] == NULL) {
            kpanic("fpu stress: task_create failed");
        }
        task_set_affinity(tasks[t], TASK_AFFINITY_ANY);
    }

    for (uint32_t w = 0; w < FPU_WORKERS; w++) {
        fpu_done[w] = 0;
        fpu_failed[w] = 0;

        thread_t *thread = thread_create(tasks[w / 2U], fpu_worker_entry,
                                         (void *)(uintptr_t)w);
        if (thread == NULL) {
            kpanic("fpu stress: thread_create failed");
        }
        if (((uintptr_t)thread->fxsave_area & 15U) != 0) {
            kpanic("fpu stress: FXSAVE area %p misaligned", thread->fxsave_area);
        }
    }

    /* This thread keeps its own XMM image live across the whole test, so the
     * creator's state is checked too. */
    uint64_t own[32] __attribute__((aligned(16)));
    sse_image_fill(own, 77000000ULL);

    __atomic_store_n(&fpu_gate, 1U, __ATOMIC_RELEASE);

    uint64_t deadline = sched_tick_count() + 20000;
    for (;;) {
        uint32_t finished = 0;

        for (uint32_t w = 0; w < FPU_WORKERS; w++) {
            finished += __atomic_load_n(&fpu_done[w], __ATOMIC_ACQUIRE);
        }
        if (finished == FPU_WORKERS) {
            break;
        }
        if (sched_tick_count() > deadline) {
            kpanic("fpu stress: workers timed out (%lu/%lu)",
                   (uint64_t)finished, (uint64_t)FPU_WORKERS);
        }
        sse_lane_stress(own, 1000);
        thread_sleep_ms(1);
    }

    wait_for_reaped(reaped_base + FPU_WORKERS);

    for (uint32_t w = 0; w < FPU_WORKERS; w++) {
        if (fpu_failed[w] != 0) {
            kpanic("fpu stress: worker %lu XMM corruption in round %lu",
                   (uint64_t)w, (uint64_t)fpu_failed[w]);
        }
    }

    for (uint32_t t = 0; t < FPU_WORKERS / 2U; t++) {
        task_destroy(tasks[t]);
    }

    uint32_t online = smp_online_count();
    uint32_t mask = __atomic_load_n(&fpu_cpu_mask, __ATOMIC_ACQUIRE);
    uint32_t distinct = (uint32_t)__builtin_popcount(mask);

    if (online >= FPU_WORKERS / 2U && distinct < FPU_WORKERS / 2U) {
        kpanic("fpu stress: workers reached only %lu CPU(s) (mask 0x%lx)",
               (uint64_t)distinct, (uint64_t)mask);
    }
    if (pmm_free_frame_count() != frames_before) {
        kpanic("fpu stress: leaked frames (%lu -> %lu)", frames_before,
               pmm_free_frame_count());
    }

    klog(KLOG_INFO,
         "fpu stress: %lu workers x %lu rounds of 16-XMM addpd on %lu/%lu CPUs "
         "(mask 0x%lx), 0 register corruptions, 0 leaked frames",
         (uint64_t)FPU_WORKERS, (uint64_t)FPU_ROUNDS, (uint64_t)distinct,
         (uint64_t)online, (uint64_t)mask);
}

#define QOS_TEST_MS 200U

static volatile uint32_t qos_stop;
static volatile uint64_t qos_counter[3];
static volatile uint32_t qos_exited;

static void qos_spin_entry(void *arg)
{
    uint32_t slot = (uint32_t)(uintptr_t)arg;

    while (__atomic_load_n(&qos_stop, __ATOMIC_ACQUIRE) == 0) {
        __atomic_fetch_add(&qos_counter[slot], 1ULL, __ATOMIC_RELAXED);
        __asm__ volatile("pause");
    }

    __atomic_fetch_add(&qos_exited, 1U, __ATOMIC_ACQ_REL);
    thread_exit();
}

/* Two never-blocking INTERACTIVE spinners share one CPU with a BACKGROUND
 * spinner and with this DEFAULT thread. Strict priority alone would starve
 * both lower classes; aging must keep them moving while the interactive class
 * still receives the bulk of the CPU. */
static void run_qos_aging_test(void)
{
    task_t *task = task_create();
    if (task == NULL) {
        kpanic("qos: task_create failed");
    }

    uint64_t reaped_base = sched_reaped_count();

    qos_stop = 0;
    qos_exited = 0;
    for (uint32_t i = 0; i < 3; i++) {
        qos_counter[i] = 0;
    }

    thread_t *spinners[3];

    for (uint32_t i = 0; i < 3; i++) {
        spinners[i] = thread_create(task, qos_spin_entry, (void *)(uintptr_t)i);
        if (spinners[i] == NULL) {
            kpanic("qos: thread_create failed");
        }
    }
    if (thread_set_qos(spinners[0], THREAD_QOS_INTERACTIVE) != SYS_OK ||
        thread_set_qos(spinners[1], THREAD_QOS_INTERACTIVE) != SYS_OK ||
        thread_set_qos(spinners[2], THREAD_QOS_BACKGROUND) != SYS_OK ||
        thread_set_qos(spinners[2], THREAD_QOS_COUNT) != SYS_ERR_INVAL) {
        kpanic("qos: thread_set_qos rejected a valid class or accepted 3");
    }
    if (spinners[0]->effective_qos != THREAD_QOS_INTERACTIVE ||
        spinners[2]->effective_qos != THREAD_QOS_BACKGROUND) {
        kpanic("qos: effective class not updated");
    }

    uint64_t start = sched_tick_count();
    thread_sleep_ms(QOS_TEST_MS);
    uint64_t slept = sched_tick_count() - start;

    __atomic_store_n(&qos_stop, 1U, __ATOMIC_RELEASE);

    uint64_t deadline = sched_tick_count() + 5000;
    while (__atomic_load_n(&qos_exited, __ATOMIC_ACQUIRE) != 3) {
        if (sched_tick_count() > deadline) {
            kpanic("qos: spinners did not stop");
        }
        thread_sleep_ms(1);
    }
    wait_for_reaped(reaped_base + 3);
    task_destroy(task);

    uint64_t interactive = qos_counter[0] + qos_counter[1];
    uint64_t background = qos_counter[2];

    if (background == 0) {
        kpanic("qos: BACKGROUND thread starved under interactive load");
    }
    if (interactive < background * 4ULL) {
        kpanic("qos: INTERACTIVE share too small (%lu vs background %lu)",
               interactive, background);
    }
    if (slept > (uint64_t)QOS_TEST_MS + 100ULL) {
        kpanic("qos: DEFAULT sleeper woke %lu ms late", slept - QOS_TEST_MS);
    }

    klog(KLOG_INFO,
         "qos: interactive %lu vs background %lu spins (background aged, "
         "not starved); default sleeper woke after %lu ms",
         interactive, background, slept);
}

/* Device and shared CAP_OBJ_FRAME semantics: device frames never touch the
 * PMM, are shared (not copied) by vspace_clone() and refuse EXEC; shared
 * frames survive until the last mapping and the capability are gone. */
static void run_frame_cap_test(paddr_t device_phys)
{
    const vaddr_t va = 0x0000003000000000ULL;
    size_t frames_before = pmm_free_frame_count();

    mk_object_t *device = cap_device_frame_object_create(device_phys, 2, true);
    if (device == NULL) {
        kpanic("frame caps: device frame creation failed");
    }
    cap_frame_object_t *dev = (cap_frame_object_t *)device;

    vspace_t *a = vspace_create();
    if (a == NULL) {
        kpanic("frame caps: vspace_create failed");
    }
    if (cap_frame_map(dev, a, va, VSPACE_PROT_EXEC) != SYS_ERR_INVAL) {
        kpanic("frame caps: executable device mapping accepted");
    }
    if (cap_frame_map(dev, a, va, VSPACE_PROT_WRITE) != SYS_OK ||
        cap_frame_map(dev, a, va + 0x10000, 0) != SYS_OK) {
        kpanic("frame caps: device frame map failed");
    }

    vspace_t *b = vspace_clone(a);
    if (b == NULL) {
        kpanic("frame caps: vspace_clone failed");
    }

    paddr_t pa = 0;
    paddr_t pb = 0;
    if (!vspace_translate(a, va + PAGE_SIZE, &pa) ||
        !vspace_translate(b, va + PAGE_SIZE, &pb) ||
        pa != device_phys + PAGE_SIZE || pb != pa) {
        kpanic("frame caps: clone did not alias the device range");
    }
    if (vspace_unmap(a, va) != SYS_OK) {
        kpanic("frame caps: device unmap failed");
    }

    paddr_t shared_phys = pmm_alloc_frame();
    if (shared_phys == 0) {
        kpanic("frame caps: frame allocation failed");
    }
    mk_object_t *shared = cap_shared_frame_object_create(shared_phys);
    if (shared == NULL) {
        kpanic("frame caps: shared frame creation failed");
    }
    cap_frame_object_t *sh = (cap_frame_object_t *)shared;

    if (cap_frame_map(sh, a, va + 0x20000, VSPACE_PROT_WRITE) != SYS_OK ||
        cap_frame_map(sh, b, va + 0x20000, 0) != SYS_OK ||
        pmm_frame_refcount(shared_phys) != 3) {
        kpanic("frame caps: shared frame mapping/refcount wrong (%lu)",
               (uint64_t)pmm_frame_refcount(shared_phys));
    }

    *(volatile uint64_t *)pmm_phys_to_virt(shared_phys) = 0x5354414745450045ULL;

    vspace_destroy(a);
    mk_object_release(shared);
    if (pmm_frame_refcount(shared_phys) != 1) {
        kpanic("frame caps: shared frame lost its last mapping reference");
    }
    vspace_destroy(b);
    mk_object_release(device);

    if (pmm_free_frame_count() != frames_before) {
        kpanic("frame caps: leaked or over-freed frames (%lu -> %lu)",
               frames_before, pmm_free_frame_count());
    }

    klog(KLOG_INFO,
         "frame caps: device frame (WC, NX, clone-shared, never PMM-freed) and "
         "shared frame refcounts verified, 0 leaked frames");
}

#define IRQ_TEST_BIT 0x4ULL

/* CAP_OBJ_IRQ over an IOAPIC GSI: binding unmasks the line, a delivery masks
 * it and signals the notification, SYS_IRQ_ACK unmasks it, and deleting the
 * capability masks and releases the GSI. The interrupt is injected as a
 * self-IPI on the allocated vector, which runs the same handler an IOAPIC
 * delivery would. */
static void run_irq_cap_test(void)
{
    if (!ioapic_available()) {
        klog(KLOG_WARN, "irq caps: no IOAPIC, test skipped");
        return;
    }

    if (blk_present()) {
        uint8_t blk_gsi = 0;
        bool low = false;
        bool level = false;
        const pci_device_t *blk = pci_find_device(0x1AF4, 0x1001);

        if (blk != NULL &&
            ioapic_resolve_irq(blk->interrupt_line, &blk_gsi, &low, &level)) {
            mk_object_t *clash = cap_irq_gsi_object_create(blk_gsi, low, level);

            if (clash != NULL) {
                kpanic("irq caps: GSI %lu owned by virtio-blk was granted",
                       (uint64_t)blk_gsi);
            }
        }
    }

    mk_object_t *object = NULL;
    uint8_t gsi = 0;

    for (uint32_t candidate = 23; candidate >= 16; candidate--) {
        object = cap_irq_gsi_object_create((uint8_t)candidate, true, true);
        if (object != NULL) {
            gsi = (uint8_t)candidate;
            break;
        }
    }
    if (object == NULL) {
        kpanic("irq caps: no free GSI in 16..23");
    }

    cap_irq_object_t *irq = (cap_irq_object_t *)object;
    uint8_t vector = irq->vector;
    notification_t *notification = notification_create();
    task_t *task = task_create();

    if (notification == NULL || task == NULL) {
        kpanic("irq caps: allocation failed");
    }

    cap_t irq_cap = CAP_NULL;
    cap_t ntf_cap = CAP_NULL;
    if (cap_insert(task->caps, CAP_OBJ_IRQ, object, CAP_RIGHT_ALL, 0,
                   &irq_cap) != SYS_OK ||
        cap_insert(task->caps, CAP_OBJ_NOTIFICATION, notification,
                   CAP_RIGHT_ALL, 0, &ntf_cap) != SYS_OK) {
        kpanic("irq caps: cap_insert failed");
    }
    mk_object_release(object);
    mk_object_release(&notification->header);

    if (!ioapic_irq_is_masked(gsi)) {
        kpanic("irq caps: unbound GSI %lu is unmasked", (uint64_t)gsi);
    }
    if (irq_cap_ack(irq) != SYS_ERR_INVAL) {
        kpanic("irq caps: ack without a binding accepted");
    }
    if (irq_cap_bind(irq, notification, IRQ_TEST_BIT) != SYS_OK) {
        kpanic("irq caps: bind failed");
    }
    if (ioapic_irq_is_masked(gsi)) {
        kpanic("irq caps: bound GSI %lu still masked", (uint64_t)gsi);
    }

    for (uint32_t round = 0; round < 2; round++) {
        uint64_t before = irq->fire_count;

        lapic_send_ipi(lapic_id(), irq->vector);

        uint64_t deadline = sched_tick_count() + 1000;
        while (__atomic_load_n(&irq->fire_count, __ATOMIC_ACQUIRE) == before) {
            if (sched_tick_count() > deadline) {
                kpanic("irq caps: vector 0x%lx never fired",
                       (uint64_t)irq->vector);
            }
            thread_sleep_ms(1);
        }

        uint64_t flags = irq_save();
        uint64_t word = notification->word;
        notification->word = 0;
        irq_restore(flags);

        if ((word & IRQ_TEST_BIT) == 0 || !irq->awaiting_ack ||
            !ioapic_irq_is_masked(gsi)) {
            kpanic("irq caps: delivery did not signal+mask (word 0x%lx)", word);
        }
        if (irq_cap_ack(irq) != SYS_OK || irq->awaiting_ack ||
            ioapic_irq_is_masked(gsi)) {
            kpanic("irq caps: SYS_IRQ_ACK did not unmask GSI %lu",
                   (uint64_t)gsi);
        }
    }

    if (cap_delete(task->caps, irq_cap) != SYS_OK ||
        cap_delete(task->caps, ntf_cap) != SYS_OK) {
        kpanic("irq caps: cap_delete failed");
    }
    if (!ioapic_irq_is_masked(gsi)) {
        kpanic("irq caps: released GSI %lu left unmasked", (uint64_t)gsi);
    }

    mk_object_t *again = cap_irq_gsi_object_create(gsi, true, true);
    if (again == NULL) {
        kpanic("irq caps: GSI %lu was not released", (uint64_t)gsi);
    }
    mk_object_release(again);
    task_destroy(task);

    if (mk_object_live_count() != 0) {
        kpanic("irq caps: %lu objects leaked", mk_object_live_count());
    }

    klog(KLOG_INFO,
         "irq caps: GSI %lu -> vector 0x%lx bound to a notification, "
         "fire->mask->notify->SYS_IRQ_ACK->unmask x2, release masks the line",
         (uint64_t)gsi, (uint64_t)vector);
}

/* Wraps the Limine framebuffer into a write-combining device frame and a
 * read-only display-info frame, registered as boot capabilities 1 and 2 for
 * PID 1. */
static void publish_boot_display(void)
{
    struct limine_framebuffer_response *response = framebuffer_request.response;

    if (response == NULL || response->framebuffer_count == 0 ||
        response->framebuffers == NULL || response->framebuffers[0] == NULL) {
        klog(KLOG_WARN, "display: bootloader reported no framebuffer");
        return;
    }

    struct limine_framebuffer *fb = response->framebuffers[0];
    uint64_t hhdm = hhdm_request.response->offset;
    uint64_t virt = (uint64_t)(uintptr_t)fb->address;

    if (virt < hhdm || fb->pitch == 0 || fb->height == 0) {
        klog(KLOG_WARN, "display: framebuffer descriptor unusable");
        return;
    }

    paddr_t phys = virt - hhdm;
    uint64_t size = fb->pitch * fb->height;
    paddr_t base = PAGE_ALIGN_DOWN(phys);
    uint64_t span = PAGE_ALIGN_UP(phys + size) - base;
    bool wc = cpu_write_combining_pat_entry() != 0;

    run_frame_cap_test(base);

    mk_object_t *fb_object =
        cap_device_frame_object_create(base, (size_t)(span / PAGE_SIZE), wc);
    paddr_t info_phys = pmm_alloc_frame();

    if (fb_object == NULL || info_phys == 0) {
        kpanic("display: cannot allocate the framebuffer capabilities");
    }

    display_info_t *info = (display_info_t *)pmm_phys_to_virt(info_phys);

    memset(info, 0, sizeof(*info));
    info->magic = DISPLAY_INFO_MAGIC;
    info->version = DISPLAY_INFO_VERSION;
    info->framebuffer_count = (uint32_t)response->framebuffer_count;
    info->phys_base = phys;
    info->size_bytes = size;
    info->mapped_bytes = span;
    info->width = fb->width;
    info->height = fb->height;
    info->pitch = fb->pitch;
    info->bpp = fb->bpp;
    info->memory_model = fb->memory_model;
    info->red_mask_size = fb->red_mask_size;
    info->red_mask_shift = fb->red_mask_shift;
    info->green_mask_size = fb->green_mask_size;
    info->green_mask_shift = fb->green_mask_shift;
    info->blue_mask_size = fb->blue_mask_size;
    info->blue_mask_shift = fb->blue_mask_shift;
    info->write_combining = wc ? 1U : 0U;

    mk_object_t *info_object = cap_shared_frame_object_create(info_phys);
    if (info_object == NULL) {
        kpanic("display: cannot wrap the display-info frame");
    }

    cap_t fb_handle = CAP_NULL;
    cap_t info_handle = CAP_NULL;

    if (cap_boot_register(CAP_OBJ_FRAME, fb_object,
                          CAP_RIGHT_READ | CAP_RIGHT_WRITE | CAP_RIGHT_GRANT,
                          &fb_handle) != SYS_OK ||
        cap_boot_register(CAP_OBJ_FRAME, info_object,
                          CAP_RIGHT_READ | CAP_RIGHT_GRANT,
                          &info_handle) != SYS_OK ||
        fb_handle != DISPLAY_BOOT_CAP_FRAMEBUFFER ||
        info_handle != DISPLAY_BOOT_CAP_INFO) {
        kpanic("display: boot capability registration failed");
    }

    /* Privileged hardware-control authority for PID 1. */
    mk_object_t *hw_object = cap_hardware_object_create();
    if (hw_object != NULL) {
        cap_t hw_handle = CAP_NULL;
        (void)cap_boot_register(CAP_OBJ_HARDWARE, hw_object, CAP_RIGHT_ALL,
                                &hw_handle);
        mk_object_release(hw_object);
    }


    /* The boot registry holds the references from here on. */
    mk_object_release(fb_object);
    mk_object_release(info_object);

    /* Kernel framebuffer text console: mirror klog + /dev/console onto the
     * linear framebuffer alongside COM1. Stops at fbcon_release(). */
    fbcon_init((void *)(uintptr_t)fb->address, (uint32_t)fb->width,
               (uint32_t)fb->height, (uint32_t)fb->pitch, (uint32_t)fb->bpp,
               fb->red_mask_size, fb->red_mask_shift, fb->green_mask_size,
               fb->green_mask_shift, fb->blue_mask_size, fb->blue_mask_shift);

    klog(KLOG_INFO,
         "display: %lux%lu %lubpp pitch %lu at 0x%lx (%lu KiB, %s) -> boot caps "
         "%lu (framebuffer) + %lu (read-only display info) for PID 1",
         fb->width, fb->height, (uint64_t)fb->bpp, fb->pitch, phys,
         span / 1024ULL, wc ? "write-combining" : "uncached",
         (uint64_t)fb_handle, (uint64_t)info_handle);
}

static void run_stage_e_tests(void)
{
    run_fpu_smp_test();
    run_qos_aging_test();
    run_irq_cap_test();
}

static void selftest_entry(void *arg)
{
    UNUSED(arg);

    /* No other worker exists yet, so this exercises the idle fallback path:
     * thread_sleep_ms() must not return until ~50 timer ticks have elapsed. */
    uint64_t start_tick = sched_tick_count();
    thread_sleep_ms(50);
    uint64_t elapsed = sched_tick_count() - start_tick;

    if (elapsed < 50) {
        kpanic("selftest: idle-fallback sleep lasted only %lu ticks", elapsed);
    }
    klog(KLOG_INFO, "selftest: idle-fallback sleep lasted %lu ticks (>= 50)",
         elapsed);

    /* Short-lived threads must be unlinked and their stacks/thread objects
     * reclaimed. A second identical batch must reuse the freed heap blocks
     * instead of mapping more physical frames. */
    task_t *boot = sched_boot_task();
    uint64_t base_reaped = sched_reaped_count();

    reaper_gate = 0;
    spawn_reapers(boot, 8);
    reaper_gate = 1;
    wait_for_reaped(base_reaped + 8);
    if (sched_reap_pending()) {
        kpanic("selftest: reap list not drained after first batch");
    }
    size_t frames_after_first = pmm_free_frame_count();

    reaper_gate = 0;
    spawn_reapers(boot, 8);
    reaper_gate = 1;
    wait_for_reaped(base_reaped + 16);
    if (sched_reap_pending()) {
        kpanic("selftest: reap list not drained after second batch");
    }
    size_t frames_after_second = pmm_free_frame_count();

    if (frames_after_second != frames_after_first) {
        kpanic("selftest: reaped memory not reused (%lu -> %lu free frames)",
               frames_after_first, frames_after_second);
    }

    klog(KLOG_INFO,
         "selftest: 16 short-lived threads reaped, heap reuse verified (%lu free frames)",
         frames_after_second);

    /* Verify the guarded-stack slot geometry and that one complete
     * allocate/run/reap cycle returns every PMM frame. The reaper batches
     * above have already populated the virtual-slot reuse list. */
    size_t frames_before_probe = pmm_free_frame_count();
    reaper_gate = 0;
    thread_t *probe = thread_create(boot, reaper_entry, NULL);

    if (probe == NULL) {
        kpanic("selftest: cannot create guarded-stack probe");
    }

    uint64_t stack_base = (uint64_t)(uintptr_t)probe->kstack_base;
    uint64_t stack_top = probe->kstack_top;

    if (stack_base < KSTACK_BASE ||
        stack_base + KSTACK_SLOT_SIZE > KSTACK_LIMIT ||
        stack_top != stack_base + KSTACK_SLOT_SIZE) {
        kpanic("selftest: guarded stack slot geometry is invalid");
    }

    paddr_t probe_phys = 0;
    if (vspace_translate(vspace_kernel(), stack_base, &probe_phys)) {
        kpanic("selftest: kernel stack guard page is mapped");
    }
    for (size_t page = 1; page <= KSTACK_PAGE_COUNT; page++) {
        if (!vspace_translate(vspace_kernel(),
                              stack_base + page * PAGE_SIZE, &probe_phys)) {
            kpanic("selftest: kernel stack page is unmapped");
        }
    }

    uint64_t probe_reaped = sched_reaped_count();
    reaper_gate = 1;
    wait_for_reaped(probe_reaped + 1);

    if (sched_reap_pending()) {
        kpanic("selftest: guarded-stack probe left a pending reap");
    }

    size_t frames_after_probe = pmm_free_frame_count();
    if (frames_after_probe != frames_before_probe) {
        kpanic("selftest: guarded-stack cycle leaked frames (%lu -> %lu)",
               frames_before_probe, frames_after_probe);
    }

    klog(KLOG_INFO,
         "selftest: guarded kernel stack (1 guard + 4 RW/NX pages) reaped, "
         "0 leaked frames");

    /* The boot/idle thread was moved off the static bootstrap stack by
     * sched_init()/boot_stack_enter(); it must have the same guard geometry. */
    thread_t *idle = sched_boot_thread();

    if (idle == NULL || idle->kstack_base == NULL) {
        kpanic("selftest: idle thread has no guarded stack");
    }

    uint64_t idle_base = (uint64_t)(uintptr_t)idle->kstack_base;
    uint64_t idle_top = idle->kstack_top;

    if (idle_base < KSTACK_BASE ||
        idle_base + KSTACK_SLOT_SIZE > KSTACK_LIMIT ||
        idle_top != idle_base + KSTACK_SLOT_SIZE) {
        kpanic("selftest: idle stack slot geometry is invalid");
    }

    paddr_t idle_phys = 0;
    if (vspace_translate(vspace_kernel(), idle_base, &idle_phys)) {
        kpanic("selftest: idle stack guard page is mapped");
    }
    for (size_t page = 1; page <= KSTACK_PAGE_COUNT; page++) {
        if (!vspace_translate(vspace_kernel(),
                              idle_base + page * PAGE_SIZE, &idle_phys)) {
            kpanic("selftest: idle stack page is unmapped");
        }
    }

    klog(KLOG_INFO, "selftest: boot/idle thread uses a guarded kernel stack");

    run_phase4_tests();
    run_phase5_tests();

    run_stress_test();
    run_smp_stress_test();
    run_cow_stress_test();
    self_test_virtio_blk();
    run_stage_e_tests();
    publish_boot_display();

    collect_bsd_boot_modules();
    ahci_init();
    bsd_init(bsd_boot_modules, bsd_boot_module_count);

    thread_exit();
}

/* Runs on the idle thread's guarded stack after the bootstrap stack has been
 * abandoned. */
static NORETURN void boot_continuation(void)
{
    syscall_init();

    /* Bring every AP online before any worker thread exists: AP startup is a
     * blocking INIT-SIPI-SIPI handshake that must not be interleaved with
     * preemptive scheduling on the BSP. */
    smp_prepare();

    smp_boot_report_t report;
    if (smp_bringup(&report)) {
        klog(KLOG_INFO,
             "smp: %lu discovered, %lu started, %lu failed, %lu online",
             (uint64_t)report.discovered, (uint64_t)report.started,
             (uint64_t)report.failed, (uint64_t)smp_online_count());
    }

    if (thread_create(sched_boot_task(), selftest_entry, NULL) == NULL) {
        kpanic("failed to create scheduler self-test thread");
    }

    lapic_timer_start();
    klog(KLOG_INFO, "Phase 3 preemptive multitasking initialized");
    sched_start();

    for (;;) {
        __asm__ volatile("hlt");
    }
}

void kernel_main(void)
{
    serial_init(115200);

    /* gdt_init() installs the GDT and reloads every data segment, which clears
     * GS_BASE. The per-CPU block must therefore be bound after it, because
     * this_cpu() (used by every spinlock) reads through GS. */
    gdt_init();

    /* Bind the BSP's per-CPU block before the first kernel log or subsystem
     * lock: spin_acquire() records its owner through this_cpu(). */
    cpu_local_bind(0);

    klog(KLOG_INFO, "LucyK x86_64 microkernel - Phase 1 HAL bootstrap");
    klog(KLOG_INFO, "Kernel base: 0x%016lx", (uint64_t)KERNEL_BASE);

    if (!LIMINE_BASE_REVISION_SUPPORTED) {
        kpanic("Limine base revision 3 is not supported by this bootloader");
    }
    klog(KLOG_INFO, "Limine base revision 3 negotiated");

    klog(KLOG_INFO,
         "GDT/TSS installed: kcode=0x%lx kdata=0x%lx ucode=0x%lx udata=0x%lx tss=0x%lx",
         (uint64_t)gdt_kernel_code_selector(),
         (uint64_t)gdt_kernel_data_selector(),
         (uint64_t)gdt_user_code_selector(),
         (uint64_t)gdt_user_data_selector(),
         (uint64_t)gdt_tss_selector());

    idt_init();
    klog(KLOG_INFO, "IDT installed with 256 interrupt gates");

    /* The BSP's per-CPU block was bound before the first log line; pmm_init()
     * below relies on this_cpu() being valid when it records a lock owner. */
    if (hhdm_request.response == NULL) {
        kpanic("Limine did not provide an HHDM response");
    }
    klog(KLOG_INFO, "Limine HHDM offset: 0x%016lx", hhdm_request.response->offset);


    log_memory_map();
    klog(KLOG_INFO, "Phase 1 HAL initialized");

    pmm_init(memmap_request.response, hhdm_request.response->offset);
    klog(KLOG_INFO, "PMM initialized: %lu frames managed, %lu free (%lu MiB)",
         pmm_total_frame_count(), pmm_free_frame_count(),
         (pmm_free_frame_count() * PAGE_SIZE) / (1024ULL * 1024ULL));

    vspace_init();
    klog(KLOG_INFO, "vspace manager initialized (NXE + CR0.WP enabled)");

    /* ACPI tables can sit in legacy holes the direct map does not cover; the
     * page-table API is available now, so cache the topology before the
     * firmware pages are reused. */
    acpi_cache_topology();

    cpu_init_security_features();

    kalloc_init();
    klog(KLOG_INFO, "kernel heap initialized (virtual window 0x%016lx)",
         (uint64_t)KHEAP_BASE);

    /* Warm the heap once so subsequent self-tests reuse an existing chunk and
     * pmm frame accounting stays stable across vspace create/destroy cycles. */
    void *heap_warmup = kalloc(sizeof(uint64_t));
    if (heap_warmup == NULL) {
        kpanic("kernel heap warm-up failed");
    }
    kfree(heap_warmup);

    self_test_pmm();
    self_test_vspace();
    self_test_cow();
    self_test_kalloc();
    self_test_kernel_mapping();

    klog(KLOG_INFO, "Phase 2 memory management initialized");

    lapic_init();
    pci_init();
    ioapic_init();
    sched_init();

    /* Move the idle context onto its guarded slot before enabling the timer.
     * syscall_init() then programs TSS.rsp0 and cpu_local.kernel_rsp from that
     * slot. The static bootstrap stack is no longer used. */
    boot_stack_enter(sched_boot_stack_top(), boot_continuation);
    __builtin_unreachable();
}
