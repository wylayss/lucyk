#include <acpi.h>
#include <cpu.h>
#include <gdt.h>
#include <idt.h>
#include <io.h>
#include <klog.h>
#include <lapic.h>
#include <pmm.h>
#include <sched.h>
#include <smp.h>
#include <string.h>
#include <syscall.h>
#include <types.h>
#include <vspace.h>

/* Physical page holding the real-mode trampoline (must be below 1 MiB). The
 * AP starts there with CS = 0x0800, IP = 0, so the page doubles as the SIPI
 * vector source. */
#define AP_TRAMPOLINE_PHYS 0x8000ULL
#define AP_SIPI_VECTOR     (AP_TRAMPOLINE_PHYS >> 12)

/* Parameter block offset inside the trampoline page; must match
 * ap_trampoline.asm (the eight qwords immediately below 0x1000). */
#define AP_PARAMS_OFFSET 0xF00U

/* Bounded spin budgets for the INIT-SIPI-SIPI handshake. These run before any
 * peer thread exists, so they are plain pause loops rather than scheduler
 * waits: a wedged AP must never hang the whole machine. */
#define AP_ONLINE_SPIN_LIMIT 800000000ULL
#define AP_TLB_ACK_SPIN_LIMIT 400000000ULL

/* Number of CPUs that reached their idle context, BSP included. */
static volatile uint32_t smp_started;
static uint32_t smp_failed;

/* Set once INIT-SIPI-SIPI is over for every discovered AP. Until then the BSP
 * is still building page tables for the APs and must not issue TLB shootdowns:
 * an AP that has just been released is not yet taking interrupts, so its
 * acknowledgement would never arrive. */
static bool smp_bringup_complete;

extern const uint8_t ap_trampoline_image[];
extern const uint8_t ap_trampoline_image_end[];

/* Layout must match the parameter block in ap_trampoline.asm. */
typedef struct PACKED ap_params {
    uint64_t cr3;       /* 0x8F00: kernel_pml4 physical address   */
    uint64_t gdt_limit; /* 0x8F08 */
    uint64_t gdt_base;  /* 0x8F10 */
    uint64_t idt_limit; /* 0x8F18 */
    uint64_t idt_base;  /* 0x8F20 */
    uint64_t kstack_top;/* 0x8F28: higher-half guarded stack top  */
    uint64_t cpu_index; /* 0x8F30 */
    uint64_t entry_fn;  /* 0x8F38: ap_entry_c, higher-half VA     */
} ap_params_t;

_Static_assert(sizeof(ap_params_t) == 8 * sizeof(uint64_t),
               "ap_params_t layout must match the trampoline");
_Static_assert(offsetof(ap_params_t, cr3) == 0x00, "cr3 offset");
_Static_assert(offsetof(ap_params_t, gdt_limit) == 0x08, "gdt_limit offset");
_Static_assert(offsetof(ap_params_t, gdt_base) == 0x10, "gdt_base offset");
_Static_assert(offsetof(ap_params_t, idt_limit) == 0x18, "idt_limit offset");
_Static_assert(offsetof(ap_params_t, idt_base) == 0x20, "idt_base offset");
_Static_assert(offsetof(ap_params_t, kstack_top) == 0x28, "kstack_top offset");
_Static_assert(offsetof(ap_params_t, cpu_index) == 0x30, "cpu_index offset");
_Static_assert(offsetof(ap_params_t, entry_fn) == 0x38, "entry_fn offset");

/* APs are brought online unconditionally: every core the firmware reports must
 * participate in scheduling, not just the BSP. */
static const bool smp_start_aps = true;

static void ipi_reschedule_handler(interrupt_frame_t *frame)
{
    UNUSED(frame);

    lapic_send_eoi();
    sched_request_reschedule();
}

