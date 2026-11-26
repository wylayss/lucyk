#ifndef LUCYK_SMP_H
#define LUCYK_SMP_H

#include <types.h>

#define IPI_RESCHEDULE_VECTOR     0xF0U
#define IPI_TLB_SHOOTDOWN_VECTOR  0xF1U

/* Result of enumerating MADT and starting every Application Processor. */
typedef struct smp_boot_report {
    uint32_t discovered;
    uint32_t started;
    uint32_t failed;
} smp_boot_report_t;

/* Discovers CPUs through ACPI and starts every AP with INIT-SIPI-SIPI.
 * Returns false only when discovery itself failed; per-AP failures are counted
 * in the report so a single unresponsive core cannot block the boot. */
bool smp_bringup(smp_boot_report_t *report);

/* Number of CPUs that reached their idle loop. */
uint32_t smp_online_count(void);

/* True when more than one CPU is participating in the scheduler. */
bool smp_multicore_active(void);

/* Copies the trampoline image to its reserved low page and registers the IPI
 * handlers. Runs on the BSP before any AP is started. */
void smp_prepare(void);

/* AP entry point referenced from the trampoline parameter block. */
void ap_entry_c(uint32_t cpu_index) NORETURN;

/* Reclaims the guarded bootstrap stack the BSP allocated for an AP. Called from
 * that AP's idle context once it has switched off the bootstrap stack; returns
 * 0 when the slot was already reclaimed. */
vaddr_t ap_bootstrap_stack_take(uint32_t cpu_index);

/* Publishes cpu_index as fully parked on its idle context. Called from the idle
 * body, which is the first point where the bootstrap stack can be recycled. */
void smp_cpu_park_ready(uint32_t cpu_index);

#endif /* LUCYK_SMP_H */
