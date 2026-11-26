; Embeds the flat AP trampoline binary into the kernel image so the BSP can
; copy its bytes to physical 0x8000. The Makefile assembles
; build/ap_trampoline.bin first with "-f bin".
bits 64

section .rodata
align 16
global ap_trampoline_image
global ap_trampoline_image_end
ap_trampoline_image:
    incbin "build/ap_trampoline.bin"
ap_trampoline_image_end:

section .note.GNU-stack noalloc noexec nowrite progbits
