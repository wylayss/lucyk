#include <io.h>
#include <io/pci.h>
#include <klog.h>
#include <spinlock.h>
#include <string.h>
#include <types.h>

#define PCI_CONFIG_ADDRESS 0xCF8U
#define PCI_CONFIG_DATA    0xCFCU

#define PCI_REG_VENDOR       0x00U
#define PCI_REG_DEVICE       0x02U
#define PCI_REG_COMMAND      0x04U
#define PCI_REG_REVISION     0x08U
#define PCI_REG_PROG_IF      0x09U
#define PCI_REG_SUBCLASS     0x0AU
#define PCI_REG_CLASS        0x0BU
#define PCI_REG_HEADER_TYPE  0x0EU
#define PCI_REG_BAR0         0x10U
#define PCI_REG_INTERRUPT    0x3CU

#define PCI_HEADER_TYPE_MASK 0x7FU
#define PCI_HEADER_NORMAL    0x00U
#define PCI_HEADER_MULTI     0x80U

#define PCI_CMD_IO          (1U << 0)
#define PCI_CMD_MEMORY      (1U << 1)
#define PCI_CMD_BUS_MASTER  (1U << 2)

static spinlock_t pci_cfg_lock;
static pci_device_t pci_devices[PCI_MAX_DEVICES];
static size_t pci_count;

static uint32_t cfg_address(uint8_t bus, uint8_t slot, uint8_t func,
                            uint8_t offset)
{
    return 0x80000000U |
           ((uint32_t)bus << 16) |
           ((uint32_t)slot << 11) |
           ((uint32_t)func << 8) |
           ((uint32_t)offset & 0xFCU);
}

uint32_t pci_cfg_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset)
{
    uint64_t flags = spin_lock_irqsave(&pci_cfg_lock);

    outl(PCI_CONFIG_ADDRESS, cfg_address(bus, slot, func, offset));
    uint32_t value = inl(PCI_CONFIG_DATA);

    spin_unlock_irqrestore(&pci_cfg_lock, flags);
    return value;
}

uint16_t pci_cfg_read16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset)
{
    uint64_t flags = spin_lock_irqsave(&pci_cfg_lock);

    outl(PCI_CONFIG_ADDRESS, cfg_address(bus, slot, func, offset));
    uint16_t value = inw((uint16_t)(PCI_CONFIG_DATA + (offset & 0x2U)));

    spin_unlock_irqrestore(&pci_cfg_lock, flags);
    return value;
}

uint8_t pci_cfg_read8(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset)
{
    uint64_t flags = spin_lock_irqsave(&pci_cfg_lock);

    outl(PCI_CONFIG_ADDRESS, cfg_address(bus, slot, func, offset));
    uint8_t value = inb((uint16_t)(PCI_CONFIG_DATA + (offset & 0x3U)));

    spin_unlock_irqrestore(&pci_cfg_lock, flags);
    return value;
}

void pci_cfg_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset,
                     uint32_t value)
{
    uint64_t flags = spin_lock_irqsave(&pci_cfg_lock);

    outl(PCI_CONFIG_ADDRESS, cfg_address(bus, slot, func, offset));
    outl(PCI_CONFIG_DATA, value);

    spin_unlock_irqrestore(&pci_cfg_lock, flags);
}

void pci_cfg_write16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset,
                     uint16_t value)
{
    uint64_t flags = spin_lock_irqsave(&pci_cfg_lock);

    outl(PCI_CONFIG_ADDRESS, cfg_address(bus, slot, func, offset));
    outw((uint16_t)(PCI_CONFIG_DATA + (offset & 0x2U)), value);

    spin_unlock_irqrestore(&pci_cfg_lock, flags);
}

void pci_cfg_write8(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset,
                    uint8_t value)
{
    uint64_t flags = spin_lock_irqsave(&pci_cfg_lock);

    outl(PCI_CONFIG_ADDRESS, cfg_address(bus, slot, func, offset));
    outb((uint16_t)(PCI_CONFIG_DATA + (offset & 0x3U)), value);

    spin_unlock_irqrestore(&pci_cfg_lock, flags);
}

/* Writes the probe mask and restores the original value. The caller has
 * already disabled I/O and memory decoding. */
