#ifndef LUCYK_IO_H
#define LUCYK_IO_H

#include <stdint.h>

/* Saves RFLAGS and disables interrupts atomically. */
static inline uint64_t irq_save(void)
{
    uint64_t flags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(flags) : : "memory");
    return flags;
}

/* Restores the interrupt-enable state captured by irq_save(). */
static inline void irq_restore(uint64_t flags)
{
    if ((flags & (1ULL << 9)) != 0) {
        __asm__ volatile("sti" : : : "memory");
    } else {
        __asm__ volatile("cli" : : : "memory");
    }
}

static inline void outb(uint16_t port, uint8_t value)
{
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port) : "memory");
}

static inline uint8_t inb(uint16_t port)
{
    uint8_t value;
    __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port) : "memory");
    return value;
}

static inline void outw(uint16_t port, uint16_t value)
{
    __asm__ volatile("outw %0, %1" : : "a"(value), "Nd"(port) : "memory");
}

static inline uint16_t inw(uint16_t port)
{
    uint16_t value;
    __asm__ volatile("inw %1, %0" : "=a"(value) : "Nd"(port) : "memory");
    return value;
}

static inline void outl(uint16_t port, uint32_t value)
{
    __asm__ volatile("outl %0, %1" : : "a"(value), "Nd"(port) : "memory");
}

static inline uint32_t inl(uint16_t port)
{
    uint32_t value;
    __asm__ volatile("inl %1, %0" : "=a"(value) : "Nd"(port) : "memory");
    return value;
}

/* Short delay by writing to the unused POST diagnostic port. */
static inline void io_wait(void)
{
    outb(0x80, 0);
}

static inline void cpu_halt(void)
{
    for (;;) {
        __asm__ volatile("cli; hlt" : : : "memory");
    }
}

#endif /* LUCYK_IO_H */
