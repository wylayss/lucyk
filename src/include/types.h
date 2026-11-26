#ifndef LUCYK_TYPES_H
#define LUCYK_TYPES_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* Address widths on x86_64 (physical frames and canonical virtual addresses). */
typedef uint64_t vaddr_t;
typedef uint64_t paddr_t;

/* Kernel image is linked in the topmost 2 GiB of the canonical address space. */
#define KERNEL_BASE  0xFFFFFFFF80000000ULL

/* Standard page geometry for the x86_64 4-level paging hierarchy. */
#define PAGE_SIZE    0x1000ULL
#define PAGE_MASK    (PAGE_SIZE - 1ULL)

#define PAGE_ALIGN_DOWN(x) ((uint64_t)(x) & ~PAGE_MASK)
#define PAGE_ALIGN_UP(x)   (((uint64_t)(x) + PAGE_MASK) & ~PAGE_MASK)
#define ALIGN_UP(x, a)     (((uint64_t)(x) + ((uint64_t)(a) - 1ULL)) & ~((uint64_t)(a) - 1ULL))
#define ALIGN_DOWN(x, a)   ((uint64_t)(x) & ~((uint64_t)(a) - 1ULL))

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

#define PACKED        __attribute__((packed))
#define NORETURN      __attribute__((noreturn))
#define UNUSED(x)     ((void)(x))

/* Native microkernel return status (see SYSCALL_ABI.md). */
typedef int64_t sys_status_t;

#define SYS_OK              ((sys_status_t)0)
#define SYS_ERR_INVAL       ((sys_status_t)-1)
#define SYS_ERR_NOMEM       ((sys_status_t)-2)
#define SYS_ERR_CAP_INVALID ((sys_status_t)-3)
#define SYS_ERR_CAP_RIGHTS  ((sys_status_t)-4)
#define SYS_ERR_BUSY        ((sys_status_t)-5)
#define SYS_ERR_DEAD        ((sys_status_t)-6)
#define SYS_ERR_NOSYS       ((sys_status_t)-7)

#endif /* LUCYK_TYPES_H */
