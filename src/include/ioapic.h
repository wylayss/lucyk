#ifndef LUCYK_IOAPIC_H
#define LUCYK_IOAPIC_H

#include <types.h>

#define IOAPIC_VECTOR_MIN 32U
#define IOAPIC_VECTOR_MAX 254U

/* Masks the legacy 8259 pair, maps every MADT IOAPIC MMIO window and masks all
 * redirection entries. Safe to call once the ACPI topology is cached. */
void ioapic_init(void);
bool ioapic_available(void);
uint32_t ioapic_unit_count(void);

/* Programs the redirection entry that owns gsi and leaves it unmasked. The
 * destination is a physical LAPIC id in the low 8 bits. */
sys_status_t ioapic_route_irq(uint8_t gsi, uint8_t vector,
                              uint32_t dest_lapic_id, bool active_low,
                              bool level_triggered);

void ioapic_unmask_irq(uint8_t gsi);
void ioapic_mask_irq(uint8_t gsi);

/* Claims gsi for a capability-owned IRQ (CAP_OBJ_IRQ) and programs its
 * redirection entry to deliver `vector` to dest_lapic_id, leaving it MASKED:
 * the line is only unmasked once a notification is bound. Returns
 * SYS_ERR_BUSY when the GSI is already routed (by a kernel driver through
 * ioapic_route_irq() or by another capability) and SYS_ERR_INVAL when no
 * IOAPIC serves it. */
sys_status_t ioapic_bind_cap_irq(uint8_t gsi, uint8_t vector,
                                 uint32_t dest_lapic_id, bool active_low,
                                 bool level_triggered);

/* Masks a capability-owned GSI and releases the claim. */
void ioapic_unbind_cap_irq(uint8_t gsi);

/* Reports the redirection entry's mask bit (true when no IOAPIC serves gsi). */
bool ioapic_irq_is_masked(uint8_t gsi);

/* Resolves a legacy IRQ number through the MADT Interrupt Source Overrides.
 * An IRQ with no override maps to the identity GSI with ISA defaults. */
bool ioapic_resolve_irq(uint8_t irq, uint8_t *gsi_out, bool *active_low_out,
                        bool *level_triggered_out);

#endif /* LUCYK_IOAPIC_H */