static void decode_bar(pci_device_t *dev, size_t index)
{
    uint8_t offset = (uint8_t)(PCI_REG_BAR0 + (index * 4U));
    uint32_t original = pci_cfg_read32(dev->bus, dev->slot, dev->func, offset);

    dev->bar[index] = original;
    dev->bar_size[index] = 0;
    dev->bar_io[index] = (original & 1U) != 0;

    if (original == 0 || original == 0xFFFFFFFFU) {
        if (original == 0xFFFFFFFFU) {
            dev->bar[index] = 0;
            dev->bar_io[index] = false;
        }
        return;
    }

    if (dev->bar_io[index]) {
        pci_cfg_write32(dev->bus, dev->slot, dev->func, offset, 0xFFFFFFFFU);
        uint32_t probe = pci_cfg_read32(dev->bus, dev->slot, dev->func, offset);
        pci_cfg_write32(dev->bus, dev->slot, dev->func, offset, original);

        uint64_t size = (~(uint64_t)(probe & 0xFFFFFFFCU) + 1ULL) & 0xFFFFULL;

        dev->bar[index] = original & 0xFFFFFFFCU;
        dev->bar_size[index] = size;
        return;
    }

    uint32_t type = original & 0x6U;

    if (type == 0x4U && index < (PCI_BAR_COUNT - 1U)) {
        uint8_t high_offset = (uint8_t)(offset + 4U);
        uint32_t original_high =
            pci_cfg_read32(dev->bus, dev->slot, dev->func, high_offset);

        pci_cfg_write32(dev->bus, dev->slot, dev->func, offset, 0xFFFFFFFFU);
        pci_cfg_write32(dev->bus, dev->slot, dev->func, high_offset,
                        0xFFFFFFFFU);

        uint32_t probe_low =
            pci_cfg_read32(dev->bus, dev->slot, dev->func, offset);
        uint32_t probe_high =
            pci_cfg_read32(dev->bus, dev->slot, dev->func, high_offset);

        pci_cfg_write32(dev->bus, dev->slot, dev->func, offset, original);
        pci_cfg_write32(dev->bus, dev->slot, dev->func, high_offset,
                        original_high);

        uint64_t mask = ((uint64_t)probe_high << 32) |
                        (uint64_t)(probe_low & 0xFFFFFFF0U);
        uint64_t size = ~mask + 1ULL;

        dev->bar[index] = ((uint64_t)original_high << 32) |
                          (uint64_t)(original & 0xFFFFFFF0U);
        dev->bar_size[index] = size;

        dev->bar_io[index + 1U] = false;
        dev->bar[index + 1U] = 0;
        dev->bar_size[index + 1U] = 0;
        return;
    }

    pci_cfg_write32(dev->bus, dev->slot, dev->func, offset, 0xFFFFFFFFU);
    uint32_t probe = pci_cfg_read32(dev->bus, dev->slot, dev->func, offset);
    pci_cfg_write32(dev->bus, dev->slot, dev->func, offset, original);

    uint64_t mask = (uint64_t)(probe & 0xFFFFFFF0U);

    dev->bar[index] = original & 0xFFFFFFF0U;
    dev->bar_size[index] = (~mask + 1ULL) & 0xFFFFFFFFULL;
}

static void decode_bars(pci_device_t *dev)
{
    if ((dev->header_type & PCI_HEADER_TYPE_MASK) != PCI_HEADER_NORMAL) {
        return;
    }

    uint16_t command =
        pci_cfg_read16(dev->bus, dev->slot, dev->func, PCI_REG_COMMAND);
    pci_cfg_write16(dev->bus, dev->slot, dev->func, PCI_REG_COMMAND,
                    (uint16_t)(command & (uint16_t)~(PCI_CMD_IO |
                                                       PCI_CMD_MEMORY |
                                                       PCI_CMD_BUS_MASTER)));

    size_t index = 0;

    while (index < PCI_BAR_COUNT) {
        uint8_t offset = (uint8_t)(PCI_REG_BAR0 + (index * 4U));
        uint32_t original =
            pci_cfg_read32(dev->bus, dev->slot, dev->func, offset);
        bool is_64bit = (original & 1U) == 0 &&
                        (original & 0x6U) == 0x4U &&
                        index < (PCI_BAR_COUNT - 1U);

        decode_bar(dev, index);
        index += is_64bit ? 2U : 1U;
    }

    pci_cfg_write16(dev->bus, dev->slot, dev->func, PCI_REG_COMMAND, command);
}