/* Remote-flush protocol.
 *
 * One global request counter, monotonically increasing, orders every published
 * page-table change. A sender allocates its id *after* the change and only then
 * sends the IPI. A target handler reads the counter before flushing and
 * publishes that value afterwards, so "serviced >= id" proves the flush ran
 * after the id was allocated, hence after the change that id was allocated for.
 *
 * The pre-flush read is essential: a value observed only after the flush could
 * name a change that the flush already missed. The handler therefore re-reads
 * after flushing and repeats while new requests arrived in between, so the
 * value it publishes always predates its flush.
 *
 * A waiter also services incoming requests while spinning. Shootdowns are
 * issued from contexts that run with interrupts disabled (page-table
 * reclamation inside the scheduler), where two CPUs could otherwise wait on
 * each other forever because neither can take the other's IPI. Servicing makes
 * a waiter a target, so the wait graph can never close into a cycle.
 *
 * tlb_flush_sequence is the per-CPU published (serviced) id. */
volatile uint64_t tlb_flush_sequence[MAX_CPUS];

static uint64_t tlb_request_counter;

/* Flushes this CPU's TLB and publishes every request that predates the flush. */
static void tlb_service_local(void)
{
    /* Two passes at most: the second covers a request that arrived during the
     * first flush. Anything later is picked up by the next delivery of the
     * vector, and bounding the loop keeps a shootdown storm from pinning this
     * CPU inside the handler. */
    for (int pass = 0; pass < 2; pass++) {
        uint64_t requested =
            __atomic_load_n(&tlb_request_counter, __ATOMIC_ACQUIRE);

        uint64_t cr3;
        __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
        __asm__ volatile("mov %0, %%cr3" : : "r"(cr3) : "memory");

        uint32_t self = this_cpu_index();
        uint64_t published =
            __atomic_load_n(&tlb_flush_sequence[self], __ATOMIC_RELAXED);

        while (published < requested) {
            if (__atomic_compare_exchange_n(&tlb_flush_sequence[self],
                                            &published, requested, false,
                                            __ATOMIC_RELEASE,
                                            __ATOMIC_RELAXED)) {
                break;
            }
        }

        if (__atomic_load_n(&tlb_request_counter, __ATOMIC_ACQUIRE) ==
            requested) {
            return;
        }
    }
}

static void ipi_tlb_handler(interrupt_frame_t *frame)
{
    UNUSED(frame);

    tlb_service_local();
    lapic_send_eoi();
}

void tlb_shootdown_service(void)
{
    ipi_tlb_handler(NULL);
}

/* Called with no vspace or PMM lock held. Peers that are not yet online are
 * skipped: their first CR3 load starts with a cold TLB, so they can never hold
 * a stale translation of the change being published. */
void tlb_shootdown_mask(uint64_t cpu_mask, vaddr_t vaddr)
{
    UNUSED(vaddr);

    uint32_t self = this_cpu_index();
    uint64_t targets = cpu_mask & ~(1ULL << self);

    if (targets == 0 || !smp_multicore_active()) {
        return;
    }

    /* Release ordering publishes the page-table change before the id becomes
     * visible to any handler. */
    uint64_t request =
        __atomic_fetch_add(&tlb_request_counter, 1ULL, __ATOMIC_RELEASE) + 1ULL;
    uint64_t pending = 0;

    for (uint32_t cpu = 0; cpu < MAX_CPUS; cpu++) {
        if ((targets & (1ULL << cpu)) == 0) {
            continue;
        }

        cpu_local_t *peer = cpu_local_for(cpu);

        if (peer == NULL || !__atomic_load_n(&peer->online, __ATOMIC_ACQUIRE)) {
            continue;
        }

        pending |= (1ULL << cpu);
    }

    if (pending == 0) {
        return;
    }

    for (uint32_t cpu = 0; cpu < MAX_CPUS; cpu++) {
        if ((pending & (1ULL << cpu)) != 0) {
            lapic_send_ipi(cpu_local_for(cpu)->lapic_id,
                           IPI_TLB_SHOOTDOWN_VECTOR);
        }
    }

    /* Wait without touching the interrupt flag: the callers include paths that
     * already run with interrupts disabled. Progress is guaranteed by
     * servicing local requests inside the loop. */
    uint64_t spins = 0;

    while (pending != 0) {
        for (uint32_t cpu = 0; cpu < MAX_CPUS; cpu++) {
            if ((pending & (1ULL << cpu)) == 0) {
                continue;
            }
            if (__atomic_load_n(&tlb_flush_sequence[cpu], __ATOMIC_ACQUIRE) >=
                request) {
                pending &= ~(1ULL << cpu);
            }
        }

        if (pending == 0) {
            break;
        }
        if (++spins > AP_TLB_ACK_SPIN_LIMIT) {
            klog(KLOG_WARN, "smp: TLB shootdown timed out (mask 0x%lx)",
                 pending);
            break;
        }

        /* Act as a target so a peer waiting on us can always finish. */
        tlb_service_local();
        __builtin_ia32_pause();
    }
}

