; 16 -> 32 -> 64-bit Application Processor bootstrap.
;
; Assembled as a flat binary at ORG 0x8000 and copied to the reserved physical
; page 0x8000, which is where an AP starts after INIT-SIPI-SIPI
; (CS = 0x0800, IP = 0, paging off).
;
; The AP runs on the kernel's own page tables from the moment paging is
; enabled: the BSP installs a temporary 4 KiB identity mapping for this page in
; kernel_pml4 (PML4[0]) before the handshake, so exactly one page-table
; hierarchy exists at all times. Execution continues at 0x8xxx until the final
; indirect jump into the higher half.
;
; The parameter block is the last 256 bytes of the image (offset 0xF00) and is
; filled by smp.c. All fields are 8-byte slots so they can be loaded with
; aligned absolute/rel accesses.
;
; The blob is position-dependent: ORG equals AP_TRAMPOLINE_PHYS, so the GDT
; descriptor below already holds the correct physical base and no runtime
; patching is required.

bits 16
org 0x8000

    cli
    cld

    ; MP startup only guarantees CS:IP; every absolute reference below assumes
    ; a zero segment base.
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax

    ; Fast A20 gate through port 0x92.
    in al, 0x92
    or al, 0x02
    and al, 0xFE
    out 0x92, al

    lgdt [gdt_desc]

    mov eax, cr0
    or eax, 0x00000001
    mov cr0, eax

    jmp 0x08:pm32_entry

; Bootstrap GDT: 0x08 32-bit code, 0x10 32-bit data, 0x18 64-bit code.
; The layout is pinned by ORG: flat_gdt sits at 0x8060 and its 6-byte
; descriptor at 0x8080. A 64-bit code segment is required to reach long mode;
; the kernel's own GDT is loaded immediately afterwards.
times 0x60 - ($ - $$) db 0
flat_gdt:
    dq 0x0000000000000000
    dq 0x00CF9A000000FFFF
    dq 0x00CF92000000FFFF
    dq 0x00AF9A000000FFFF
flat_gdt_end:

gdt_desc:
    dw (flat_gdt_end - flat_gdt) - 1
    dd flat_gdt

bits 32

pm32_entry:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    ; The identity page is read-only under W^X and nothing below pushes or
    ; calls: the real stack is installed in long mode from the parameter
    ; block. ESP is pointed at the page only so a stray push faults loudly.
    mov esp, 0x8E00

    ; NX must be enabled before paging whenever a reachable PTE has bit 63
    ; set: every writable kernel page (including this AP's stack) is mapped
    ; NX, and with EFER.NXE clear that bit is reserved and faults on access.
    ; Probe CPUID and fall back to a plain long-mode entry when NX is absent.
    xor esi, esi
    mov eax, 0x80000000
    cpuid
    cmp eax, 0x80000001
    jb .efer_ready
    mov eax, 0x80000001
    cpuid
    test edx, 1 << 20
    jz .efer_ready
    mov esi, 1
.efer_ready:
    ; PAE must be enabled before IA32_EFER.LME and before a 4-level CR3 is
    ; loaded.
    mov eax, cr4
    or eax, 1 << 5
    mov cr4, eax

    ; Boot straight onto the kernel page tables; there is no private
    ; bootstrap hierarchy to switch away from later.
    mov eax, [ap_p_cr3]
    mov cr3, eax

    mov ecx, 0xC0000080
    rdmsr
    or eax, 1 << 8
    test esi, esi
    jz .write_efer
    or eax, 1 << 11
.write_efer:
    wrmsr

    mov eax, cr0
    or eax, 1 << 31
    mov cr0, eax

    ; PML4[0] identity-maps this page, so the fetch of the next instruction
    ; still resolves after paging comes on.
    jmp 0x18:lm64_entry

bits 64

lm64_entry:
    ; Still executing at the physical address: this page is mapped only there
    ; (the identity leaf), so every access below is a low-page rel load. The
    ; far return changes CS but not RIP, so this code keeps running low until
    ; the final indirect jump into the kernel.
    mov rsp, [rel ap_p_kstack]
    sub rsp, 16

    ; Build the kernel GDTR and IDTR operands on the guarded stack, then
    ; install both. The bootstrap descriptors cached in the segment registers
    ; keep this code running even though GDTR already points at the kernel.
    mov rax, [rel ap_p_gdt_limit]
    mov [rsp], ax
    mov rax, [rel ap_p_gdt_base]
    mov [rsp + 2], rax
    lgdt [rsp]

    mov rax, [rel ap_p_idt_limit]
    mov [rsp], ax
    mov rax, [rel ap_p_idt_base]
    mov [rsp + 2], rax
    lidt [rsp]

    mov rdi, [rel ap_p_cpu]

    ; Far-jump into the kernel's 64-bit code segment (0x08) so CS matches the
    ; GDT that was just installed.
    push qword 0x08
    lea rax, [rel .kernel_cs]
    push rax
    retfq

.kernel_cs:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    ; Move to the top of the higher-half guarded stack and shape the frame like
    ; a call so ap_entry_c sees the System V invariant RSP % 16 == 8. The low
    ; trampoline page is never used as a C stack.
    mov rsp, [rel ap_p_kstack]
    sub rsp, 8
    mov qword [rsp], 0

    mov rax, [rel ap_p_entry]
    jmp rax

.hang:
    cli
    hlt
    jmp .hang

; Parameter block, written by smp.c before INIT-SIPI-SIPI. Offsets are mirrored
; by ap_params_t.
times 0xF00 - ($ - $$) db 0

ap_p_cr3:       dq 0                      ; 0x8F00 kernel_pml4 physical address
ap_p_gdt_limit: dq 0                      ; 0x8F08
ap_p_gdt_base:  dq 0                      ; 0x8F10
ap_p_idt_limit: dq 0                      ; 0x8F18
ap_p_idt_base:  dq 0                      ; 0x8F20
ap_p_kstack:    dq 0                      ; 0x8F28 kstack_top, higher-half VA
ap_p_cpu:       dq 0                      ; 0x8F30 cpu_index
ap_p_entry:     dq 0                      ; 0x8F38 ap_entry_c, higher-half VA

times 0x1000 - ($ - $$) db 0

global ap_trampoline_image_end
ap_trampoline_image_end:
