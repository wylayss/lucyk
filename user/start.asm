bits 64

section .text
global _start
extern user_main
extern user_exit

_start:
    xor rbp, rbp
    and rsp, -16
    call user_main
    xor edi, edi
    call user_exit

.hang:
    hlt
    jmp .hang

section .note.GNU-stack noalloc noexec nowrite progbits
