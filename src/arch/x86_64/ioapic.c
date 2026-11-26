#include <acpi.h>
#include <io.h>
#include <ioapic.h>
#include <klog.h>
#include <spinlock.h>
#include <string.h>
#include <types.h>
#include <vspace.h>

#define IOAPIC_MMIO_VA_BASE 0xFFFF900000100000ULL
#define IOAPIC_MMIO_STRIDE  0x0000000000001000ULL

#define IOAPIC_REG_ID       0x00U
#define IOAPIC_REG_VERSION  0x01U
#define IOAPIC_REG_REDTBL   0x10U

#define IOAPIC_REDTBL_POLARITY (1U << 13)
#define IOAPIC_REDTBL_TRIGGER  (1U << 15)
#define IOAPIC_REDTBL_MASK     (1U << 16)

typedef struct ioapic_unit {
    bool     present;
    uint8_t  id;
    uint32_t gsi_base;
    uint32_t gsi_count;
    vaddr_t  regs;
} ioapic_unit_t;

static ioapic_unit_t ioapic_units[ACPI_MAX_IOAPICS];
static uint32_t ioapic_total;
static spinlock_t ioapic_lock;

static uint32_t ioapic_reg_read(const ioapic_unit_t *unit, uint32_t reg)
{
    volatile uint32_t *select =
        (volatile uint32_t *)(uintptr_t)(unit->regs + IOAPIC_REG_ID);
    volatile uint32_t *window =
        (volatile uint32_t *)(uintptr_t)(unit->regs + 0x10U);

    *select = reg;
    return *window;
}

static void ioapic_reg_write(const ioapic_unit_t *unit, uint32_t reg,
                             uint32_t value)
{
    volatile uint32_t *select =
        (volatile uint32_t *)(uintptr_t)(unit->regs + IOAPIC_REG_ID);
    volatile uint32_t *window =
        (volatile uint32_t *)(uintptr_t)(unit->regs + 0x10U);

    *select = reg;
    *window = value;
}

static ioapic_unit_t *ioapic_for_gsi(uint8_t gsi)
{
    for (uint32_t i = 0; i < ioapic_total; i++) {
        ioapic_unit_t *unit = &ioapic_units[i];

        if (!unit->present) {
            continue;
        }
        if ((uint32_t)gsi >= unit->gsi_base &&
            (uint32_t)gsi < unit->gsi_base + unit->gsi_count) {
            return unit;
        }
    }

    return NULL;
}

void ioapic_init(void)
{
    spinlock_init(&ioapic_lock);
    memset(ioapic_units, 0, sizeof(ioapic_units));
    ioapic_total = 0;

    outb(0x21, 0xFF);
    outb(0xA1, 0xFF);

    acpi_topology_t topology;

    if (!acpi_topology_cached(&topology) || topology.ioapic_count == 0) {
        klog(KLOG_WARN, "ioapic: MADT did not declare an IOAPIC");
        return;
    }

    uint32_t mapped = 0;

    for (uint32_t i = 0; i < topology.ioapic_count && i < ACPI_MAX_IOAPICS;
         i++) {
        const acpi_ioapic_t *source = &topology.ioapics[i];
        vaddr_t window = IOAPIC_MMIO_VA_BASE + ((vaddr_t)i * IOAPIC_MMIO_STRIDE);
        paddr_t aligned = PAGE_ALIGN_DOWN(source->mmio_base);

        if (source->mmio_base == 0) {
            klog(KLOG_WARN, "ioapic: unit %u has a zero MMIO base",
                 (uint64_t)i);
            continue;
        }

        if (vspace_map_kernel(window, aligned,
                              VSPACE_PROT_WRITE | VSPACE_PROT_PCD |
                                  VSPACE_PROT_PWT) != SYS_OK) {
            klog(KLOG_WARN, "ioapic: cannot map MMIO base 0x%08lx",
                 (uint64_t)source->mmio_base);
            continue;
        }

        ioapic_unit_t *unit = &ioapic_units[mapped];

        unit->present = true;
        unit->id = source->id;
        unit->gsi_base = source->gsi_base;
        unit->regs = window + (source->mmio_base & PAGE_MASK);

        uint32_t version = ioapic_reg_read(unit, IOAPIC_REG_VERSION);
        unit->gsi_count = ((version >> 16) & 0xFFU) + 1U;

        for (uint32_t pin = 0; pin < unit->gsi_count; pin++) {
            ioapic_reg_write(unit, IOAPIC_REG_REDTBL + (pin * 2U) + 1U, 0);
            ioapic_reg_write(unit, IOAPIC_REG_REDTBL + (pin * 2U),
                             IOAPIC_REDTBL_MASK);
        }

        mapped++;

        klog(KLOG_INFO,
             "ioapic: unit %u id=%u base=0x%08lx gsi=%lu..%lu mmio=0x%lx",
             (uint64_t)(mapped - 1U), (uint64_t)unit->id,
             (uint64_t)source->mmio_base,
             (uint64_t)unit->gsi_base,
             (uint64_t)(unit->gsi_base + unit->gsi_count - 1U),
             (uint64_t)unit->regs);
    }

    ioapic_total = mapped;

    klog(KLOG_INFO,
         "ioapic: %lu unit(s) online, %lu source override(s), 8259 masked",
         (uint64_t)mapped, (uint64_t)topology.iso_count);

    for (uint32_t i = 0; i < topology.iso_count; i++) {
        const acpi_iso_t *iso = &topology.isos[i];

        klog(KLOG_DEBUG,
             "ioapic: iso bus=%u irq=%u -> gsi=%lu flags=0x%04lx",
             (uint64_t)iso->bus, (uint64_t)iso->source_irq,
             (uint64_t)iso->gsi, (uint64_t)iso->flags);
    }
}

