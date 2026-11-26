#include <acpi.h>
#include <klog.h>
#include <limine.h>
#include <string.h>
#include <types.h>
#include <vspace.h>

extern volatile struct limine_rsdp_request rsdp_request;
extern volatile struct limine_hhdm_request hhdm_request;

#define ACPI_RSDP_SIG "RSD PTR "
#define MADT_SIGNATURE "APIC"

#define MADT_TYPE_LOCAL_APIC        0U
#define MADT_TYPE_IOAPIC            1U
#define MADT_TYPE_ISO               2U
#define MADT_TYPE_LAPIC_OVERRIDE    5U
#define MADT_FLAG_ENABLED           0x1U

/* Firmware tables can sit in legacy holes the direct map does not cover, so
 * each one is mapped into a dedicated window before it is parsed. Windows are
 * released once discovery finishes. */
#define ACPI_WINDOW_BASE   0xFFFF900000002000ULL
#define ACPI_WINDOW_STRIDE 0x0000000000004000ULL
#define ACPI_WINDOW_MAX    32U
#define ACPI_WINDOW_PAGES  2U
#define ACPI_MAX_TABLE_BYTES (1024U * 1024U)

typedef struct PACKED acpi_rsdp {
    char     signature[8];
    uint8_t  checksum;
    char     oem_id[6];
    uint8_t  revision;
    uint32_t rsdt_address;
} acpi_rsdp_t;

typedef struct PACKED acpi_rsdp_v2 {
    acpi_rsdp_t v1;
    uint32_t length;
    uint64_t xsdt_address;
    uint8_t  extended_checksum;
    uint8_t  reserved[3];
} acpi_rsdp_v2_t;

typedef struct PACKED acpi_sdt_header {
    char     signature[4];
    uint32_t length;
    uint8_t  revision;
    uint8_t  checksum;
    char     oem_id[6];
    char     oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} acpi_sdt_header_t;

typedef struct PACKED madt_local_apic {
    uint8_t  type;
    uint8_t  length;
    uint8_t  acpi_processor_id;
    uint8_t  apic_id;
    uint32_t flags;
} madt_local_apic_t;

typedef struct PACKED madt_ioapic {
    uint8_t  type;
    uint8_t  length;
    uint8_t  id;
    uint8_t  reserved;
    uint32_t mmio_base;
    uint32_t gsi_base;
} madt_ioapic_t;

typedef struct PACKED madt_iso {
    uint8_t  type;
    uint8_t  length;
    uint8_t  bus;
    uint8_t  source_irq;
    uint32_t gsi;
    uint16_t flags;
} madt_iso_t;

typedef struct PACKED madt_lapic_override {
    uint8_t  type;
    uint8_t  length;
    uint16_t reserved;
    uint32_t mmio_base;
} madt_lapic_override_t;

static acpi_topology_t cached_topology;
static bool cached_valid;

static uint32_t window_count;

static void acpi_windows_release(void)
{
    for (uint32_t i = 0; i < window_count; i++) {
        vaddr_t base = ACPI_WINDOW_BASE + ((vaddr_t)i * ACPI_WINDOW_STRIDE);

        for (size_t page = 0; page < 2; page++) {
            (void)vspace_unmap_kernel(base + page * PAGE_SIZE);
        }
    }
    window_count = 0;
}

static bool checksum_ok(const void *base, size_t length)
{
    const uint8_t *bytes = (const uint8_t *)base;
    uint8_t sum = 0;

    for (size_t i = 0; i < length; i++) {
        sum = (uint8_t)(sum + bytes[i]);
    }
    return sum == 0;
}

static bool signature_matches(const char *actual, const char *expected,
                              size_t length)
{
    return memcmp(actual, expected, length) == 0;
}

/* Returns a readable pointer to `phys`, mapping a window when the direct map
 * does not cover it. Two pages are mapped so a full header is always legible. */
