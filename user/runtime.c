#include <stdint.h>
#include "user_lib.h"

__attribute__((noreturn))
void user_exit(uint64_t code)
{
    (void)user_syscall(SYS_THREAD_EXIT, code, 0, 0, 0, 0);

    for (;;) {
        __asm__ volatile("hlt");
    }
}
