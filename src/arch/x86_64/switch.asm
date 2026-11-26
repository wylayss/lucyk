bits 64

section .text

global switch_context
global thread_entry_start
global boot_stack_enter

extern thread_entry_c

; void switch_context(cpu_context_t **old_ctx, cpu_context_t *new_ctx,
;                     uint64_t new_rsp0, uint64_t new_cr3,
;                     uint8_t *old_fx, const uint8_t *new_fx)
;
; Saves the outgoing FPU/SSE image (FXSAVE64 into old_fx, r8), the
; callee-saved registers and the resume RIP, installs the new stack, updates
; this CPU's TSS.rsp0 and GS-resident kernel_rsp, reloads CR3 when new_cr3 is
; non-zero, and restores the incoming FPU/SSE image (FXRSTOR64 from new_fx,
; r9). Both images are 16-byte aligned 512-byte areas; NULL skips the step.
; The kernel is compiled without x87/SSE code generation, so the registers
; saved here always hold the outgoing thread's own (user) state.
; IA32_FS_BASE save/restore is handled by the caller (sched.c switch_to) via
; RDMSR/WRMSR around this call.
switch_context:
    test r8, r8
    jz .no_fx_save
    fxsave64 [r8]
.no_fx_save:
    push rbp
    push rbx
    push r12
    push r13
    push r14
    push r15

    mov [rdi], rsp
    mov rsp, rsi

    ; TSS.rsp0 = new_rsp0 for the calling CPU, selected by cpu_index.
    mov rax, [gs:48]          ; cpu_local_t *self
    mov r10d, [rax + 16]      ; cpu_index
    lea r11, [rel gdt_tss_bases]
    mov r11, [r11 + r10 * 8]  ; tss64_t * for this CPU
    mov [r11 + 4], rdx        ; tss->rsp0

    ; Keep the SYSCALL kernel stack pointer in lockstep with TSS.rsp0.
    mov [rax + 0], rdx

    test rcx, rcx
    jz .no_cr3
    mov cr3, rcx
.no_cr3:

    test r9, r9
    jz .no_fx_restore
    fxrstor64 [r9]
.no_fx_restore:

    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret

; Fresh threads start here with r12 = thread_t * and a 16-byte aligned stack.
thread_entry_start:
    mov rdi, r12
    call thread_entry_c

.hang:
    cli
    hlt
    jmp .hang

; void boot_stack_enter(uint64_t stack_top, void (*entry)(void))
;
; One-way transition from the static bootstrap stack to the idle thread's
; guarded slot. The bootstrap frames are deliberately abandoned.
boot_stack_enter:
    cli
    mov rsp, rdi
    xor rbp, rbp
    call rsi

.hang:
    cli
    hlt
    jmp .hang

section .data
align 8
; Published by gdt.c: kernel VA of each CPU's tss64_t (16 entries).
extern gdt_tss_bases

section .note.GNU-stack noalloc noexec nowrite progbits