static const void *table_from_physical(paddr_t phys, size_t *limit_out)
{
    if (phys == 0) {
        return NULL;
    }

    uint64_t hhdm = hhdm_request.response != NULL
                        ? hhdm_request.response->offset
                        : 0;

    bool direct = hhdm != 0 && phys >= 0x0000000000100000ULL &&
                  phys < 0x000000001F000000ULL;

    if (direct) {
        if (limit_out != NULL) {
            *limit_out = ACPI_MAX_TABLE_BYTES;
        }
        return (const void *)(uintptr_t)(phys + hhdm);
    }

    if (window_count >= ACPI_WINDOW_MAX) {
        return NULL;
    }

    vaddr_t base = ACPI_WINDOW_BASE + ((vaddr_t)window_count *
                                       ACPI_WINDOW_STRIDE);
    uint64_t aligned = phys & ~(PAGE_SIZE - 1ULL);
    uint64_t page_offset = phys & (PAGE_SIZE - 1ULL);

    if (vspace_map_kernel(base, aligned, 0) != SYS_OK) {
        return NULL;
    }
    if (vspace_map_kernel(base + PAGE_SIZE, aligned + PAGE_SIZE, 0) != SYS_OK) {
        (void)vspace_unmap_kernel(base);
        return NULL;
    }

    window_count++;

    if (limit_out != NULL) {
        *limit_out = ((size_t)ACPI_WINDOW_PAGES * PAGE_SIZE) -
                     (size_t)page_offset;
    }

    return (const void *)(uintptr_t)(base + page_offset);
}

static bool parse_madt(const acpi_sdt_header_t *madt, acpi_topology_t *out)
{
    if (!signature_matches(madt->signature, MADT_SIGNATURE, 4)) {
        return false;
    }
    if (madt->length < sizeof(acpi_sdt_header_t) + 8) {
        return false;
    }
    if (!checksum_ok(madt, madt->length)) {
        klog(KLOG_WARN, "acpi: MADT checksum mismatch");
        return false;
    }

    const uint8_t *base = (const uint8_t *)madt;
    const uint8_t *cursor = base + sizeof(acpi_sdt_header_t);

    /* The MADT payload starts with the local APIC address and flags. */
    uint32_t lapic_base = 0;
    memcpy(&lapic_base, cursor, sizeof(lapic_base));
    cursor += 8;

    if (lapic_base != 0) {
        out->local_apic_base = lapic_base;
    }

    const uint8_t *end = base + madt->length;

    while (cursor + 2 <= end) {
        const madt_local_apic_t *entry = (const madt_local_apic_t *)cursor;

        if (entry->length < 2 || cursor + entry->length > end) {
            return false;
        }

        if (entry->type == MADT_TYPE_LOCAL_APIC &&
            entry->length >= sizeof(madt_local_apic_t)) {
            if ((entry->flags & MADT_FLAG_ENABLED) != 0 &&
                out->cpu_count < ARRAY_SIZE(out->apic_ids)) {
                out->apic_ids[out->cpu_count++] = entry->apic_id;
            }
        } else if (entry->type == MADT_TYPE_IOAPIC &&
                   entry->length >= sizeof(madt_ioapic_t)) {
            const madt_ioapic_t *ioapic = (const madt_ioapic_t *)cursor;

            if (out->ioapic_count < ACPI_MAX_IOAPICS) {
                acpi_ioapic_t *slot = &out->ioapics[out->ioapic_count++];

                slot->id = ioapic->id;
                slot->mmio_base = ioapic->mmio_base;
                slot->gsi_base = ioapic->gsi_base;
            }
        } else if (entry->type == MADT_TYPE_ISO &&
                   entry->length >= sizeof(madt_iso_t)) {
            const madt_iso_t *iso = (const madt_iso_t *)cursor;

            if (out->iso_count < ACPI_MAX_ISO) {
                acpi_iso_t *slot = &out->isos[out->iso_count++];

                slot->bus = iso->bus;
                slot->source_irq = iso->source_irq;
                slot->gsi = iso->gsi;
                slot->flags = iso->flags;
            }
        } else if (entry->type == MADT_TYPE_LAPIC_OVERRIDE &&
                   entry->length >= sizeof(madt_lapic_override_t)) {
            const madt_lapic_override_t *override =
                (const madt_lapic_override_t *)cursor;

            if (override->mmio_base != 0) {
                out->local_apic_base = override->mmio_base;
            }
        }

        cursor += entry->length;
    }

    return true;
}

