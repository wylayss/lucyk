#include <cpu.h>
#include <idt.h>
#include <io.h>
#include <isr.h>
#include <klog.h>
#include <lapic.h>
#include <sched.h>
#include <types.h>
#include <vspace.h>

#define IA32_APIC_BASE    0x1B
#define APIC_X2APIC_ENABLE (1ULL << 10)
#define APIC_BASE_ENABLE  (1ULL << 11)
#define APIC_BASE_MASK    0x0000000FFFFFF000ULL

#define LAPIC_SPURIOUS_VECTOR 0xFFU

#define LAPIC_VADDR 0xFFFFC00000000000ULL

#define LAPIC_REG_ID         0x020
#define LAPIC_REG_VERSION    0x030
#define LAPIC_REG_TPR        0x080
#define LAPIC_REG_EOI        0x0B0
#define LAPIC_REG_SVR        0x0F0
#define LAPIC_REG_ICR_LOW    0x300
#define LAPIC_REG_ICR_HIGH   0x310
#define LAPIC_REG_LVT_TIMER  0x320
#define LAPIC_REG_LVT_ERROR  0x370
#define LAPIC_REG_TIMER_INIT 0x380
#define LAPIC_REG_TIMER_CUR  0x390
#define LAPIC_REG_TIMER_DIV  0x3E0

#define LAPIC_SVR_ENABLE    0x100U
#define LAPIC_LVT_MASKED    (1U << 16)
#define LAPIC_LVT_PERIODIC  (1U << 17)
#define LAPIC_TIMER_DIV_16  0x03U

/* ICR delivery modes and flags. */
#define ICR_FIXED           0x00000U
#define ICR_INIT            0x00500U
#define ICR_STARTUP         0x00600U
#define ICR_LEVEL_ASSERT    (1U << 14)
#define ICR_DELIVERY_PENDING (1U << 12)

#define PIT_CHANNEL2_PORT 0x42
#define PIT_COMMAND_PORT  0x43
#define PIT_PORT61        0x61
#define PIT_MODE0_CH2     0xB0U
#define PIT_10MS_COUNT    11932U

#define LAPIC_TIMER_10MS_TICKS 0xFFFFFFFFU

static paddr_t lapic_base_phys;
static uint32_t ticks_per_ms;
static uint32_t lapic_base_reported;

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

static uint32_t lapic_read(uint32_t offset)
{
    return *(volatile uint32_t *)(uintptr_t)(LAPIC_VADDR + offset);
}

static void lapic_write(uint32_t offset, uint32_t value)
{
    *(volatile uint32_t *)(uintptr_t)(LAPIC_VADDR + offset) = value;
}

static bool cpu_has_apic(void)
{
    uint32_t eax;
    uint32_t ebx;
    uint32_t ecx;
    uint32_t edx;

    __asm__ volatile("cpuid"
                     : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                     : "a"(1U), "c"(0U));
    UNUSED(eax);
    UNUSED(ebx);
    UNUSED(ecx);

    return (edx & (1U << 9)) != 0;
}

static void pit_prepare_channel2(uint16_t count)
{
    uint8_t port61 = inb(PIT_PORT61);

    port61 = (uint8_t)((port61 & (uint8_t)~0x02U) | 0x01U);
    outb(PIT_PORT61, port61);

    outb(PIT_COMMAND_PORT, PIT_MODE0_CH2);
    outb(PIT_CHANNEL2_PORT, (uint8_t)(count & 0xFFU));
    outb(PIT_CHANNEL2_PORT, (uint8_t)(count >> 8));
}

static void pit_wait_output(bool high, const char *stage)
{
    uint64_t spins = 0;
    uint8_t expected = high ? 0x20U : 0x00U;

    while ((inb(PIT_PORT61) & 0x20U) != expected) {
        if (++spins > 100000000ULL) {
            kpanic("lapic: PIT calibration timeout (%s)", stage);
        }
        __asm__ volatile("pause");
    }
}

