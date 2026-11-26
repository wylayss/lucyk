#ifndef LUCYK_MK_HOOKS_H
#define LUCYK_MK_HOOKS_H

#include <syscall.h>
#include <task.h>
#include <types.h>

/* Personality-layer integration contract (see ARCHITECTURE.md). The syscall
 * number remains available in frame->rax on entry. */
typedef struct mk_hooks {
    void (*on_personality_syscall)(thread_t *caller, syscall_frame_t *frame);

    bool (*on_user_exception)(thread_t *faulting_thread, uint64_t vector,
                              uint64_t error_code, uint64_t fault_addr);

    void (*on_task_exit)(task_t *task, int64_t exit_status);
} mk_hooks_t;

void mk_register_hooks(const mk_hooks_t *hooks);
const mk_hooks_t *mk_hooks_get(void);

#endif /* LUCYK_MK_HOOKS_H */