bool acpi_discover_topology(acpi_topology_t *out)
{
    if (out == NULL) {
        return false;
    }

    memset(out, 0, sizeof(*out));

    struct limine_rsdp_response *response = rsdp_request.response;

    if (response == NULL || response->address == 0) {
        klog(KLOG_WARN, "acpi: bootloader provided no RSDP");
        return false;
    }

    size_t rsdp_limit = 0;
    const acpi_rsdp_t *rsdp = (const acpi_rsdp_t *)table_from_physical(
        (paddr_t)response->address, &rsdp_limit);

    if (rsdp == NULL) {
        klog(KLOG_WARN, "acpi: cannot map the RSDP");
        return false;
    }
    if (!signature_matches(rsdp->signature, ACPI_RSDP_SIG, 8)) {
        klog(KLOG_WARN, "acpi: RSDP signature mismatch");
        return false;
    }
    if (!checksum_ok(rsdp, sizeof(acpi_rsdp_t))) {
        klog(KLOG_WARN, "acpi: RSDP checksum mismatch");
        return false;
    }

    const acpi_sdt_header_t *root = NULL;
    size_t root_limit = 0;

    if (rsdp->revision >= 2) {
        const acpi_rsdp_v2_t *rsdp_v2 = (const acpi_rsdp_v2_t *)rsdp;

        if (rsdp_v2->length < sizeof(acpi_rsdp_v2_t) ||
            rsdp_v2->length > rsdp_limit ||
            !checksum_ok(rsdp_v2, rsdp_v2->length)) {
            klog(KLOG_WARN, "acpi: RSDP v2 extended checksum mismatch");
            return false;
        }
        if (rsdp_v2->xsdt_address != 0) {
            root = (const acpi_sdt_header_t *)table_from_physical(
                (paddr_t)rsdp_v2->xsdt_address, &root_limit);
        }
    }

    if (root == NULL && rsdp->rsdt_address != 0) {
        root = (const acpi_sdt_header_t *)table_from_physical(
            (paddr_t)rsdp->rsdt_address, &root_limit);
    }

    if (root == NULL) {
        klog(KLOG_WARN, "acpi: no RSDT or XSDT present");
        return false;
    }

    bool is_xsdt = signature_matches(root->signature, "XSDT", 4);
    bool is_rsdt = signature_matches(root->signature, "RSDT", 4);

    if (!is_xsdt && !is_rsdt) {
        klog(KLOG_WARN, "acpi: root table signature is neither RSDT nor XSDT");
        return false;
    }
    if (root->length < sizeof(acpi_sdt_header_t) ||
        root->length > root_limit ||
        !checksum_ok(root, root->length)) {
        klog(KLOG_WARN, "acpi: root table checksum mismatch");
        return false;
    }

    size_t entry_size = is_xsdt ? sizeof(uint64_t) : sizeof(uint32_t);
    size_t payload = root->length - sizeof(acpi_sdt_header_t);
    size_t entries = payload / entry_size;
    const uint8_t *cursor =
        (const uint8_t *)root + sizeof(acpi_sdt_header_t);

    for (size_t i = 0; i < entries; i++) {
        paddr_t table_phys;

        if (is_xsdt) {
            uint64_t address;
            memcpy(&address, cursor + (i * entry_size), sizeof(address));
            table_phys = (paddr_t)address;
        } else {
            uint32_t address;
            memcpy(&address, cursor + (i * entry_size), sizeof(address));
            table_phys = (paddr_t)address;
        }

        if (table_phys == 0) {
            continue;
        }

        size_t table_limit = 0;
        const acpi_sdt_header_t *table =
            (const acpi_sdt_header_t *)table_from_physical(table_phys,
                                                           &table_limit);

        if (table == NULL) {
            continue;
        }
        if (table->length < sizeof(acpi_sdt_header_t) ||
            table->length > table_limit) {
            continue;
        }

        if (signature_matches(table->signature, MADT_SIGNATURE, 4)) {
            if (!parse_madt(table, out)) {
                return false;
            }
            out->valid = out->cpu_count > 0;
            return out->valid;
        }
    }

    klog(KLOG_WARN, "acpi: MADT table was not found");
    return false;
}

void acpi_cache_topology(void)
{
    acpi_topology_t topology;

    if (acpi_discover_topology(&topology)) {
        cached_topology = topology;
        cached_valid = true;
    }

    /* Every table has been parsed; release the temporary windows. */
    acpi_windows_release();
}

bool acpi_topology_cached(acpi_topology_t *out)
{
    if (out == NULL || !cached_valid) {
        return false;
    }

    *out = cached_topology;
    return true;
}