static uint32_t calibrate_ticks_per_ms(void)
{
    lapic_write(LAPIC_REG_LVT_TIMER, LAPIC_LVT_MASKED);
    lapic_write(LAPIC_REG_TIMER_DIV, LAPIC_TIMER_DIV_16);
    lapic_write(LAPIC_REG_TIMER_INIT, LAPIC_TIMER_10MS_TICKS);

    pit_prepare_channel2(PIT_10MS_COUNT);
    pit_wait_output(false, "start");
    pit_wait_output(true, "expiry");

    uint32_t remaining = lapic_read(LAPIC_REG_TIMER_CUR);
    lapic_write(LAPIC_REG_TIMER_INIT, 0);

    uint32_t elapsed = LAPIC_TIMER_10MS_TICKS - remaining;
    uint32_t result = elapsed / 10U;

    if (result == 0) {
        kpanic("lapic: PIT calibration produced zero ticks/ms");
    }
    return result;
}

void lapic_send_eoi(void)
{
    lapic_write(LAPIC_REG_EOI, 0);
}

uint32_t lapic_id(void)
{
    return (lapic_read(LAPIC_REG_ID) >> 24) & 0xFFU;
}

uint32_t lapic_ticks_per_ms(void)
{
    return ticks_per_ms;
}

void lapic_set_base(uint32_t phys_base)
{
    if (phys_base != 0) {
        lapic_base_phys = (paddr_t)phys_base;
        lapic_base_reported = phys_base;
    }
}

uint32_t lapic_set_base_from_msr(void)
{
    uint64_t apic_msr = read_msr(IA32_APIC_BASE);

    if (lapic_base_phys == 0) {
        lapic_base_phys = apic_msr & APIC_BASE_MASK;
        lapic_base_reported = (uint32_t)lapic_base_phys;
    }

    return lapic_base_reported;
}

/* Maps the LAPIC MMIO window and enables the APIC on the calling CPU. The
 * mapping is global, so every CPU observes it through the shared kernel
 * upper half. APs skip the mapping when the BSP has already installed it,
 * which avoids a redundant TLB shootdown during bringup. */
static void lapic_map_and_enable(void)
{
    uint64_t apic_msr = read_msr(IA32_APIC_BASE);

    /* Force xAPIC MMIO mode; the driver does not program x2APIC MSRs. */
    apic_msr &= ~APIC_X2APIC_ENABLE;
    write_msr(IA32_APIC_BASE, apic_msr | APIC_BASE_ENABLE);

    uint64_t pte_flags = 0;
    if (!vspace_probe_flags(LAPIC_VADDR, &pte_flags)) {
        if (vspace_map_kernel(LAPIC_VADDR, lapic_base_phys,
                              VSPACE_PROT_WRITE | VSPACE_PROT_PCD |
                                  VSPACE_PROT_PWT) != SYS_OK) {
            kpanic("lapic: cannot map MMIO base 0x%016lx", lapic_base_phys);
        }
    }

    if (lapic_id() == 0xFFU && lapic_read(LAPIC_REG_VERSION) == 0xFFFFFFFFU) {
        kpanic("lapic: MMIO page is not accessible");
    }
}

static void lapic_enable_core(void)
{
    lapic_write(LAPIC_REG_TPR, 0);
    lapic_write(LAPIC_REG_SVR, LAPIC_SVR_ENABLE | LAPIC_SPURIOUS_VECTOR);
    lapic_write(LAPIC_REG_LVT_TIMER, LAPIC_LVT_MASKED);
    lapic_write(LAPIC_REG_LVT_ERROR, LAPIC_LVT_MASKED);
    lapic_write(LAPIC_REG_TIMER_DIV, LAPIC_TIMER_DIV_16);
}

static void lapic_timer_irq(interrupt_frame_t *frame)
{
    UNUSED(frame);

    lapic_send_eoi();
    sched_tick();
}

static void lapic_spurious_irq(interrupt_frame_t *frame)
{
    UNUSED(frame);
}

