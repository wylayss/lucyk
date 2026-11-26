#ifndef LUCYK_DISPLAY_H
#define LUCYK_DISPLAY_H

#include <stdint.h>

/* Boot display description shared with user space. The kernel copies the
 * Limine linear framebuffer geometry into one read-only CAP_OBJ_FRAME page and
 * hands it, together with a device CAP_OBJ_FRAME covering the framebuffer
 * itself, to the first user task (see docs/SYSCALL_ABI.md §4.2). The kernel
 * contains no graphics code: user space maps both frames and renders. */
#define DISPLAY_INFO_MAGIC   0x4C55435944495350ULL /* "LUCYDISP" */
#define DISPLAY_INFO_VERSION 1U

/* Handles the boot capabilities occupy in the first user task's table. */
#define DISPLAY_BOOT_CAP_FRAMEBUFFER 1U
#define DISPLAY_BOOT_CAP_INFO        2U

typedef struct display_info {
    uint64_t magic;            /* DISPLAY_INFO_MAGIC */
    uint32_t version;          /* DISPLAY_INFO_VERSION */
    uint32_t framebuffer_count;/* Displays reported by the bootloader */
    uint64_t phys_base;        /* Physical address of the framebuffer */
    uint64_t size_bytes;       /* pitch * height */
    uint64_t mapped_bytes;     /* Page-rounded span of the frame capability */
    uint64_t width;            /* Pixels */
    uint64_t height;           /* Pixels */
    uint64_t pitch;            /* Bytes per scanline */
    uint16_t bpp;              /* Bits per pixel */
    uint8_t  memory_model;     /* 1 = RGB */
    uint8_t  red_mask_size;
    uint8_t  red_mask_shift;
    uint8_t  green_mask_size;
    uint8_t  green_mask_shift;
    uint8_t  blue_mask_size;
    uint8_t  blue_mask_shift;
    uint8_t  write_combining;  /* 1 when mapped write-combining */
    uint8_t  reserved[6];
} display_info_t;

_Static_assert(sizeof(display_info_t) == 80, "display_info_t ABI size");

#endif /* LUCYK_DISPLAY_H */