/* Copies the flat trampoline image into the reserved low page, installs the
 * temporary PML4[0] identity leaf that lets the AP execute there once paging is
 * enabled, and registers the IPI handlers. Runs once on the BSP. */
void smp_prepare(void)
{
    paddr_t slot = (paddr_t)pmm_ap_trampoline_page();
    uint8_t *dest = (uint8_t *)pmm_phys_to_virt(slot);
    size_t size = (size_t)(ap_trampoline_image_end - ap_trampoline_image);

    if (size == 0 || size > PAGE_SIZE) {
        kpanic("smp: trampoline image size %lu is invalid", size);
    }

    memset(dest, 0, PAGE_SIZE);
    memcpy(dest, ap_trampoline_image, size);

    /* The trampoline is position-dependent and assembled for ORG 0x8000, which
     * is also where it is copied, so its inline GDT descriptor needs no
     * patching. Verify the reservation actually gave us that page. */
    if (slot != AP_TRAMPOLINE_PHYS) {
        kpanic("smp: trampoline page is 0x%lx, expected 0x8000",
               (uint64_t)slot);
    }

    /* The AP enables paging while still executing at 0x8xxx. A single 4 KiB
     * identity leaf in kernel_pml4 covers exactly that page; the intermediate
     * tables are private and never alias PML4[511] or the HHDM. */
    if (vspace_identity_map(slot) != SYS_OK) {
        kpanic("smp: cannot install the trampoline identity mapping");
    }

    idt_register_handler(IPI_RESCHEDULE_VECTOR, ipi_reschedule_handler);
    idt_register_handler(IPI_TLB_SHOOTDOWN_VECTOR, ipi_tlb_handler);

    /* The BSP has been executing since boot, so it counts as online from the
     * start. Publishing it here means smp_online_count() reports the whole
     * machine (BSP included) once the APs are up, and the scheduler can treat
     * CPU 0 as a placement candidate on equal terms. */
    cpu_local_t *bsp = cpu_local_for(0);

    bsp->cpu_index = 0;
    bsp->lapic_id = lapic_id();
    __atomic_store_n(&bsp->online, 1ULL, __ATOMIC_RELEASE);
    __atomic_fetch_add(&smp_started, 1U, __ATOMIC_ACQ_REL);

    klog(KLOG_INFO,
         "smp: trampoline at 0x%lx, %lu bytes, identity page installed",
         (uint64_t)slot, (uint64_t)size);
}

uint32_t smp_online_count(void)
{
    return __atomic_load_n(&smp_started, __ATOMIC_ACQUIRE);
}

bool smp_multicore_active(void)
{
    return smp_online_count() > 1 && smp_bringup_complete;
}

/* Guarded stack slot of each AP. The BSP allocates and maps it before the
 * handshake so kstack_top in the parameter block is valid the moment the AP
 * enters long mode; once the AP is parked on its own idle context, the slot is
 * handed back to the PMM. */
static vaddr_t ap_stack_slot[MAX_CPUS];

/* Called by an AP's idle context once it is no longer executing on the
 * bootstrap stack. Returns that stack's slot base exactly once; a second call
 * for the same CPU returns 0. */
