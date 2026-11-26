bits 64

section .bss
align 16
stack_bottom:
    resb 65536
global kernel_stack_top
kernel_stack_top:

section .text
global _start
extern kernel_main

; Limine transfers control to the ELF entry point. Install a known-good kernel
; stack before entering C so no bootloader stack assumptions leak into the core.
_start:
    cli
    cld
    mov rsp, kernel_stack_top
    xor rbp, rbp
    call kernel_main

.hang:
    cli
    hlt
    jmp .hang

section .note.GNU-stack noalloc noexec nowrite progbits
