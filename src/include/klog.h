#ifndef LUCYK_KLOG_H
#define LUCYK_KLOG_H

#include <stdarg.h>
#include <types.h>

typedef enum klog_level {
    KLOG_DEBUG = 0,
    KLOG_INFO,
    KLOG_WARN,
    KLOG_ERROR
} klog_level_t;

/* COM1 (0x3F8) UART driver — src/arch/x86_64/serial.c */
void serial_init(uint32_t baud);
void serial_putc(char c);
void serial_write(const char *buf, size_t len);
int serial_getc(void);

/* Serialises one complete formatted line across CPUs. Interrupt masking only
 * excludes the local core, so every multi-character output path holds this. */
uint64_t serial_lock_acquire(void);
void serial_lock_release(uint64_t flags);

/* Formatted kernel logging — src/mk/klog.c */
void kprintf(const char *fmt, ...);
void klog(klog_level_t level, const char *fmt, ...);
NORETURN void kpanic(const char *fmt, ...);

#endif /* LUCYK_KLOG_H */
