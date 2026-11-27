#include <io.h>
#include <io/fbcon.h>
#include <klog.h>
#include <spinlock.h>
#include <types.h>

#define COM1_BASE 0x3F8
#define UART_CLOCK_DIVISOR 115200U

#define UART_REG_DATA   0
#define UART_REG_IER    1
#define UART_REG_FCR    2
#define UART_REG_LCR    3
#define UART_REG_MCR    4
#define UART_REG_LSR    5

#define UART_LSR_DR     0x01
#define UART_LSR_THRE   0x20
#define UART_LSR_TEMT   0x40

/* Serialises entire formatted lines across CPUs. Interrupt masking alone only
 * excludes the local core, so without this two APs would interleave bytes. */
static spinlock_t serial_lock;

uint64_t serial_lock_acquire(void)
{
    return spin_lock_irqsave(&serial_lock);
}

void serial_lock_release(uint64_t flags)
{
    spin_unlock_irqrestore(&serial_lock, flags);
}

void serial_init(uint32_t baud)
{
    spinlock_init(&serial_lock);

    if (baud == 0) {
        baud = UART_CLOCK_DIVISOR;
    }

    uint32_t divisor = UART_CLOCK_DIVISOR / baud;
    if (divisor == 0) {
        divisor = 1;
    }

    outb(COM1_BASE + UART_REG_IER, 0x00);
    outb(COM1_BASE + UART_REG_LCR, 0x80);
    outb(COM1_BASE + UART_REG_DATA, (uint8_t)(divisor & 0xFF));
    outb(COM1_BASE + UART_REG_IER, (uint8_t)((divisor >> 8) & 0xFF));
    outb(COM1_BASE + UART_REG_LCR, 0x03);
    /* Clearing DLAB exposes IER; keep all UART interrupts disabled. */
    outb(COM1_BASE + UART_REG_IER, 0x00);
    outb(COM1_BASE + UART_REG_FCR, 0xC7);
    outb(COM1_BASE + UART_REG_MCR, 0x0B);

    while ((inb(COM1_BASE + UART_REG_LSR) & UART_LSR_TEMT) == 0) {
        __asm__ volatile("pause");
    }
}

void serial_putc(char c)
{
    while ((inb(COM1_BASE + UART_REG_LSR) & UART_LSR_THRE) == 0) {
        __asm__ volatile("pause");
    }
    outb(COM1_BASE + UART_REG_DATA, (uint8_t)c);

    /* Mirror to the framebuffer text console (no-op once released / none). */
    fbcon_putc(c);
}

void serial_write(const char *buf, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        serial_putc(buf[i]);
    }
}

/* Non-blocking receive: returns the next byte or -1 when the FIFO is empty. */
int serial_getc(void)
{
    if ((inb(COM1_BASE + UART_REG_LSR) & UART_LSR_DR) == 0) {
        return -1;
    }

    return (int)inb(COM1_BASE + UART_REG_DATA);
}
