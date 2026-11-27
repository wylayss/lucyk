#ifndef LUCYK_IO_PCI_H
#define LUCYK_IO_PCI_H

#include <types.h>

#define PCI_MAX_DEVICES 256U
#define PCI_BAR_COUNT   6U

#define PCI_VENDOR_INVALID 0xFFFFU

/* One decoded PCI function. BARs are stored as their address portion with the
 * low flag/type bits stripped; bar_io distinguishes I/O space from MMIO. */
typedef struct pci_device {
    uint8_t  bus;
    uint8_t  slot;
    uint8_t  func;
    uint8_t  header_type;
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t  class_code;
    uint8_t  subclass;
    uint8_t  prog_if;
    uint8_t  interrupt_line;
    uint8_t  interrupt_pin;
    uint64_t bar[PCI_BAR_COUNT];
    uint64_t bar_size[PCI_BAR_COUNT];
    bool     bar_io[PCI_BAR_COUNT];
    bool     bar64[PCI_BAR_COUNT]; /* slot consumed by a 64-bit BAR pair */
} pci_device_t;

/* Enumerates bus 0..255, slot 0..31 and every function of a multi-function
 * device, decoding configuration space and BARs. */
void pci_init(void);

uint8_t  pci_cfg_read8(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
uint16_t pci_cfg_read16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
uint32_t pci_cfg_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);

void pci_cfg_write8(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset,
                    uint8_t value);
void pci_cfg_write16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset,
                     uint16_t value);
void pci_cfg_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset,
                     uint32_t value);

/* First stored function matching both identifiers, or NULL. */
pci_device_t *pci_find_device(uint16_t vendor_id, uint16_t device_id);

/* Enables I/O, memory and bus-master decoding for one function. */
void pci_enable_bus_master(pci_device_t *dev);

size_t pci_device_count(void);
const pci_device_t *pci_device_at(size_t index);

/* Clears MSI/MSI-X enable for one function. */
void pci_msi_disable(uint8_t bus, uint8_t slot, uint8_t func);

/* Programs PCI MSI/MSI-X to a LAPIC vector. */
sys_status_t pci_msi_configure(uint8_t bus, uint8_t slot, uint8_t func,
                               uint8_t vector, uint32_t apic_id);

/* Kernel-owned Ring 0 device (AHCI/VirtIO-Blk); not exposed to SYS_PCI_*. */
sys_status_t pci_kernel_claim(uint8_t bus, uint8_t slot, uint8_t func);
bool pci_is_kernel_claimed(uint8_t bus, uint8_t slot, uint8_t func);

/* Claim tracking: a function+BAR may be claimed once. */
sys_status_t pci_claim_bar(uint8_t bus, uint8_t slot, uint8_t func,
                           uint32_t bar_index);
void pci_release_bar(uint8_t bus, uint8_t slot, uint8_t func,
                     uint32_t bar_index);

/* Read-only introspection helper: copies the decoded function at index into
 * *out. Returns false past the end of the enumerated list. Never touches
 * configuration space, so callers above src/io/ cannot alter device state. */
bool pci_device_snapshot(size_t index, pci_device_t *out);

#endif /* LUCYK_IO_PCI_H */