#ifndef LUCYK_BSD_MMAP_H
#define LUCYK_BSD_MMAP_H

#include <bsd/bsd.h>
#include <types.h>

struct bsd_proc;

/* Anonymous private mappings and the process data break. */
sys_status_t bsd_mmap(struct bsd_proc *proc, uint64_t addr, uint64_t len,
                      uint32_t prot, uint32_t flags, int32_t fd,
                      int64_t offset, vaddr_t *out);
sys_status_t bsd_munmap(struct bsd_proc *proc, uint64_t addr, uint64_t len);
sys_status_t bsd_brk(struct bsd_proc *proc, uint64_t new_brk,
                     vaddr_t *result);

/* Changes protection of page-aligned tracked mmap regions. Enforces W^X
 * (rejects PROT_WRITE | PROT_EXEC), preserves CoW semantics, and shoots down
 * stale TLB entries on remote CPUs. */
sys_status_t bsd_mprotect(struct bsd_proc *proc, uint64_t addr, uint64_t len,
                          uint32_t prot);

#endif /* LUCYK_BSD_MMAP_H */
