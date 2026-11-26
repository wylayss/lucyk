bits 64

section .text

extern isr_dispatch
extern cpu_has_smap
global isr_common

; Stub for vectors without a hardware-pushed error code: synthesize a zero
; error code so every frame has identical layout.
%macro ISR_NOERR 1
global isr%1
isr%1:
    push qword 0
    push qword %1
    jmp isr_common
%endmacro

; Stub for vectors where the CPU already pushed an error code.
%macro ISR_ERR 1
global isr%1
isr%1:
    push qword %1
    jmp isr_common
%endmacro

; Push order is chosen so the resulting stack matches interrupt_frame_t:
; lowest address (r15) .. highest (rax), then vector, error, CPU frame.
isr_common:
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    ; Interrupts can arrive from Ring 0 as well as Ring 3. GS_BASE is per-CPU
    ; cpu_local in Ring 0 and the user value in Ring 3, so a Ring 0 interrupt
    ; must not swap. However, a kernel context that switched in from user mode
    ; already holds the kernel GS base, so the saved CS decides.
    mov rax, [rsp + 144]
    test al, 3
    jz .gs_entry_done
    swapgs
.gs_entry_done:

    cld
    ; A user context can enter the ISR with RFLAGS.AC=1. Clear it before any
    ; C code runs so SMAP never silently permits supervisor accesses to user
    ; pages. The instruction is gated because CPUs without SMAP may #UD on
    ; CLAC; on such CPUs AC has no effect and nothing needs clearing.
    cmp byte [rel cpu_has_smap], 0
    je .ac_cleared
    clac
.ac_cleared:

    mov rdi, rsp
    ; Same-ring interrupts inherit the interrupted RSP, which is not guaranteed
    ; to be 16-byte aligned. rbx is callee-saved (and already saved on the
    ; frame), so use it to keep the original frame pointer across the call.
    mov rbx, rsp
    and rsp, -16
    call isr_dispatch
    mov rsp, rbx

    ; Restore the user GS base when returning to Ring 3.
    mov rax, [rsp + 144]
    test al, 3
    jz .gs_exit_done
    swapgs
.gs_exit_done:

    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax
    add rsp, 16

    ; IRETQ to Ring 3 requires SS.RPL == CPL. Some resume paths can leave the
    ; saved selector with the RPL bits cleared (0x18 instead of 0x1B); force
    ; the only valid Ring 3 data selector, and never touch a Ring 0 frame.
    ; RAX was just restored from the frame, so preserve it across the test.
    push rax
    mov al, byte [rsp + 16]
    and al, 3
    cmp al, 3
    jne .ss_restore
    mov qword [rsp + 40], 0x1B
.ss_restore:
    pop rax
    iretq

%assign vec 0
%rep 256
%if vec == 8 || vec == 10 || vec == 11 || vec == 12 || vec == 13 || vec == 14 || vec == 17 || vec == 21 || vec == 29 || vec == 30
    ISR_ERR vec
%else
    ISR_NOERR vec
%endif
%assign vec vec+1
%endrep

section .rodata
align 8
global isr_stub_table
isr_stub_table:
%assign vec 0
%rep 256
    dq isr %+ vec
%assign vec vec+1
%endrep

section .note.GNU-stack noalloc noexec nowrite progbits