vaddr_t ap_bootstrap_stack_take(uint32_t cpu_index)
{
    if (cpu_index >= MAX_CPUS) {
        return 0;
    }

    return __atomic_exchange_n(&ap_stack_slot[cpu_index], 0, __ATOMIC_ACQ_REL);
}

/* Called from the idle body. Publishing here is what releases the BSP onto the
 * next AP: only now has this CPU left the bootstrap stack for good. */
void smp_cpu_park_ready(uint32_t cpu_index)
{
    if (cpu_index >= MAX_CPUS) {
        return;
    }

    cpu_local_t *cpu = cpu_local_for(cpu_index);

    if (cpu == NULL) {
        return;
    }

    __atomic_store_n(&cpu->online, 1ULL, __ATOMIC_RELEASE);
    __atomic_fetch_add(&smp_started, 1U, __ATOMIC_ACQ_REL);

    klog(KLOG_INFO, "smp: CPU %u online (LAPIC id %u, 1000 Hz)", cpu_index,
         cpu->lapic_id);
}

/* Fills the trampoline parameter block for one AP and fires INIT-SIPI-SIPI.
 * The AP's guarded stack is allocated and mapped here, in the kernel page
 * tables it will share, so its higher-half top is valid the moment long mode
 * comes up. */
static bool smp_start_ap(uint32_t cpu_index, uint32_t apic_id)
{
    cpu_local_t *cpu = cpu_local_for(cpu_index);

    if (cpu == NULL) {
        return false;
    }

    vaddr_t stack = kstack_alloc();
    if (stack == 0) {
        return false;
    }

    ap_stack_slot[cpu_index] = stack;

    cpu->cpu_index = cpu_index;
    cpu->lapic_id = apic_id;

    uint64_t gdt_limit;
    uint64_t gdt_base;
    gdt_descriptor(&gdt_limit, &gdt_base);

    uint64_t idt_limit;
    uint64_t idt_base;
    idt_descriptor(&idt_limit, &idt_base);

    paddr_t trampoline = (paddr_t)pmm_ap_trampoline_page();
    ap_params_t *params =
        (ap_params_t *)pmm_phys_to_virt(trampoline + AP_PARAMS_OFFSET);

    memset(params, 0, sizeof(*params));
    params->cr3 = vspace_kernel()->pml4_phys;
    params->gdt_limit = gdt_limit;
    params->gdt_base = gdt_base;
    params->idt_limit = idt_limit;
    params->idt_base = idt_base;
    params->kstack_top = stack + KSTACK_SLOT_SIZE;
    params->cpu_index = cpu_index;
    params->entry_fn = (uint64_t)(uintptr_t)ap_entry_c;

    /* The AP clears and re-sets this flag as it initialises, so the BSP's wait
     * below always observes the final publication from the idle context. */
    __atomic_store_n(&cpu->online, 0ULL, __ATOMIC_RELEASE);

    klog(KLOG_INFO,
         "smp: starting CPU %u (LAPIC %u) sipi=0x%lx cr3=0x%lx stack=0x%lx",
         cpu_index, apic_id, (uint64_t)AP_SIPI_VECTOR, params->cr3,
         params->kstack_top);

    lapic_start_ap(apic_id, AP_SIPI_VECTOR);

    uint64_t spins = 0;

    while (__atomic_load_n(&cpu->online, __ATOMIC_ACQUIRE) == 0) {
        if (++spins > AP_ONLINE_SPIN_LIMIT) {
            klog(KLOG_WARN, "smp: CPU %u (LAPIC %u) did not come online",
                 cpu_index, apic_id);

            /* Withdraw the slot so the bring-up teardown does not free it a
             * second time. */
            (void)__atomic_exchange_n(&ap_stack_slot[cpu_index], 0,
                                      __ATOMIC_ACQ_REL);
            kstack_free(stack);
            return false;
        }
        __builtin_ia32_pause();
    }

    return true;
}

