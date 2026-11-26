#include <idt.h>
#include <io.h>
#include <ioapic.h>
#include <isr.h>
#include <klog.h>
#include <lapic.h>
#include <mk_hooks.h>
#include <sched.h>
#include <smp.h>
#include <string.h>
#include <syscall.h>
#include <types.h>
#include <vspace.h>

#define IDT_ENTRY_COUNT 256
#define KERNEL_CODE_SELECTOR 0x08
#define IDT_GATE_INTERRUPT 0x8E

typedef struct PACKED idt_entry {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t  ist;
    uint8_t  type_attr;
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t reserved;
} idt_entry_t;

typedef struct PACKED idtr {
    uint16_t limit;
    uint64_t base;
} idtr_t;

extern uint64_t isr_stub_table[IDT_ENTRY_COUNT];

static idt_entry_t idt[IDT_ENTRY_COUNT] __attribute__((aligned(16)));
static idtr_t idtr;
static interrupt_handler_t irq_handlers[IDT_ENTRY_COUNT];

static const char *const exception_names[32] = {
    "Divide Error",
    "Debug",
    "Non-Maskable Interrupt",
    "Breakpoint",
    "Overflow",
    "Bound Range Exceeded",
    "Invalid Opcode",
    "Device Not Available",
    "Double Fault",
    "Coprocessor Segment Overrun",
    "Invalid TSS",
    "Segment Not Present",
    "Stack-Segment Fault",
    "General Protection Fault",
    "Page Fault",
    "Reserved",
    "x87 Floating-Point Exception",
    "Alignment Check",
    "Machine Check",
    "SIMD Floating-Point Exception",
    "Virtualization Exception",
    "Control Protection Exception",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Hypervisor Injection Exception",
    "VMM Communication Exception",
    "Security Exception",
    "Reserved"
};

static void idt_set_gate(size_t vector, uint64_t handler)
{
    idt[vector].offset_low  = (uint16_t)(handler & 0xFFFFULL);
    idt[vector].selector    = KERNEL_CODE_SELECTOR;
    idt[vector].ist         = 0;
    idt[vector].type_attr   = IDT_GATE_INTERRUPT;
    idt[vector].offset_mid  = (uint16_t)((handler >> 16) & 0xFFFFULL);
    idt[vector].offset_high = (uint32_t)((handler >> 32) & 0xFFFFFFFFULL);
    idt[vector].reserved    = 0;
}

void idt_init(void)
{
    memset(idt, 0, sizeof(idt));

    for (size_t vector = 0; vector < IDT_ENTRY_COUNT; vector++) {
        idt_set_gate(vector, isr_stub_table[vector]);
    }

    /* IDT IST indices are 1-based: 1 = TSS.ist[0] (IST1), 2 = TSS.ist[1]. */
    idt[2].ist = 2; /* #NMI -> IST2 */
    idt[8].ist = 1; /* #DF  -> IST1 */

    idtr.limit = (uint16_t)(sizeof(idt) - 1);
    idtr.base = (uint64_t)(uintptr_t)&idt;

    __asm__ volatile("lidt %0" : : "m"(idtr) : "memory");
}

void idt_load(void)
{
    __asm__ volatile("lidt %0" : : "m"(idtr) : "memory");
}

void idt_descriptor(uint64_t *limit_out, uint64_t *base_out)
{
    if (limit_out != NULL) {
        *limit_out = (uint64_t)idtr.limit;
    }
    if (base_out != NULL) {
        *base_out = idtr.base;
    }
}

void idt_register_handler(uint8_t vector, interrupt_handler_t handler)
{
    if (vector < 32) {
        return;
    }

    irq_handlers[vector] = handler;
}

void isr_dispatch(interrupt_frame_t *frame)
{
    const char *name = "External/Spurious Interrupt";

    if (frame->vector < 32 && (frame->cs & 3) == 3) {
        uint64_t fault_addr = 0;

        if (frame->vector == 14) {
            __asm__ volatile("mov %%cr2, %0" : "=r"(fault_addr));

            /* error bit0 = present, bit1 = write, bit2 = user. A write to a
             * present user page may be a copy-on-write fault; resolve it
             * before the personality layer turns it into a signal. */
            if ((frame->error & 0x7ULL) == 0x7ULL) {
                thread_t *thread = sched_current_thread();
                vspace_t *vspace =
                    (thread != NULL && thread->owner_task != NULL)
                        ? thread->owner_task->vspace
                        : NULL;

                if (vspace_handle_cow_fault(vspace, fault_addr)) {
                    return;
                }
            }
        }

        const mk_hooks_t *hooks = mk_hooks_get();
        bool handled = false;

        if (hooks != NULL && hooks->on_user_exception != NULL) {
            handled = hooks->on_user_exception(sched_current_thread(),
                                               frame->vector, frame->error,
                                               fault_addr);
        }

        if (handled) {
            return;
        }

        kprintf("\r\n[USER FAULT] vector=%lu error=0x%016lx rip=0x%016lx cr2=0x%016lx\r\n",
                frame->vector, frame->error, frame->rip, fault_addr);
        kprintf("             task terminating\r\n");
        task_terminate();
    }

    if (frame->vector >= 32) {
        interrupt_handler_t handler = irq_handlers[frame->vector];

        if (handler != NULL) {
            handler(frame);

            /* External IRQ vectors are acknowledged here so every handler
             * shares one EOI contract. The LAPIC timer, the IPI vectors and
             * the spurious vector keep their own acknowledgement paths. */
            if (frame->vector <= IOAPIC_VECTOR_MAX &&
                frame->vector != LAPIC_TIMER_VECTOR &&
                frame->vector != IPI_RESCHEDULE_VECTOR &&
                frame->vector != IPI_TLB_SHOOTDOWN_VECTOR) {
                lapic_send_eoi();
            }
            return;
        }
    } else {
        name = exception_names[frame->vector];
    }

    kprintf("\r\n[FAULT] vector=%lu (%s)\r\n",
            frame->vector, name);
    kprintf("        error=0x%016lx rip=0x%016lx cs=0x%lx rflags=0x%016lx\r\n",
            frame->error, frame->rip, frame->cs, frame->rflags);
    kprintf("        rsp=0x%016lx ss=0x%lx\r\n", frame->rsp, frame->ss);
    kprintf("        rax=0x%016lx rbx=0x%016lx rcx=0x%016lx rdx=0x%016lx\r\n",
            frame->rax, frame->rbx, frame->rcx, frame->rdx);
    kprintf("        rsi=0x%016lx rdi=0x%016lx rbp=0x%016lx\r\n",
            frame->rsi, frame->rdi, frame->rbp);
    kprintf("        r8 =0x%016lx r9 =0x%016lx r10=0x%016lx r11=0x%016lx\r\n",
            frame->r8, frame->r9, frame->r10, frame->r11);
    kprintf("        r12=0x%016lx r13=0x%016lx r14=0x%016lx r15=0x%016lx\r\n",
            frame->r12, frame->r13, frame->r14, frame->r15);

    kpanic("unhandled %s (vector %lu)", name, frame->vector);
}
