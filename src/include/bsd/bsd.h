#ifndef LUCYK_BSD_BSD_H
#define LUCYK_BSD_BSD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <bsd/errno.h>
#include <types.h>

/* Size limits shared by the process and VFS layers. */
#define BSD_MAX_FDS     64
#define BSD_MAX_PROCS   256
#define BSD_COMM_LEN    32
#define BSD_PATH_MAX    256
#define BSD_ARG_MAX     4096
#define BSD_IO_CHUNK    4096
#define BSD_NAME_MAX    255
#define BSD_NSIG        32
#define BSD_MAX_EXEC_ARGS 16

/* Negative BSD errno as a syscall/VFS return value. */
#define BSD_ERR(e) (-(int64_t)(e))

/* Open flags (BSD_SYSCALLS.md section 4.1). */
#define BSD_O_RDONLY    0x0000U
#define BSD_O_WRONLY    0x0001U
#define BSD_O_RDWR      0x0002U
#define BSD_O_ACCMODE   0x0003U
#define BSD_O_APPEND    0x0008U
#define BSD_O_CREAT     0x0200U
#define BSD_O_TRUNC     0x0400U
#define BSD_O_DIRECTORY 0x00200000U

/* lseek() origins. */
#define BSD_SEEK_SET 0
#define BSD_SEEK_CUR 1
#define BSD_SEEK_END 2

/* wait4() options. */
#define BSD_WNOHANG 1

/* Anonymous memory windows. The mmap region lives far above the ELF image and
 * data break, and below the fixed user stack at 0x7FFFFFF000. */
#define BSD_MMAP_BASE     0x0000001000000000ULL
#define BSD_MMAP_LIMIT    0x0000007000000000ULL
#define BSD_BRK_LIMIT     0x0000000040000000ULL
#define BSD_MAX_MMAPS     16
#define BSD_PIPE_CAPACITY 4096

/* mmap protection and flags (BSD_SYSCALLS.md section 4.4). */
#define BSD_PROT_NONE  0x0U
#define BSD_PROT_READ  0x1U
#define BSD_PROT_WRITE 0x2U
#define BSD_PROT_EXEC  0x4U

#define BSD_MAP_SHARED  0x01U
#define BSD_MAP_PRIVATE 0x02U
#define BSD_MAP_FIXED   0x10U
#define BSD_MAP_ANON    0x20U
#define BSD_MAP_FAILED  ((void *)-1)

/* Directory entry type values for BSD_SYS_GETDENTS. */
#define BSD_DT_CHR 2U
#define BSD_DT_DIR 4U
#define BSD_DT_REG 8U

/* TIOCGWINSZ request and window-size layout. */
#define BSD_TIOCGWINSZ 0x5413U

typedef struct bsd_dirent {
    uint64_t d_ino;
    uint16_t d_reclen;
    uint8_t  d_type;
    uint8_t  d_namlen;
    char     d_name[256];
} bsd_dirent_t;

_Static_assert(sizeof(bsd_dirent_t) == 272,
               "bsd_dirent_t must match the BSD_SYSCALLS.md ABI layout");

typedef struct bsd_winsize {
    uint16_t ws_row;
    uint16_t ws_col;
    uint16_t ws_xpixel;
    uint16_t ws_ypixel;
} bsd_winsize_t;

_Static_assert(sizeof(bsd_winsize_t) == 8,
               "bsd_winsize_t must match the TIOCGWINSZ ABI");

/* st_mode file type bits and permission helper. */
#define BSD_S_IFMT  0170000U
#define BSD_S_IFDIR 0040000U
#define BSD_S_IFCHR 0020000U
#define BSD_S_IFREG 0100000U

/* Result of stat()/fstat() in the BSD ABI layout. */
typedef struct bsd_stat {
    uint32_t st_mode;
    uint32_t st_nlink;
    uint32_t st_uid;
    uint32_t st_gid;
    uint64_t st_ino;
    uint64_t st_size;
    uint64_t st_blksize;
    uint64_t st_blocks;
} bsd_stat_t;

_Static_assert(sizeof(bsd_stat_t) == 48,
               "bsd_stat_t must match the BSD_SYSCALLS.md ABI layout");

/* Boot module descriptor handed to bsd_init() by kernel_main(). */
typedef struct bsd_module {
    const char *name;
    const void *address;
    size_t size;
} bsd_module_t;

/* Page-aligned end of the highest PT_LOAD segment in an ELF64 image. Returns 0
 * when the image headers are invalid or out of range. */
uint64_t bsd_elf_image_break(const void *image, size_t size);

/* Mounts the initrd, initializes the console, registers the personality hooks
 * and starts PID 1 (/bin/init). */
void bsd_init(const bsd_module_t *modules, size_t module_count);

#endif /* LUCYK_BSD_BSD_H */