static void add_function(uint8_t bus, uint8_t slot, uint8_t func)
{
    uint16_t vendor =
        pci_cfg_read16(bus, slot, func, PCI_REG_VENDOR);

    if (vendor == PCI_VENDOR_INVALID || pci_count >= PCI_MAX_DEVICES) {
        return;
    }

    pci_device_t *dev = &pci_devices[pci_count];
    memset(dev, 0, sizeof(*dev));

    dev->bus = bus;
    dev->slot = slot;
    dev->func = func;
    dev->vendor_id = vendor;
    dev->device_id =
        pci_cfg_read16(bus, slot, func, PCI_REG_DEVICE);
    dev->class_code =
        pci_cfg_read8(bus, slot, func, PCI_REG_CLASS);
    dev->subclass =
        pci_cfg_read8(bus, slot, func, PCI_REG_SUBCLASS);
    dev->header_type =
        pci_cfg_read8(bus, slot, func, PCI_REG_HEADER_TYPE);
    dev->interrupt_line =
        pci_cfg_read8(bus, slot, func, PCI_REG_INTERRUPT);
    dev->interrupt_pin =
        pci_cfg_read8(bus, slot, func, (uint8_t)(PCI_REG_INTERRUPT + 1U));

    decode_bars(dev);

    pci_count++;
}

void pci_init(void)
{
    spinlock_init(&pci_cfg_lock);
    memset(pci_devices, 0, sizeof(pci_devices));
    pci_count = 0;

    for (uint32_t bus = 0; bus < 256U; bus++) {
        for (uint32_t slot = 0; slot < 32U; slot++) {
            uint8_t b = (uint8_t)bus;
            uint8_t s = (uint8_t)slot;

            if (pci_cfg_read16(b, s, 0, PCI_REG_VENDOR) ==
                PCI_VENDOR_INVALID) {
                continue;
            }

            add_function(b, s, 0);

            uint8_t header =
                pci_cfg_read8(b, s, 0, PCI_REG_HEADER_TYPE);

            if ((header & PCI_HEADER_MULTI) == 0) {
                continue;
            }

            for (uint8_t func = 1; func < 8; func++) {
                if (pci_cfg_read16(b, s, func, PCI_REG_VENDOR) !=
                    PCI_VENDOR_INVALID) {
                    add_function(b, s, func);
                }
            }
        }
    }

    klog(KLOG_INFO, "pci: enumerated %lu function(s) across 256 buses",
         (uint64_t)pci_count);

    for (size_t i = 0; i < pci_count; i++) {
        const pci_device_t *dev = &pci_devices[i];

        klog(KLOG_DEBUG,
             "pci: %02x:%02x.%x vendor=%04x device=%04x class=%02x:%02x irq=%u pin=%u",
             (uint64_t)dev->bus, (uint64_t)dev->slot, (uint64_t)dev->func,
             (uint64_t)dev->vendor_id, (uint64_t)dev->device_id,
             (uint64_t)dev->class_code, (uint64_t)dev->subclass,
             (uint64_t)dev->interrupt_line, (uint64_t)dev->interrupt_pin);
    }
}

pci_device_t *pci_find_device(uint16_t vendor_id, uint16_t device_id)
{
    for (size_t i = 0; i < pci_count; i++) {
        if (pci_devices[i].vendor_id == vendor_id &&
            pci_devices[i].device_id == device_id) {
            return &pci_devices[i];
        }
    }

    return NULL;
}

void pci_enable_bus_master(pci_device_t *dev)
{
    if (dev == NULL) {
        return;
    }

    uint16_t command =
        pci_cfg_read16(dev->bus, dev->slot, dev->func, PCI_REG_COMMAND);

    command = (uint16_t)(command | PCI_CMD_IO | PCI_CMD_MEMORY |
                         PCI_CMD_BUS_MASTER);

    pci_cfg_write16(dev->bus, dev->slot, dev->func, PCI_REG_COMMAND, command);
}

size_t pci_device_count(void)
{
    return pci_count;
}

const pci_device_t *pci_device_at(size_t index)
{
    if (index >= pci_count) {
        return NULL;
    }

    return &pci_devices[index];
}