void ap_entry_c(uint32_t cpu_index)
{
    if (cpu_index == 0 || cpu_index >= MAX_CPUS) {
        cpu_halt();
    }

    cpu_local_t *cpu = cpu_local_for(cpu_index);

    if (cpu == NULL) {
        cpu_halt();
    }

    /* Control arrives on the higher-half guarded stack prepared by the BSP,
     * with interrupts disabled, on the kernel's own page tables. */
    cpu_local_bind(cpu_index);
    gdt_load_tss_for_cpu(cpu_index);
    idt_load();

    cpu_init_ap_features();
    syscall_init_ap();

    lapic_init_ap();

    /* Register with the scheduler before the timer is armed: once the LAPIC
     * timer runs, sched_tick() may switch contexts, and this CPU must already
     * have a ring and an idle context for that to be well defined. The
     * BSP-prepared bootstrap stack this code is running on is abandoned by the
     * switch below, so the idle context gets a slot of its own. */
    sched_register_cpu(cpu_index);

    lapic_timer_start();

    /* Switch onto this CPU's idle context. That switch abandons the bootstrap
     * stack; the idle body publishes this CPU as online once it is running
     * there, which is what lets the BSP recycle the slot and start the next
     * AP. */
    sched_enter_idle();
}

bool smp_bringup(smp_boot_report_t *report)
{
    acpi_topology_t topology;

    if (report == NULL) {
        return false;
    }

    memset(report, 0, sizeof(*report));

    if (!acpi_topology_cached(&topology) &&
        !acpi_discover_topology(&topology)) {
        klog(KLOG_WARN, "smp: ACPI topology unavailable, staying uniprocessor");

        /* No AP will run, so the bootstrap page and its temporary identity leaf
         * are dead weight; release both before returning. */
        vspace_identity_unmap(AP_TRAMPOLINE_PHYS);
        return false;
    }

    lapic_set_base(topology.local_apic_base);

    report->discovered = topology.cpu_count;

    klog(KLOG_INFO, "smp: MADT reports %u CPUs, lapic base 0x%lx",
         topology.cpu_count, (uint64_t)topology.local_apic_base);

    if (topology.cpu_count <= 1) {
        /* smp_prepare() already installed the temporary identity leaf; no AP
         * will ever use it, so tear it down before leaving the uniprocessor
         * configuration. */
        vspace_identity_unmap(AP_TRAMPOLINE_PHYS);
        return true;
    }

    if (!smp_start_aps) {
        /* The identity leaf installed by smp_prepare() is unused without APs. */
        vspace_identity_unmap(AP_TRAMPOLINE_PHYS);
        return true;
    }

    uint32_t bsp_lapic = lapic_id();
    uint32_t cpu_index = 1;

    for (uint32_t i = 0; i < topology.cpu_count && cpu_index < MAX_CPUS; i++) {
        uint32_t apic_id = topology.apic_ids[i];

        if (apic_id == bsp_lapic) {
            continue;
        }

        if (smp_start_ap(cpu_index, apic_id)) {
            report->started++;
        } else {
            smp_failed++;
            report->failed++;
        }

        cpu_index++;
    }

    /* Every discovered AP has either published itself online or timed out; the
     * APs are parked on their own idle stacks and cross-CPU shootdowns are safe
     * from this point on. */
    smp_bringup_complete = true;

    /* Reclaim the bootstrap stacks the APs arrived on. They are abandoned now,
     * and freeing them from the BSP keeps the flush synchronised with a CPU
     * that is known to be taking interrupts. */
    for (uint32_t cpu = 1; cpu < MAX_CPUS; cpu++) {
        vaddr_t bootstrap = ap_bootstrap_stack_take(cpu);

        if (bootstrap != 0) {
            kstack_free(bootstrap);
        }
    }

    /* The low identity leaf existed only for the handshake, so it is torn down
     * now: the PML4[0] entry is restored, its three private intermediate
     * frames go back to the PMM, and a broadcast shootdown guarantees no CPU
     * keeps a stale low-half translation of the recycled hierarchy. */
    vspace_identity_unmap(AP_TRAMPOLINE_PHYS);

    klog(KLOG_INFO, "smp: %u online, %u failed, low identity window removed",
         (uint32_t)smp_online_count(), smp_failed);

    return true;
}