void lapic_init(void)
{
    if (!cpu_has_apic()) {
        kpanic("lapic: CPU does not expose a local APIC");
    }

    /* Mask the legacy 8259 PIC; all external interrupts come via LAPIC. */
    outb(0x21, 0xFF);
    outb(0xA1, 0xFF);

    if (lapic_base_phys == 0) {
        lapic_set_base_from_msr();
    }
    if (lapic_base_phys == 0) {
        kpanic("lapic: IA32_APIC_BASE reported a zero base");
    }

    lapic_map_and_enable();
    lapic_enable_core();

    idt_register_handler(LAPIC_TIMER_VECTOR, lapic_timer_irq);
    idt_register_handler(LAPIC_SPURIOUS_VECTOR, lapic_spurious_irq);

    ticks_per_ms = calibrate_ticks_per_ms();

    klog(KLOG_INFO,
         "LAPIC: base=0x%016lx version=0x%lx id=%lu, timer=%lu ticks/ms",
         (uint64_t)lapic_base_phys,
         (uint64_t)lapic_read(LAPIC_REG_VERSION),
         (uint64_t)lapic_id(), (uint64_t)ticks_per_ms);
}

void lapic_init_ap(void)
{
    if (lapic_base_phys == 0) {
        kpanic("lapic: AP started before the BSP mapped the LAPIC");
    }

    lapic_map_and_enable();
    lapic_enable_core();

    if (ticks_per_ms == 0) {
        kpanic("lapic: AP started before timer calibration");
    }
}

void lapic_timer_start(void)
{
    lapic_write(LAPIC_REG_TIMER_DIV, LAPIC_TIMER_DIV_16);
    lapic_write(LAPIC_REG_LVT_TIMER, LAPIC_TIMER_VECTOR | LAPIC_LVT_PERIODIC);
    lapic_write(LAPIC_REG_TIMER_INIT, ticks_per_ms);
}

/* Waits until the local APIC accepts a new ICR write. Bounded so a wedged
 * APIC surfaces as a warning rather than an infinite boot hang. */
static void icr_wait_ready(void)
{
    uint64_t spins = 0;

    while ((lapic_read(LAPIC_REG_ICR_LOW) & ICR_DELIVERY_PENDING) != 0) {
        if (++spins > 10000000ULL) {
            klog(KLOG_WARN, "lapic: ICR delivery stayed pending");
            return;
        }
        __asm__ volatile("pause");
    }
}

static void icr_send(uint32_t apic_id, uint32_t low)
{
    icr_wait_ready();
    lapic_write(LAPIC_REG_ICR_HIGH, apic_id << 24);
    lapic_write(LAPIC_REG_ICR_LOW, low);
}

void lapic_send_ipi(uint32_t apic_id, uint8_t vector)
{
    /* Fixed-delivery IPIs use assert level; INIT/SIPI below are edge-triggered
     * and must NOT set the level bit or the AP never leaves the wait state. */
    icr_send(apic_id, ICR_FIXED | ICR_LEVEL_ASSERT | (uint32_t)vector);
}

void lapic_broadcast_ipi_others(uint8_t vector)
{
    icr_wait_ready();
    lapic_write(LAPIC_REG_ICR_HIGH, 0);
    lapic_write(LAPIC_REG_ICR_LOW,
                0x000C0000U /* all excluding self */ | ICR_LEVEL_ASSERT |
                    (uint32_t)vector);
}

/* INIT -> 10 ms -> SIPI -> 200 us -> SIPI, per the Intel MP specification. */
/* Busy-waits for roughly `microseconds`. AP bringup runs from the boot
 * continuation context, which has no runnable peer to yield to, so a blocking
 * sleep would never return. */
static void ap_delay_us(uint32_t microseconds)
{
    uint64_t spins = (uint64_t)microseconds * 2000ULL;

    for (uint64_t i = 0; i < spins; i++) {
        __asm__ volatile("pause");
    }
}

void lapic_start_ap(uint32_t apic_id, uint32_t sipi_vector)
{
    klog(KLOG_INFO, "lapic: start_ap id=%u icr=0x%lx", apic_id,
         (uint64_t)lapic_read(LAPIC_REG_ICR_LOW));
    /* INIT and SIPI are edge-triggered: the level-assert bit must be clear. */
    icr_send(apic_id, ICR_INIT);

    klog(KLOG_INFO, "lapic: INIT written, icr=0x%lx",
         (uint64_t)lapic_read(LAPIC_REG_ICR_LOW));

    ap_delay_us(10000);

    icr_send(apic_id, ICR_STARTUP | (sipi_vector & 0xFFU));

    ap_delay_us(200);

    icr_send(apic_id, ICR_STARTUP | (sipi_vector & 0xFFU));
}
