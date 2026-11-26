#include "bsd_user.h"

__attribute__((noreturn)) void user_exit(uint64_t code)
{
    (void)bsd_syscall(BSD_SYS_EXIT, code, 0, 0);

    for (;;) {
        __asm__ volatile("hlt");
    }
}

size_t text_len(const char *text)
{
    size_t length = 0;

    while (text[length] != '\0') {
        length++;
    }
    return length;
}

void write_all(int32_t fd, const char *text, size_t length)
{
    size_t written = 0;

    while (written < length) {
        int64_t result = bsd_write(fd, text + written, length - written);

        if (result <= 0) {
            return;
        }
        written += (size_t)result;
    }
}

void write_text(int32_t fd, const char *text)
{
    write_all(fd, text, text_len(text));
}

void write_unsigned(int32_t fd, uint64_t value)
{
    char digits[24];
    size_t count = 0;

    do {
        digits[count++] = (char)('0' + (int)(value % 10U));
        value /= 10U;
    } while (value != 0);

    while (count > 0) {
        write_all(fd, &digits[--count], 1);
    }
}

void write_signed(int32_t fd, int64_t value)
{
    if (value < 0) {
        write_all(fd, "-", 1);
        write_unsigned(fd, (uint64_t)(-(value + 1)) + 1U);
        return;
    }
    write_unsigned(fd, (uint64_t)value);
}

bool str_eq(const char *a, const char *b)
{
    size_t i = 0;

    while (a[i] != '\0' && b[i] != '\0') {
        if (a[i] != b[i]) {
            return false;
        }
        i++;
    }

    return a[i] == b[i];
}

bool str_eq_len(const char *a, const char *b, size_t length)
{
    for (size_t i = 0; i < length; i++) {
        if (a[i] != b[i]) {
            return false;
        }
    }
    return true;
}

bool parse_int(const char *text, int64_t *value)
{
    if (text == NULL || value == NULL || text[0] == '\0') {
        return false;
    }

    bool negative = false;
    size_t i = 0;

    if (text[0] == '-') {
        negative = true;
        i = 1;
        if (text[1] == '\0') {
            return false;
        }
    }

    int64_t result = 0;

    for (; text[i] != '\0'; i++) {
        if (text[i] < '0' || text[i] > '9') {
            return false;
        }

        int digit = text[i] - '0';

        if (result > (INT64_MAX - digit) / 10) {
            return false;
        }
        result = result * 10 + digit;
    }

    *value = negative ? -result : result;
    return true;
}