bool ioapic_available(void)
{
    return ioapic_total != 0;
}

uint32_t ioapic_unit_count(void)
{
    return ioapic_total;
}

sys_status_t ioapic_route_irq(uint8_t gsi, uint8_t vector,
                              uint32_t dest_lapic_id, bool active_low,
                              bool level_triggered)
{
    if (vector < IOAPIC_VECTOR_MIN || vector > IOAPIC_VECTOR_MAX) {
        return SYS_ERR_INVAL;
    }
    if (dest_lapic_id > 0xFFU) {
        return SYS_ERR_INVAL;
    }

    uint64_t flags = spin_lock_irqsave(&ioapic_lock);
    ioapic_unit_t *unit = ioapic_for_gsi(gsi);

    if (unit == NULL) {
        spin_unlock_irqrestore(&ioapic_lock, flags);
        return SYS_ERR_INVAL;
    }

    uint32_t pin = (uint32_t)gsi - unit->gsi_base;
    uint32_t low = (uint32_t)vector;

    /* Fixed delivery, physical destination, unmasked. */
    if (active_low) {
        low |= IOAPIC_REDTBL_POLARITY;
    }
    if (level_triggered) {
        low |= IOAPIC_REDTBL_TRIGGER;
    }

    uint32_t high = (dest_lapic_id & 0xFFU) << 24;

    /* The high word must be stable before the low word commits the entry. */
    ioapic_reg_write(unit, IOAPIC_REG_REDTBL + (pin * 2U) + 1U, high);
    ioapic_reg_write(unit, IOAPIC_REG_REDTBL + (pin * 2U), low);

    spin_unlock_irqrestore(&ioapic_lock, flags);
    return SYS_OK;
}

void ioapic_unmask_irq(uint8_t gsi)
{
    uint64_t flags = spin_lock_irqsave(&ioapic_lock);
    ioapic_unit_t *unit = ioapic_for_gsi(gsi);

    if (unit != NULL) {
        uint32_t pin = (uint32_t)gsi - unit->gsi_base;
        uint32_t low =
            ioapic_reg_read(unit, IOAPIC_REG_REDTBL + (pin * 2U));

        low &= ~IOAPIC_REDTBL_MASK;
        ioapic_reg_write(unit, IOAPIC_REG_REDTBL + (pin * 2U), low);
    }

    spin_unlock_irqrestore(&ioapic_lock, flags);
}

void ioapic_mask_irq(uint8_t gsi)
{
    uint64_t flags = spin_lock_irqsave(&ioapic_lock);
    ioapic_unit_t *unit = ioapic_for_gsi(gsi);

    if (unit != NULL) {
        uint32_t pin = (uint32_t)gsi - unit->gsi_base;
        uint32_t low =
            ioapic_reg_read(unit, IOAPIC_REG_REDTBL + (pin * 2U));

        low |= IOAPIC_REDTBL_MASK;
        ioapic_reg_write(unit, IOAPIC_REG_REDTBL + (pin * 2U), low);
    }

    spin_unlock_irqrestore(&ioapic_lock, flags);
}

bool ioapic_resolve_irq(uint8_t irq, uint8_t *gsi_out, bool *active_low_out,
                        bool *level_triggered_out)
{
    if (gsi_out == NULL || active_low_out == NULL ||
        level_triggered_out == NULL) {
        return false;
    }

    acpi_topology_t topology;

    if (acpi_topology_cached(&topology)) {
        for (uint32_t i = 0; i < topology.iso_count; i++) {
            const acpi_iso_t *iso = &topology.isos[i];

            if (iso->source_irq != irq) {
                continue;
            }
            if (iso->gsi > 0xFFU) {
                return false;
            }

            *gsi_out = (uint8_t)iso->gsi;

            /* ISO flags use the MPS INTI encoding: bits 0-1 carry polarity
             * (3 = active low) and bits 2-3 carry trigger mode (3 = level). */
            *active_low_out = (iso->flags & 0x3U) == 0x3U;
            *level_triggered_out = ((iso->flags >> 2) & 0x3U) == 0x3U;
            return true;
        }
    }

    *gsi_out = irq;

    /* An IRQ with no override follows the ISA defaults below GSI 16. Above
     * that it can only come from a PCI INTx line, which is level/low. */
    *active_low_out = irq >= 16U;
    *level_triggered_out = irq >= 16U;
    return true;
}