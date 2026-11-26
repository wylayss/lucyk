#ifndef LUCYK_LAPIC_H
#define LUCYK_LAPIC_H

#include <types.h>

#define LAPIC_TIMER_VECTOR 0x20

/* BSP path: masks the 8259 PIC, maps the LAPIC MMIO page, enables the APIC,
 * calibrates the timer against the PIT and registers the timer handler. */
void lapic_init(void);

/* AP path: maps the same LAPIC page, enables the APIC and copies the
 * calibrated tick count from the BSP. No PIT calibration is performed. */
void lapic_init_ap(void);

/* Per-CPU: starts a 1 ms periodic preemption timer on LAPIC_TIMER_VECTOR. */
void lapic_timer_start(void);

/* Acknowledges the current in-service interrupt on the local APIC. */
void lapic_send_eoi(void);

/* Physical base address discovered through ACPI; falls back to IA32_APIC_BASE
 * when the firmware did not publish a valid MADT. */
void lapic_set_base(uint32_t phys_base);
uint32_t lapic_set_base_from_msr(void);

/* Local APIC id of the calling CPU. */
uint32_t lapic_id(void);

/* Targeted and broadcast IPIs. The ICR delivery is level-triggered with a
 * bounded wait for acceptance. */
void lapic_send_ipi(uint32_t apic_id, uint8_t vector);
void lapic_broadcast_ipi_others(uint8_t vector);

/* INIT-SIPI-SIPI sequencing for one AP; the trampoline handles the rest. */
void lapic_start_ap(uint32_t apic_id, uint32_t sipi_vector);

/* Millisecond calibration shared by every CPU. */
uint32_t lapic_ticks_per_ms(void);

#endif /* LUCYK_LAPIC_H */
