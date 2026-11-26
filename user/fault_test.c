#include <stdint.h>

void user_main(void);
__attribute__((noreturn)) void user_exit(uint64_t code);

void user_main(void)
{
    volatile uint64_t *fault_address = (volatile uint64_t *)(uintptr_t)0;

    *fault_address = 0xDEADBEEFULL;

    for (;;) {
        __asm__ volatile("hlt");
    }
}
