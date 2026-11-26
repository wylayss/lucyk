#ifndef LUCYK_BSD_EXT2_H
#define LUCYK_BSD_EXT2_H

#include <bsd/bsd.h>
#include <io/blkdev.h>
#include <types.h>

/* Reads the ext2 superblock and block group descriptor table from dev, then
 * installs the root inode as the VFS root. Returns SYS_OK or a negative BSD
 * errno. */
sys_status_t ext2_mount(blkdev_t *dev);

/* True once a volume has been mounted successfully. */
bool ext2_mounted(void);

#endif /* LUCYK_BSD_EXT2_H */