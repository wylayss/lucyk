bits 64

section .text
global syscall_entry
extern syscall_dispatch

; SYSCALL entry. The hardware has saved user RIP in rcx and user RFLAGS in r11
; and has applied IA32_FMASK. The kernel GS base is swapped in immediately so
; cpu_local (offset 0 = kernel_rsp, offset 8 = user_rsp_scratch) is reachable.
syscall_entry:
    swapgs
    mov [gs:8], rsp
    mov rsp, [gs:0]

    push rbp
    push rbx
    push r12
    push r13
    push r14
    push r15

    ; syscall_frame_t, highest field first so rsp points at frame.rdi.
    push qword [gs:8]       ; frame.rsp
    push r11                ; frame.r11 (user RFLAGS)
    push rcx                ; frame.rcx (user RIP)
    push rax                ; frame.rax (syscall number / return)
    push r9                 ; frame.r9
    push r8                 ; frame.r8
    push r10                ; frame.r10
    push rdx                ; frame.rdx
    push rsi                ; frame.rsi
    push rdi                ; frame.rdi

    mov rdi, rsp
    sti
    call syscall_dispatch
    cli

    ; Read the syscall result and user return state back out of the frame.
    mov rax, [rsp + 48]
    mov rdx, [rsp + 16]
    mov r10, [rsp + 24]
    mov rdi, [rsp + 0]
    mov rsi, [rsp + 8]
    mov r8,  [rsp + 32]
    mov r9,  [rsp + 40]
    mov rcx, [rsp + 56]
    mov r11, [rsp + 64]

    ; Sanitize user RFLAGS: clear TF/DF/IOPL/NT/RF/VM/AC, force IF + bit 1.
    ; rbx is still backed by the saved user value on the stack, so it is safe
    ; to use as scratch here.
    mov rbx, 0x0000000000077500
    not rbx
    and r11, rbx
    or r11, 0x202

    mov rbx, 0x00007FFFFFFFFFFF
    cmp rcx, rbx
    ja .iretq_path

    add rsp, 80
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp

    ; frame.rsp lives at kstack_top - 56 after the frame and callee-saved
    ; slots have been popped.
    mov rsp, [rsp - 56]
    swapgs
    o64 sysret

.iretq_path:
    ; Non-canonical return RIP: force a canonical user address so the user
    ; fault path (not a kernel fault) terminates the task.
    xor ecx, ecx

    add rsp, 80
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp

    ; Build the IRETQ frame on the kernel stack. The return RAX is reloaded
    ; from the syscall frame after the five pushes.
    mov rax, [rsp - 56]
    push qword 0x1B
    push rax
    push r11
    push qword 0x23
    push rcx
    mov rax, [rsp - 40]
    swapgs
    iretq

section .note.GNU-stack noalloc noexec nowrite progbits

; void user_frame_resume(const syscall_frame_t *frame)
;
; Restores a complete Ring 3 register set from a syscall_frame_t and enters
; user mode through IRETQ. Offsets match the C structure:
;   0 rdi,   8 rsi,  16 rdx,  24 r10,  32 r8,   40 r9,
;  48 rax,  56 rcx (RIP), 64 r11 (RFLAGS), 72 rsp (user),
;  80 r15,  88 r14,  96 r13, 104 r12, 112 rbx, 120 rbp
;
; Interrupts are disabled while the IRETQ frame is built so no ISR can observe
; the half-swapped GS base. The IRETQ restores user IF.
section .text
global user_frame_resume
user_frame_resume:
    cli

    mov rax, [rdi + 72]
    push qword 0x1B
    push rax

    mov rax, [rdi + 64]
    mov rcx, 0xFFFFFFFFFFF88AFF   ; ~(TF|IF|DF|IOPL|NT|RF|VM|AC)
    and rax, rcx
    or rax, 0x202
    push rax

    push qword 0x23
    mov rax, [rdi + 56]
    push rax

    mov r15, [rdi + 80]
    mov r14, [rdi + 88]
    mov r13, [rdi + 96]
    mov r12, [rdi + 104]
    mov rbx, [rdi + 112]
    mov rbp, [rdi + 120]
    mov rsi, [rdi + 8]
    mov rdx, [rdi + 16]
    mov r10, [rdi + 24]
    mov r8,  [rdi + 32]
    mov r9,  [rdi + 40]
    mov rax, [rdi + 48]
    mov rcx, [rdi + 56]
    mov r11, [rdi + 64]
    mov rdi, [rdi + 0]

    swapgs
    iretq
