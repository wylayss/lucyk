#ifndef LUCYK_ACPI_H
#define LUCYK_ACPI_H

#include <types.h>

#define ACPI_MAX_IOAPICS 4U
#define ACPI_MAX_ISO     16U

/* One IOAPIC discovered through the MADT: its MMIO window and the first GSI
 * that it owns. */
typedef struct acpi_ioapic {
    uint8_t  id;
    uint32_t mmio_base;
    uint32_t gsi_base;
} acpi_ioapic_t;

/* Interrupt Source Override: an ISA IRQ that is wired to a different GSI, with
 * the MADT polarity/trigger flags preserved verbatim. */
typedef struct acpi_iso {
    uint8_t  bus;
    uint8_t  source_irq;
    uint32_t gsi;
    uint16_t flags;
} acpi_iso_t;

/* Result of walking the RSDP -> RSDT/XSDT -> MADT chain. */
typedef struct acpi_topology {
    uint32_t local_apic_base;
    uint32_t cpu_count;
    uint32_t apic_ids[16];
    uint32_t ioapic_count;
    acpi_ioapic_t ioapics[ACPI_MAX_IOAPICS];
    uint32_t iso_count;
    acpi_iso_t isos[ACPI_MAX_ISO];
    bool     valid;
} acpi_topology_t;

/* Locates the RSDP through the bootloader request, validates every checksum on
 * the path, and parses the MADT for enabled CPUs, the LAPIC MMIO base, IOAPIC
 * units and Interrupt Source Overrides. Returns false if any table is missing
 * or corrupt. */
bool acpi_discover_topology(acpi_topology_t *out);

/* Both helpers run while the firmware tables are still mapped during early
 * boot. They avoid re-reading ACPI memory after the allocator has reserved it. */
void acpi_cache_topology(void);
bool acpi_topology_cached(acpi_topology_t *out);

#endif /* LUCYK_ACPI_H */