#include <klog.h>
#include <io.h>
#include <string.h>
#include <types.h>

/* ------------------------------------------------------------------------- */
/* Freestanding memory / string primitives                                    */
/* ------------------------------------------------------------------------- */

void *memcpy(void *dst, const void *src, size_t len)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;

    for (size_t i = 0; i < len; i++) {
        d[i] = s[i];
    }
    return dst;
}

void *memmove(void *dst, const void *src, size_t len)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;

    if (d < s) {
        for (size_t i = 0; i < len; i++) {
            d[i] = s[i];
        }
    } else if (d > s) {
        for (size_t i = len; i > 0; i--) {
            d[i - 1] = s[i - 1];
        }
    }
    return dst;
}

void *memset(void *dst, int value, size_t len)
{
    uint8_t *d = (uint8_t *)dst;

    for (size_t i = 0; i < len; i++) {
        d[i] = (uint8_t)value;
    }
    return dst;
}

int memcmp(const void *a, const void *b, size_t len)
{
    const uint8_t *pa = (const uint8_t *)a;
    const uint8_t *pb = (const uint8_t *)b;

    for (size_t i = 0; i < len; i++) {
        if (pa[i] != pb[i]) {
            return (int)pa[i] - (int)pb[i];
        }
    }
    return 0;
}

size_t strlen(const char *str)
{
    size_t len = 0;

    while (str[len] != '\0') {
        len++;
    }
    return len;
}

int strcmp(const char *a, const char *b)
{
    while (*a != '\0' && *a == *b) {
        a++;
        b++;
    }
    return (int)(uint8_t)*a - (int)(uint8_t)*b;
}

int strncmp(const char *a, const char *b, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        if (a[i] != b[i] || a[i] == '\0') {
            return (int)(uint8_t)a[i] - (int)(uint8_t)b[i];
        }
    }
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Formatted output                                                           */
/* ------------------------------------------------------------------------- */

typedef enum format_length {
    LEN_INT = 0,
    LEN_LONG,
    LEN_LLONG,
    LEN_SIZE
} format_length_t;

static void out_str(const char *str)
{
    while (*str != '\0') {
        serial_putc(*str++);
    }
}

static void out_unsigned(uint64_t value, unsigned base, bool upper,
                         int width, char pad)
{
    char tmp[66];
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    int count = 0;

    do {
        tmp[count++] = digits[value % base];
        value /= base;
    } while (value != 0);

    for (int i = count; i < width; i++) {
        serial_putc(pad);
    }
    while (count > 0) {
        serial_putc(tmp[--count]);
    }
}

static void out_signed(int64_t value, int width, char pad)
{
    char tmp[32];
    bool negative = value < 0;
    uint64_t magnitude = negative ? (uint64_t)(-(value + 1)) + 1U : (uint64_t)value;
    int count = 0;

    do {
        tmp[count++] = (char)('0' + (int)(magnitude % 10U));
        magnitude /= 10U;
    } while (magnitude != 0);

    int total = count + (negative ? 1 : 0);
    int padding = width > total ? width - total : 0;

    if (pad == '0') {
        if (negative) {
            serial_putc('-');
        }
        for (int i = 0; i < padding; i++) {
            serial_putc('0');
        }
    } else {
        for (int i = 0; i < padding; i++) {
            serial_putc(' ');
        }
        if (negative) {
            serial_putc('-');
        }
    }

    while (count > 0) {
        serial_putc(tmp[--count]);
    }
}

static void kvprintf(const char *fmt, va_list ap)
{
    const char *p = fmt;

    while (*p != '\0') {
        if (*p != '%') {
            serial_putc(*p++);
            continue;
        }

        p++;
        if (*p == '%') {
            serial_putc('%');
            p++;
            continue;
        }

        char pad = ' ';
        if (*p == '0') {
            pad = '0';
            p++;
        } else if (*p == '-') {
            p++;
        }

        int width = 0;
        while (*p >= '0' && *p <= '9') {
            width = (width * 10) + (*p - '0');
            p++;
        }

        format_length_t length = LEN_INT;
        if (*p == 'l') {
            p++;
            if (*p == 'l') {
                p++;
                length = LEN_LLONG;
            } else {
                length = LEN_LONG;
            }
        } else if (*p == 'z') {
            p++;
            length = LEN_SIZE;
        } else if (*p == 'h') {
            p++;
            if (*p == 'h') {
                p++;
            }
            length = LEN_INT;
        }

        char conv = *p;
        if (conv == '\0') {
            break;
        }
        p++;

        switch (conv) {
        case 'd':
        case 'i': {
            int64_t value;
            switch (length) {
            case LEN_LONG:  value = va_arg(ap, long); break;
            case LEN_LLONG: value = va_arg(ap, long long); break;
            case LEN_SIZE:  value = (int64_t)va_arg(ap, size_t); break;
            case LEN_INT:
            default:        value = va_arg(ap, int); break;
            }
            out_signed(value, width, pad);
            break;
        }
        case 'u':
        case 'x':
        case 'X':
        case 'o': {
            uint64_t value;
            switch (length) {
            case LEN_LONG:  value = va_arg(ap, unsigned long); break;
            case LEN_LLONG: value = va_arg(ap, unsigned long long); break;
            case LEN_SIZE:  value = va_arg(ap, size_t); break;
            case LEN_INT:
            default:        value = va_arg(ap, unsigned int); break;
            }
            unsigned base = (conv == 'o') ? 8U : ((conv == 'u') ? 10U : 16U);
            out_unsigned(value, base, conv == 'X', width, pad);
            break;
        }
        case 'p': {
            uint64_t value = (uint64_t)(uintptr_t)va_arg(ap, void *);
            out_str("0x");
            out_unsigned(value, 16U, false, 16, '0');
            break;
        }
        case 'c': {
            int value = va_arg(ap, int);
            serial_putc((char)value);
            break;
        }
        case 's': {
            const char *str = va_arg(ap, const char *);
            out_str(str != NULL ? str : "(null)");
            break;
        }
        default:
            serial_putc('%');
            serial_putc(conv);
            break;
        }
    }
}

void kprintf(const char *fmt, ...)
{
    /* Fault-path output must never block: the interrupted context may already
     * own the serial lock. Interleaving is acceptable for diagnostics. */
    va_list ap;

    va_start(ap, fmt);
    kvprintf(fmt, ap);
    va_end(ap);
}

static const char *level_tag(klog_level_t level)
{
    switch (level) {
    case KLOG_DEBUG: return "[DEBUG] ";
    case KLOG_INFO:  return "[INFO] ";
    case KLOG_WARN:  return "[WARN] ";
    case KLOG_ERROR: return "[ERROR] ";
    default:         return "[LOG] ";
    }
}

void klog(klog_level_t level, const char *fmt, ...)
{
    uint64_t flags = serial_lock_acquire();
    va_list ap;

    out_str(level_tag(level));
    va_start(ap, fmt);
    kvprintf(fmt, ap);
    va_end(ap);
    out_str("\r\n");

    serial_lock_release(flags);
}

NORETURN void kpanic(const char *fmt, ...)
{
    va_list ap;

    __asm__ volatile("cli" : : : "memory");

    out_str("\r\n[PANIC] ");
    va_start(ap, fmt);
    kvprintf(fmt, ap);
    va_end(ap);
    out_str("\r\n[PANIC] system halted\r\n");

    cpu_halt();
    __builtin_unreachable();
}
