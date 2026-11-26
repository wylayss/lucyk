#include <elf.h>
#include <pmm.h>
#include <string.h>
#include <types.h>
#include <vspace.h>

#define ELF_USER_MAX 0x00007FFFFFFFFFFFULL

static bool header_valid(const elf64_ehdr_t *header, size_t size)
{
    if (size < sizeof(elf64_ehdr_t)) {
        return false;
    }
    if (header->e_ident[0] != ELF_MAGIC0 ||
        header->e_ident[1] != ELF_MAGIC1 ||
        header->e_ident[2] != ELF_MAGIC2 ||
        header->e_ident[3] != ELF_MAGIC3) {
        return false;
    }
    if (header->e_ident[4] != ELFCLASS64 || header->e_ident[5] != ELFDATA2LSB) {
        return false;
    }
    if (header->e_type != ET_EXEC || header->e_machine != EM_X86_64) {
        return false;
    }
    if (header->e_phentsize != sizeof(elf64_phdr_t) || header->e_phnum == 0) {
        return false;
    }
    if (header->e_phoff > size ||
        (uint64_t)header->e_phnum >
            (size - header->e_phoff) / sizeof(elf64_phdr_t)) {
        return false;
    }
    return true;
}

static uint32_t segment_protection(uint32_t p_flags)
{
    uint32_t prot = VSPACE_PROT_USER | VSPACE_PROT_OWNED;

    if ((p_flags & PF_W) != 0) {
        prot |= VSPACE_PROT_WRITE;
    } else if ((p_flags & PF_X) != 0) {
        prot |= VSPACE_PROT_EXEC;
    }

    return prot;
}

sys_status_t elf64_load(vspace_t *vspace, const void *image, size_t size,
                        uint64_t *entry_out)
{
    if (vspace == NULL || image == NULL || entry_out == NULL) {
        return SYS_ERR_INVAL;
    }

    const elf64_ehdr_t *header = (const elf64_ehdr_t *)image;

    if (!header_valid(header, size)) {
        return SYS_ERR_INVAL;
    }

    uint64_t entry = header->e_entry;
    if (entry < 0x1000 || entry > ELF_USER_MAX) {
        return SYS_ERR_INVAL;
    }

    bool entry_executable = false;

    for (uint16_t i = 0; i < header->e_phnum; i++) {
        const elf64_phdr_t *segment =
            (const elf64_phdr_t *)((const uint8_t *)image +
                                   header->e_phoff +
                                   ((uint64_t)i * sizeof(elf64_phdr_t)));

        if (segment->p_type != PT_LOAD) {
            continue;
        }
        if ((segment->p_flags & PF_W) != 0 && (segment->p_flags & PF_X) != 0) {
            return SYS_ERR_INVAL;
        }
        if (segment->p_memsz == 0 || segment->p_filesz > segment->p_memsz) {
            return SYS_ERR_INVAL;
        }
        if (segment->p_offset > size ||
            segment->p_filesz > size - segment->p_offset) {
            return SYS_ERR_INVAL;
        }
        if (segment->p_vaddr < 0x1000 || segment->p_vaddr > ELF_USER_MAX) {
            return SYS_ERR_INVAL;
        }
        if (segment->p_memsz > ELF_USER_MAX - segment->p_vaddr + 1ULL) {
            return SYS_ERR_INVAL;
        }

        uint64_t segment_start = segment->p_vaddr;
        uint64_t segment_mem_end = segment_start + segment->p_memsz;
        uint64_t segment_file_end = segment_start + segment->p_filesz;

        if (entry >= segment_start && entry < segment_mem_end &&
            (segment->p_flags & PF_X) != 0) {
            entry_executable = true;
        }

        uint64_t page = PAGE_ALIGN_DOWN(segment_start);
        uint64_t end_page = PAGE_ALIGN_UP(segment_mem_end);
        uint32_t prot = segment_protection(segment->p_flags);

        for (; page < end_page; page += PAGE_SIZE) {
            paddr_t phys = pmm_alloc_frame();
            if (phys == 0) {
                return SYS_ERR_NOMEM;
            }

            uint8_t *page_mem = (uint8_t *)pmm_phys_to_virt(phys);

            uint64_t copy_start = page > segment_start ? page : segment_start;
            uint64_t page_end = page + PAGE_SIZE;
            uint64_t copy_end =
                page_end < segment_file_end ? page_end : segment_file_end;

            if (copy_end > copy_start) {
                size_t dst_offset = (size_t)(copy_start - page);
                size_t src_offset =
                    (size_t)(segment->p_offset + (copy_start - segment_start));
                size_t copy_len = (size_t)(copy_end - copy_start);

                memcpy(page_mem + dst_offset,
                       (const uint8_t *)image + src_offset, copy_len);
            }

            if (vspace_map_user(vspace, page, phys, prot) != SYS_OK) {
                pmm_free_frame(phys);
                return SYS_ERR_NOMEM;
            }
        }
    }

    if (!entry_executable) {
        return SYS_ERR_INVAL;
    }

    uint64_t stack_base = ELF_USER_STACK_TOP - ELF_USER_STACK_SIZE;

    for (uint64_t offset = 0; offset < ELF_USER_STACK_SIZE; offset += PAGE_SIZE) {
        paddr_t phys = pmm_alloc_frame();
        if (phys == 0) {
            return SYS_ERR_NOMEM;
        }

        if (vspace_map_user(vspace, stack_base + offset, phys,
                            VSPACE_PROT_USER | VSPACE_PROT_WRITE |
                                VSPACE_PROT_OWNED) != SYS_OK) {
            pmm_free_frame(phys);
            return SYS_ERR_NOMEM;
        }
    }

    *entry_out = entry;
    return SYS_OK;
}
