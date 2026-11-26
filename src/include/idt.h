#ifndef LUCYK_IDT_H
#define LUCYK_IDT_H

#include <isr.h>

typedef void (*interrupt_handler_t)(interrupt_frame_t *frame);

/* Installs 256 interrupt gates: CPU exceptions 0..31 use dedicated stubs,
 * vectors 32..255 use generic stubs so spurious IRQs cannot triple fault. */
void idt_init(void);

/* Registers a handler for an external vector (32..255). Exceptions 0..31 are
 * always reported by the core and cannot be overridden. */
void idt_register_handler(uint8_t vector, interrupt_handler_t handler);

/* Re-loads the IDT on the calling CPU. The table is shared, but the IDTR is a
 * per-CPU register, so each AP must execute LIDT once. */
void idt_load(void);

/* Fills the 10-byte IDTR operand for AP trampoline handoff. */
void idt_descriptor(uint64_t *limit_out, uint64_t *base_out);

#endif /* LUCYK_IDT_H */
