bits 64

section .text
global gdt_load
global gdt_ltr

; void gdt_load(const gdtr_t *gdtr)
; Loads the new GDT, reloads every data segment and performs a far return to
; reload CS with 0x08. The TSS is loaded separately per CPU.
gdt_load:
    lgdt [rdi]

    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    push qword 0x08
    lea rax, [rel .reload_cs]
    push rax
    retfq

.reload_cs:
    ret

; void gdt_ltr(uint16_t selector)
; Loads the calling CPU's Task Register. The hardware sets the descriptor Busy
; bit, so every CPU must be given its own descriptor.
gdt_ltr:
    mov ax, di
    ltr ax
    ret

section .note.GNU-stack noalloc noexec nowrite progbits
