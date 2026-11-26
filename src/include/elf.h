#ifndef LUCYK_ELF_H
#define LUCYK_ELF_H

#include <types.h>
#include <vspace.h>

#define ELF_MAGIC0   0x7FU
#define ELF_MAGIC1   'E'
#define ELF_MAGIC2   'L'
#define ELF_MAGIC3   'F'
#define ELFCLASS64   2U
#define ELFDATA2LSB  1U
#define ET_EXEC      2U
#define EM_X86_64    62U
#define PT_LOAD      1U
#define PF_X         1U
#define PF_W         2U
#define PF_R         4U

#define ELF_USER_STACK_TOP 0x0000007FFFFFF000ULL
#define ELF_USER_STACK_SIZE (4ULL * 4096ULL)

typedef struct PACKED elf64_ehdr {
    uint8_t  e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} elf64_ehdr_t;

typedef struct PACKED elf64_phdr {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} elf64_phdr_t;

sys_status_t elf64_load(vspace_t *vspace, const void *image, size_t size,
                        uint64_t *entry_out);

#endif /* LUCYK_ELF_H */
