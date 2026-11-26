#ifndef LUCYK_KALLOC_H
#define LUCYK_KALLOC_H

#include <types.h>

/* Dedicated higher-half window backing the kernel heap. Pages are mapped
 * individually with RW+NX; physical frames need not be contiguous. */
#define KHEAP_BASE  0xFFFFA00000000000ULL
#define KHEAP_LIMIT 0xFFFFB00000000000ULL

void kalloc_init(void);

void *kalloc(size_t size);
void kfree(void *ptr);

#endif /* LUCYK_KALLOC_H */
