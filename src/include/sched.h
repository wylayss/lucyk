#ifndef LUCYK_SCHED_H
#define LUCYK_SCHED_H

#include <task.h>

void sched_init(void);
void sched_start(void);
void sched_add(thread_t *thread);
void sched_tick(void);

thread_t *sched_current_thread(void);

/* Switches directly to next. The caller is expected to have set its own state
 * to a blocked state (for example THREAD_BLOCKED_REPLY). */
void sched_handoff(thread_t *next);

/* Parks the current thread in its already-set blocked state and switches to
 * the next runnable worker, or to the idle context when none is ready. */
void sched_block_current(void);

/* Makes a blocked/sleeping thread runnable again. The caller sets its
 * ipc_status / wake result before calling. */
void sched_wake(thread_t *thread);

/* Removes a non-current thread from its run queue and queues it for deferred
 * stack/thread reclamation. Used by task_terminate(). */
void sched_detach_thread(thread_t *thread);

/* Registers an AP in the scheduler and gives it an idle context. Called once
 * from ap_entry_c() before the CPU enters its idle loop. */
void sched_register_cpu(uint32_t cpu_index);

/* Idle body for an AP: halts until an interrupt arrives, then re-evaluates. */
void sched_idle_wait(void);

/* Switches an AP onto its idle context for the first time. Never returns, so
 * the bootstrap stack the AP arrived on is abandoned cleanly. */
NORETURN void sched_enter_idle(void);

/* Deferred reschedule request raised by IPI_RESCHEDULE_VECTOR. */
void sched_request_reschedule(void);

/* Diagnostics used by the boot self-tests. */
uint64_t sched_tick_count(void);
uint64_t sched_reaped_count(void);
bool sched_reap_pending(void);
task_t *sched_boot_task(void);
thread_t *sched_boot_thread(void);
uint64_t sched_boot_stack_top(void);

/* Number of worker threads that ran on each CPU; used by the SMP self-test. */
uint64_t sched_cpu_dispatch_count(uint32_t cpu_index);

/* Moves the boot/idle context from the static bootstrap stack onto its
 * guarded kernel-stack slot. Called once from kernel_main; never returns. */
NORETURN void boot_stack_enter(uint64_t stack_top, void (*entry)(void));

/* Implemented in src/arch/x86_64/switch.asm. new_cr3 == 0 keeps the current
 * address space; otherwise CR3 is reloaded and TSS.rsp0 is always updated. */
void switch_context(cpu_context_t **old_ctx, cpu_context_t *new_ctx,
                    uint64_t new_rsp0, uint64_t new_cr3);

#endif /* LUCYK_SCHED_H */
