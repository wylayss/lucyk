#include <bsd/bsd.h>
#include <bsd/errno.h>
#include <bsd/ext2.h>
#include <bsd/vfs.h>
#include <io/blkdev.h>
#include <kalloc.h>
#include <klog.h>
#include <spinlock.h>
#include <string.h>
#include <types.h>

#define EXT2_SUPER_MAGIC      0xEF53U
#define EXT2_SUPER_OFFSET     1024U
#define EXT2_SUPER_BYTES      1024U
#define EXT2_ROOT_INO         2U
#define EXT2_INODE_BYTES      128U
#define EXT2_DIRECT_BLOCKS    12U
#define EXT2_SINGLE_BLOCK     12U
#define EXT2_DOUBLE_BLOCK     13U
#define EXT2_TRIPLE_BLOCK     14U
#define EXT2_DIRENT_MIN       8U
#define EXT2_DIR_PREV_NONE    0xFFFFFFFFU
#define EXT2_MAX_BLOCK_SIZE   32768U
#define EXT2_MAX_GROUPS       4096U

#define EXT2_FEATURE_INCOMPAT_FILETYPE 0x0002U
#define EXT2_FEATURE_RO_COMPAT_CSUM    0x0400U
#define EXT2_FEATURE_COMPAT_RESIZE_INODE 0x0010U
#define EXT2_SB_RESERVED_GDT_OFFSET    206U

#define EXT2_FT_REG_FILE 1U
#define EXT2_FT_DIR      2U

#define EXT2_S_IFMT  0170000U
#define EXT2_S_IFREG 0100000U
#define EXT2_S_IFDIR 0040000U

typedef struct PACKED ext2_superblock {
    uint32_t s_inodes_count;
    uint32_t s_blocks_count;
    uint32_t s_r_blocks_count;
    uint32_t s_free_blocks_count;
    uint32_t s_free_inodes_count;
    uint32_t s_first_data_block;
    uint32_t s_log_block_size;
    uint32_t s_log_frag_size;
    uint32_t s_blocks_per_group;
    uint32_t s_frags_per_group;
    uint32_t s_inodes_per_group;
    uint32_t s_mtime;
    uint32_t s_wtime;
    uint16_t s_mnt_count;
    uint16_t s_max_mnt_count;
    uint16_t s_magic;
    uint16_t s_state;
    uint16_t s_errors;
    uint16_t s_minor_rev_level;
    uint32_t s_lastcheck;
    uint32_t s_checkinterval;
    uint32_t s_creator_os;
    uint32_t s_rev_level;
    uint16_t s_def_resuid;
    uint16_t s_def_resgid;
    uint32_t s_first_ino;
    uint16_t s_inode_size;
    uint16_t s_block_group_nr;
    uint32_t s_feature_compat;
    uint32_t s_feature_incompat;
    uint32_t s_feature_ro_compat;
} ext2_superblock_t;

_Static_assert(sizeof(ext2_superblock_t) == 104,
               "ext2 superblock prefix layout");

typedef struct PACKED ext2_group_desc {
    uint32_t bg_block_bitmap;
    uint32_t bg_inode_bitmap;
    uint32_t bg_inode_table;
    uint16_t bg_free_blocks_count;
    uint16_t bg_free_inodes_count;
    uint16_t bg_used_dirs_count;
    uint16_t bg_pad;
    uint8_t  bg_reserved[12];
} ext2_group_desc_t;

_Static_assert(sizeof(ext2_group_desc_t) == 32, "ext2 group descriptor layout");

typedef struct PACKED ext2_inode_disk {
    uint16_t i_mode;
    uint16_t i_uid;
    uint32_t i_size_lo;
    uint32_t i_atime;
    uint32_t i_ctime;
    uint32_t i_mtime;
    uint32_t i_dtime;
    uint16_t i_gid;
    uint16_t i_links_count;
    uint32_t i_blocks_lo;
    uint32_t i_flags;
    uint32_t i_osd1;
    uint32_t i_block[15];
    uint32_t i_generation;
    uint32_t i_file_acl;
    uint32_t i_size_high;
    uint32_t i_faddr;
    uint8_t  i_osd2[12];
} ext2_inode_disk_t;

_Static_assert(sizeof(ext2_inode_disk_t) == 128, "ext2 inode layout");

typedef struct PACKED ext2_dirent {
    uint32_t inode;
    uint16_t rec_len;
    uint8_t  name_len;
    uint8_t  file_type;
    char     name[];
} ext2_dirent_t;

typedef struct ext2_inode {
    uint16_t mode;
    uint16_t uid;
    uint16_t gid;
    uint16_t links_count;
    uint32_t blocks;
    uint64_t size;
    uint32_t block[15];
} ext2_inode_t;

typedef struct ext2_fs {
    blkdev_t      *dev;
    bool           mounted;
    ext2_superblock_t sb;
    uint8_t        sb_raw[EXT2_SUPER_BYTES];
    ext2_group_desc_t *groups;
    uint32_t       group_count;
    uint32_t       block_size;
    uint32_t       sectors_per_block;
    uint32_t       inode_size;
    uint32_t       first_data_block;
    uint32_t       first_ino;
    uint32_t       blocks_per_group;
    uint32_t       inodes_per_group;
    uint32_t       inode_table_blocks;
    uint32_t       inodes_count;
    uint32_t       blocks_count;
    uint32_t       group_table_block;
    uint32_t       group_table_blocks;
    uint16_t       reserved_gdt_blocks;
} ext2_fs_t;

typedef struct ext2_open_entry {
    struct ext2_open_entry *next;
    ext2_fs_t *fs;
    uint32_t   ino;
    uint32_t   refs;
    bool       unlinked;
} ext2_open_entry_t;

typedef struct ext2_node {
    ext2_fs_t *fs;
    uint32_t   ino;
    ext2_open_entry_t *open;
    char       name[BSD_NAME_MAX + 1];
} ext2_node_t;

typedef struct ext2_dir_scan {
    uint32_t block;
    uint32_t offset;
    uint32_t prev_offset;
    uint32_t ino;
    uint8_t  type;
    char     name[BSD_NAME_MAX + 1];
} ext2_dir_scan_t;

static ext2_fs_t ext2_fs;
static spinlock_t ext2_lock;
static ext2_open_entry_t *ext2_open_head;

static sys_status_t ext2_read(vnode_t *vnode, void *buf, size_t len,
                              uint64_t offset, size_t *done);
static sys_status_t ext2_write(vnode_t *vnode, const void *buf, size_t len,
                               uint64_t offset, size_t *done);
static sys_status_t ext2_lookup(vnode_t *dir, const char *name,
                                vnode_t **out);
static sys_status_t ext2_readdir(vnode_t *dir, uint64_t index,
                                 bsd_dirent_t *entry);
static sys_status_t ext2_create(vnode_t *dir, const char *name, uint32_t mode,
                                vnode_t **out);
static sys_status_t ext2_mkdir(vnode_t *dir, const char *name, uint32_t mode,
                               vnode_t **out);
static sys_status_t ext2_unlink(vnode_t *dir, const char *name);
static sys_status_t ext2_rmdir(vnode_t *dir, const char *name);
static sys_status_t ext2_truncate(vnode_t *vnode);
static sys_status_t ext2_get_size(vnode_t *vnode, uint64_t *size);
static void ext2_release(vnode_t *vnode);

static vnode_ops_t ext2_vnode_ops = {
    .read = ext2_read,
    .write = ext2_write,
    .lookup = ext2_lookup,
    .readdir = ext2_readdir,
    .close = NULL,
    .release = ext2_release,
    .create = ext2_create,
    .mkdir = ext2_mkdir,
    .unlink = ext2_unlink,
    .rmdir = ext2_rmdir,
    .truncate = ext2_truncate,
    .get_size = ext2_get_size,
};

static sys_status_t ext2_io(sys_status_t status)
{
    return status == SYS_OK ? SYS_OK : BSD_ERR(BSD_EIO);
}

static sys_status_t ext2_lba_transfer(ext2_fs_t *fs, uint64_t lba,
                                      uint32_t sectors, void *buffer,
                                      bool write)
{
    uint8_t *cursor = (uint8_t *)buffer;

    while (sectors > 0) {
        uint32_t chunk = sectors > BLK_MAX_REQUEST_SECTORS
                             ? BLK_MAX_REQUEST_SECTORS
                             : sectors;
        sys_status_t status;

        if (write) {
            status = fs->dev->write(fs->dev, lba, chunk, cursor);
        } else {
            status = fs->dev->read(fs->dev, lba, chunk, cursor);
        }

        if (status != SYS_OK) {
            return status;
        }

        lba += chunk;
        cursor += (size_t)chunk * BLK_SECTOR_SIZE;
        sectors -= chunk;
    }

    return SYS_OK;
}

static sys_status_t ext2_read_block(ext2_fs_t *fs, uint32_t block,
                                    void *buffer)
{
    if (block >= fs->blocks_count) {
        return BSD_ERR(BSD_EIO);
    }

    uint64_t lba = (uint64_t)block * fs->sectors_per_block;
    return ext2_io(ext2_lba_transfer(fs, lba, fs->sectors_per_block, buffer,
                                     false));
}

static sys_status_t ext2_write_block(ext2_fs_t *fs, uint32_t block,
                                     const void *buffer)
{
    if (block >= fs->blocks_count) {
        return BSD_ERR(BSD_EIO);
    }

    uint64_t lba = (uint64_t)block * fs->sectors_per_block;
    return ext2_io(ext2_lba_transfer(fs, lba, fs->sectors_per_block,
                                     (void *)buffer, true));
}

static sys_status_t ext2_flush_super(ext2_fs_t *fs)
{
    memcpy(fs->sb_raw, &fs->sb, sizeof(fs->sb));
    return ext2_io(ext2_lba_transfer(fs, EXT2_SUPER_OFFSET / BLK_SECTOR_SIZE,
                                     EXT2_SUPER_BYTES / BLK_SECTOR_SIZE,
                                     fs->sb_raw, true));
}

static sys_status_t ext2_flush_groups(ext2_fs_t *fs)
{
    return ext2_io(ext2_lba_transfer(
        fs, (uint64_t)fs->group_table_block * fs->sectors_per_block,
        fs->group_table_blocks * fs->sectors_per_block, fs->groups, true));
}

static sys_status_t ext2_inode_location(ext2_fs_t *fs, uint32_t ino,
                                        uint32_t *block_out,
                                        uint32_t *offset_out)
{
    if (ino == 0 || ino > fs->inodes_count) {
        return BSD_ERR(BSD_EINVAL);
    }

    uint32_t group = (ino - 1U) / fs->inodes_per_group;
    uint32_t index = (ino - 1U) % fs->inodes_per_group;

    if (group >= fs->group_count) {
        return BSD_ERR(BSD_EIO);
    }

    uint64_t byte = (uint64_t)index * fs->inode_size;
    uint64_t table_block = (uint64_t)fs->groups[group].bg_inode_table +
                           (byte / fs->block_size);

    if (table_block >= fs->blocks_count ||
        table_block >= (uint64_t)fs->groups[group].bg_inode_table +
                           fs->inode_table_blocks) {
        return BSD_ERR(BSD_EIO);
    }

    *block_out = (uint32_t)table_block;
    *offset_out = (uint32_t)(byte % fs->block_size);
    return SYS_OK;
}

static sys_status_t ext2_read_inode(ext2_fs_t *fs, uint32_t ino,
                                    ext2_inode_t *out)
{
    uint32_t block;
    uint32_t offset;
    sys_status_t status = ext2_inode_location(fs, ino, &block, &offset);

    if (status != SYS_OK) {
        return status;
    }

    uint8_t *buffer = (uint8_t *)kalloc(fs->block_size);
    if (buffer == NULL) {
        return BSD_ERR(BSD_ENOMEM);
    }

    status = ext2_read_block(fs, block, buffer);

    if (status == SYS_OK) {
        if (offset + EXT2_INODE_BYTES > fs->block_size) {
            status = BSD_ERR(BSD_EIO);
        } else {
            const ext2_inode_disk_t *disk =
                (const ext2_inode_disk_t *)(const void *)(buffer + offset);

            memset(out, 0, sizeof(*out));
            out->mode = disk->i_mode;
            out->uid = disk->i_uid;
            out->gid = disk->i_gid;
            out->links_count = disk->i_links_count;
            out->blocks = disk->i_blocks_lo;
            out->size = disk->i_size_lo;

            uint16_t type = (uint16_t)(disk->i_mode & EXT2_S_IFMT);

            /* i_size_high is only a size field for rev-1 regular files; on
             * rev-0 inodes and directories it is the directory ACL. */
            if (fs->sb.s_rev_level > 0 && type == EXT2_S_IFREG) {
                out->size |= (uint64_t)disk->i_size_high << 32;
            }

            if (out->size > (uint64_t)fs->blocks_count * fs->block_size ||
                (out->size / fs->block_size) > UINT32_MAX) {
                status = BSD_ERR(BSD_EIO);
            } else if (type == EXT2_S_IFDIR &&
                       (out->size % fs->block_size) != 0) {
                status = BSD_ERR(BSD_EIO);
            }

            memcpy(out->block, disk->i_block, sizeof(out->block));
        }
    }

    kfree(buffer);
    return status;
}

static sys_status_t ext2_write_inode(ext2_fs_t *fs, uint32_t ino,
                                     const ext2_inode_t *inode)
{
    uint32_t block;
    uint32_t offset;
    sys_status_t status = ext2_inode_location(fs, ino, &block, &offset);

    if (status != SYS_OK) {
        return status;
    }

    uint8_t *buffer = (uint8_t *)kalloc(fs->block_size);
    if (buffer == NULL) {
        return BSD_ERR(BSD_ENOMEM);
    }

    status = ext2_read_block(fs, block, buffer);

    if (status == SYS_OK) {
        if (offset + EXT2_INODE_BYTES > fs->block_size) {
            status = BSD_ERR(BSD_EIO);
        } else {
            ext2_inode_disk_t *disk =
                (ext2_inode_disk_t *)(void *)(buffer + offset);

            disk->i_mode = inode->mode;
            disk->i_uid = inode->uid;
            disk->i_gid = inode->gid;
            disk->i_links_count = inode->links_count;
            disk->i_size_lo = (uint32_t)inode->size;
            if (fs->sb.s_rev_level > 0 &&
                (inode->mode & EXT2_S_IFMT) == EXT2_S_IFREG) {
                disk->i_size_high = (uint32_t)(inode->size >> 32);
            }
            disk->i_blocks_lo = inode->blocks;
            memcpy(disk->i_block, inode->block, sizeof(inode->block));

            status = ext2_write_block(fs, block, buffer);
        }
    }

    kfree(buffer);
    return status;
}

static void ext2_inode_add_blocks(ext2_inode_t *inode, uint32_t sectors)
{
    if (inode->blocks > UINT32_MAX - sectors) {
        inode->blocks = UINT32_MAX;
    } else {
        inode->blocks += sectors;
    }
}

static void ext2_inode_sub_blocks(ext2_inode_t *inode, uint32_t sectors)
{
    inode->blocks = inode->blocks > sectors ? inode->blocks - sectors : 0;
}

/* The open-entry table lets unlink() keep an inode alive until the last live
 * vnode is released, matching POSIX unlink-while-open semantics. Callers hold
 * ext2_lock except where noted. */
static ext2_open_entry_t *ext2_open_find(ext2_fs_t *fs, uint32_t ino)
{
    for (ext2_open_entry_t *entry = ext2_open_head; entry != NULL;
         entry = entry->next) {
        if (entry->fs == fs && entry->ino == ino) {
            return entry;
        }
    }
    return NULL;
}

static ext2_open_entry_t *ext2_open_get(ext2_fs_t *fs, uint32_t ino)
{
    ext2_open_entry_t *entry = ext2_open_find(fs, ino);

    if (entry != NULL) {
        return entry;
    }

    entry = (ext2_open_entry_t *)kalloc(sizeof(*entry));
    if (entry == NULL) {
        return NULL;
    }

    memset(entry, 0, sizeof(*entry));
    entry->fs = fs;
    entry->ino = ino;
    entry->next = ext2_open_head;
    ext2_open_head = entry;
    return entry;
}

static void ext2_open_remove(ext2_open_entry_t *entry)
{
    ext2_open_entry_t **link = &ext2_open_head;

    while (*link != NULL) {
        if (*link == entry) {
            *link = entry->next;
            kfree(entry);
            return;
        }
        link = &(*link)->next;
    }
}

/* Metadata must never be handed to a file or released by a corrupt inode,
 * even when the on-disk bitmap is inconsistent. */
static bool ext2_block_is_reserved(ext2_fs_t *fs, uint32_t block)
{
    if (block <= fs->first_data_block || block >= fs->blocks_count) {
        return true;
    }

    if ((uint64_t)block >= (uint64_t)fs->group_table_block &&
        (uint64_t)block < (uint64_t)fs->group_table_block +
                            fs->group_table_blocks) {
        return true;
    }

    if ((uint64_t)block >= (uint64_t)fs->group_table_block +
                                fs->group_table_blocks &&
        (uint64_t)block < (uint64_t)fs->group_table_block +
                            fs->group_table_blocks +
                            fs->reserved_gdt_blocks) {
        return true;
    }

    for (uint32_t group = 0; group < fs->group_count; group++) {
        uint64_t group_first = (uint64_t)fs->first_data_block +
                               ((uint64_t)group * fs->blocks_per_group);

        if ((uint64_t)block < group_first) {
            continue;
        }
        /* Every group reserves its backup superblock and GDT area; groups
         * without a backup lose a couple of data blocks to this conservative
         * check, which is preferable to freeing metadata. */
        if ((uint64_t)block < group_first + 1U + fs->group_table_blocks) {
            return true;
        }
        if (block == fs->groups[group].bg_block_bitmap ||
            block == fs->groups[group].bg_inode_bitmap) {
            return true;
        }
        if ((uint64_t)block >= fs->groups[group].bg_inode_table &&
            (uint64_t)block < (uint64_t)fs->groups[group].bg_inode_table +
                                fs->inode_table_blocks) {
            return true;
        }
    }

    return false;
}

static sys_status_t ext2_alloc_block(ext2_fs_t *fs, uint32_t *block_out)
{
    uint8_t *bitmap = (uint8_t *)kalloc(fs->block_size);

    if (bitmap == NULL) {
        return BSD_ERR(BSD_ENOMEM);
    }

    for (uint32_t group = 0; group < fs->group_count; group++) {
        if (fs->groups[group].bg_free_blocks_count == 0) {
            continue;
        }

        sys_status_t status =
            ext2_read_block(fs, fs->groups[group].bg_block_bitmap, bitmap);

        if (status != SYS_OK) {
            kfree(bitmap);
            return status;
        }

        uint64_t first64 = (uint64_t)fs->first_data_block +
                           ((uint64_t)group * fs->blocks_per_group);
        if (first64 >= fs->blocks_count) {
            continue;
        }

        uint32_t first = (uint32_t)first64;
        uint32_t limit = fs->blocks_per_group;

        if (limit > fs->blocks_count - first) {
            limit = fs->blocks_count - first;
        }

        for (uint32_t bit = 0; bit < limit; bit++) {
            uint8_t mask = (uint8_t)(1U << (bit & 7U));

            if ((bitmap[bit >> 3] & mask) != 0) {
                continue;
            }

            uint32_t block = first + bit;

            if (ext2_block_is_reserved(fs, block)) {
                continue;
            }

            bitmap[bit >> 3] |= mask;

            status = ext2_write_block(fs, fs->groups[group].bg_block_bitmap,
                                      bitmap);
            if (status != SYS_OK) {
                kfree(bitmap);
                return status;
            }

            if (fs->groups[group].bg_free_blocks_count > 0) {
                fs->groups[group].bg_free_blocks_count--;
            }
            if (fs->sb.s_free_blocks_count > 0) {
                fs->sb.s_free_blocks_count--;
            }

            status = ext2_flush_groups(fs);
            if (status == SYS_OK) {
                status = ext2_flush_super(fs);
            }
            if (status != SYS_OK) {
                kfree(bitmap);
                return status;
            }

            memset(bitmap, 0, fs->block_size);
            status = ext2_write_block(fs, block, bitmap);
            kfree(bitmap);

            if (status != SYS_OK) {
                return status;
            }

            *block_out = block;
            return SYS_OK;
        }
    }

    kfree(bitmap);
    return BSD_ERR(BSD_ENOSPC);
}

static sys_status_t ext2_free_block(ext2_fs_t *fs, uint32_t block)
{
    if (block >= fs->blocks_count || ext2_block_is_reserved(fs, block)) {
        return BSD_ERR(BSD_EINVAL);
    }

    uint32_t group =
        (block - fs->first_data_block) / fs->blocks_per_group;
    uint32_t bit = (block - fs->first_data_block) % fs->blocks_per_group;

    if (group >= fs->group_count) {
        return BSD_ERR(BSD_EIO);
    }

    uint8_t *bitmap = (uint8_t *)kalloc(fs->block_size);
    if (bitmap == NULL) {
        return BSD_ERR(BSD_ENOMEM);
    }

    sys_status_t status =
        ext2_read_block(fs, fs->groups[group].bg_block_bitmap, bitmap);

    if (status != SYS_OK) {
        kfree(bitmap);
        return status;
    }

    uint8_t mask = (uint8_t)(1U << (bit & 7U));

    if ((bitmap[bit >> 3] & mask) == 0) {
        kfree(bitmap);
        return SYS_OK;
    }

    bitmap[bit >> 3] &= (uint8_t)~mask;

    status = ext2_write_block(fs, fs->groups[group].bg_block_bitmap, bitmap);
    kfree(bitmap);

    if (status != SYS_OK) {
        return status;
    }

    if (fs->groups[group].bg_free_blocks_count < UINT16_MAX) {
        fs->groups[group].bg_free_blocks_count++;
    }
    if (fs->sb.s_free_blocks_count < UINT32_MAX) {
        fs->sb.s_free_blocks_count++;
    }

    status = ext2_flush_groups(fs);
    if (status == SYS_OK) {
        status = ext2_flush_super(fs);
    }

    return status;
}

static sys_status_t ext2_alloc_inode(ext2_fs_t *fs, uint16_t mode,
                                     uint32_t *ino_out)
{
    bool is_dir = (mode & EXT2_S_IFMT) == EXT2_S_IFDIR;
    uint8_t *bitmap = (uint8_t *)kalloc(fs->block_size);
    uint8_t *inode_buffer = (uint8_t *)kalloc(fs->block_size);

    if (bitmap == NULL || inode_buffer == NULL) {
        if (bitmap != NULL) {
            kfree(bitmap);
        }
        if (inode_buffer != NULL) {
            kfree(inode_buffer);
        }
        return BSD_ERR(BSD_ENOMEM);
    }

    for (uint32_t group = 0; group < fs->group_count; group++) {
        if (fs->groups[group].bg_free_inodes_count == 0) {
            continue;
        }

        uint64_t group_base = (uint64_t)group * fs->inodes_per_group;

        if (group_base >= fs->inodes_count) {
            continue;
        }

        sys_status_t status =
            ext2_read_block(fs, fs->groups[group].bg_inode_bitmap, bitmap);

        if (status != SYS_OK) {
            kfree(bitmap);
            kfree(inode_buffer);
            return status;
        }

        uint32_t start = 0;
        uint32_t limit = fs->inodes_per_group;

        if (group == 0 && fs->first_ino > 0) {
            start = fs->first_ino - 1U;
        }
        if ((uint64_t)limit > fs->inodes_count - group_base) {
            limit = (uint32_t)(fs->inodes_count - group_base);
        }
        if (start >= limit) {
            continue;
        }

        for (uint32_t bit = start; bit < limit; bit++) {
            uint8_t inode_mask = (uint8_t)(1U << (bit & 7U));

            if ((bitmap[bit >> 3] & inode_mask) != 0) {
                continue;
            }

            uint32_t ino = (uint32_t)(group_base + bit + 1U);
            uint32_t table_block;
            uint32_t offset;

            status = ext2_inode_location(fs, ino, &table_block, &offset);
            if (status != SYS_OK) {
                kfree(bitmap);
                kfree(inode_buffer);
                return status;
            }

            memset(inode_buffer, 0, fs->block_size);
            status = ext2_read_block(fs, table_block, inode_buffer);

            if (status == SYS_OK) {
                if (offset + fs->inode_size > fs->block_size) {
                    status = BSD_ERR(BSD_EIO);
                } else {
                    ext2_inode_disk_t *disk =
                        (ext2_inode_disk_t *)(void *)(inode_buffer + offset);

                    memset(disk, 0, fs->inode_size);
                    disk->i_mode = mode;
                    disk->i_links_count = (uint16_t)(is_dir ? 2U : 1U);
                    disk->i_size_lo = 0;
                    disk->i_blocks_lo = 0;

                    status = ext2_write_block(fs, table_block, inode_buffer);
                }
            }

            if (status != SYS_OK) {
                kfree(bitmap);
                kfree(inode_buffer);
                return status;
            }

            /* Publish the inode only after its table entry is valid. */
            bitmap[bit >> 3] |= inode_mask;

            status = ext2_write_block(fs, fs->groups[group].bg_inode_bitmap,
                                      bitmap);
            if (status != SYS_OK) {
                kfree(bitmap);
                kfree(inode_buffer);
                return status;
            }

            if (fs->groups[group].bg_free_inodes_count > 0) {
                fs->groups[group].bg_free_inodes_count--;
            }
            if (is_dir &&
                fs->groups[group].bg_used_dirs_count < UINT16_MAX) {
                fs->groups[group].bg_used_dirs_count++;
            }
            if (fs->sb.s_free_inodes_count > 0) {
                fs->sb.s_free_inodes_count--;
            }

            status = ext2_flush_groups(fs);
            if (status == SYS_OK) {
                status = ext2_flush_super(fs);
            }

            kfree(bitmap);
            kfree(inode_buffer);

            if (status != SYS_OK) {
                return status;
            }

            *ino_out = ino;
            return SYS_OK;
        }
    }

    kfree(bitmap);
    kfree(inode_buffer);
    return BSD_ERR(BSD_ENOSPC);
}

static sys_status_t ext2_free_inode(ext2_fs_t *fs, uint32_t ino, bool is_dir)
{
    if (ino == 0 || ino > fs->inodes_count) {
        return BSD_ERR(BSD_EINVAL);
    }

    uint32_t group = (ino - 1U) / fs->inodes_per_group;
    uint32_t bit = (ino - 1U) % fs->inodes_per_group;

    if (group >= fs->group_count) {
        return BSD_ERR(BSD_EIO);
    }

    uint8_t *buffer = (uint8_t *)kalloc(fs->block_size);
    if (buffer == NULL) {
        return BSD_ERR(BSD_ENOMEM);
    }

    sys_status_t status =
        ext2_read_block(fs, fs->groups[group].bg_inode_bitmap, buffer);

    if (status != SYS_OK) {
        kfree(buffer);
        return status;
    }

    uint8_t mask = (uint8_t)(1U << (bit & 7U));

    if ((buffer[bit >> 3] & mask) == 0) {
        kfree(buffer);
        return SYS_OK;
    }

    buffer[bit >> 3] &= (uint8_t)~mask;

    status = ext2_write_block(fs, fs->groups[group].bg_inode_bitmap, buffer);

    if (status != SYS_OK) {
        kfree(buffer);
        return status;
    }

    if (fs->groups[group].bg_free_inodes_count < UINT16_MAX) {
        fs->groups[group].bg_free_inodes_count++;
    }
    if (is_dir && fs->groups[group].bg_used_dirs_count > 0) {
        fs->groups[group].bg_used_dirs_count--;
    }
    if (fs->sb.s_free_inodes_count < UINT32_MAX) {
        fs->sb.s_free_inodes_count++;
    }

    status = ext2_flush_groups(fs);
    if (status == SYS_OK) {
        status = ext2_flush_super(fs);
    }

    if (status == SYS_OK) {
        uint32_t table_block;
        uint32_t offset;

        status = ext2_inode_location(fs, ino, &table_block, &offset);
        if (status == SYS_OK) {
            memset(buffer, 0, fs->block_size);
            status = ext2_read_block(fs, table_block, buffer);

            if (status == SYS_OK) {
                if (offset + fs->inode_size > fs->block_size) {
                    status = BSD_ERR(BSD_EIO);
                } else {
                    memset(buffer + offset, 0, fs->inode_size);
                    status = ext2_write_block(fs, table_block, buffer);
                }
            }
        }
    }

    kfree(buffer);
    return status;
}

static uint32_t ext2_bmap(ext2_fs_t *fs, ext2_inode_t *inode,
                          uint32_t file_block, bool allocate,
                          sys_status_t *status)
{
    uint32_t pointers = fs->block_size / sizeof(uint32_t);

    *status = SYS_OK;

    if (file_block < EXT2_DIRECT_BLOCKS) {
        uint32_t block = inode->block[file_block];

        if (block == 0 && allocate) {
            *status = ext2_alloc_block(fs, &block);
            if (*status != SYS_OK) {
                return 0;
            }
            inode->block[file_block] = block;
            ext2_inode_add_blocks(inode, fs->sectors_per_block);
        }

        return block;
    }

    if (file_block < EXT2_DIRECT_BLOCKS + pointers) {
        uint32_t index = file_block - EXT2_DIRECT_BLOCKS;
        uint32_t indirect = inode->block[EXT2_SINGLE_BLOCK];
        uint8_t *buffer = (uint8_t *)kalloc(fs->block_size);

        if (buffer == NULL) {
            *status = BSD_ERR(BSD_ENOMEM);
            return 0;
        }

        if (indirect == 0) {
            if (!allocate) {
                kfree(buffer);
                return 0;
            }

            *status = ext2_alloc_block(fs, &indirect);
            if (*status != SYS_OK) {
                kfree(buffer);
                return 0;
            }

            inode->block[EXT2_SINGLE_BLOCK] = indirect;
            ext2_inode_add_blocks(inode, fs->sectors_per_block);
            memset(buffer, 0, fs->block_size);
        } else {
            *status = ext2_read_block(fs, indirect, buffer);
            if (*status != SYS_OK) {
                kfree(buffer);
                return 0;
            }
        }

        uint32_t *data = (uint32_t *)(void *)buffer;
        uint32_t block = data[index];

        if (block == 0 && allocate) {
            *status = ext2_alloc_block(fs, &block);
            if (*status == SYS_OK) {
                data[index] = block;
                *status = ext2_write_block(fs, indirect, buffer);
                if (*status == SYS_OK) {
                    ext2_inode_add_blocks(inode, fs->sectors_per_block);
                } else {
                    block = 0;
                }
            }
        }

        kfree(buffer);
        return block;
    }

    if (file_block <
        EXT2_DIRECT_BLOCKS + pointers + (pointers * pointers)) {
        uint32_t relative = file_block - EXT2_DIRECT_BLOCKS - pointers;
        uint32_t first_index = relative / pointers;
        uint32_t second_index = relative % pointers;
        uint32_t double_block = inode->block[EXT2_DOUBLE_BLOCK];
        uint8_t *buffer1 = (uint8_t *)kalloc(fs->block_size);
        uint8_t *buffer2 = (uint8_t *)kalloc(fs->block_size);

        if (buffer1 == NULL || buffer2 == NULL) {
            if (buffer1 != NULL) {
                kfree(buffer1);
            }
            if (buffer2 != NULL) {
                kfree(buffer2);
            }
            *status = BSD_ERR(BSD_ENOMEM);
            return 0;
        }

        if (double_block == 0) {
            if (!allocate) {
                kfree(buffer1);
                kfree(buffer2);
                return 0;
            }

            *status = ext2_alloc_block(fs, &double_block);
            if (*status != SYS_OK) {
                kfree(buffer1);
                kfree(buffer2);
                return 0;
            }

            inode->block[EXT2_DOUBLE_BLOCK] = double_block;
            ext2_inode_add_blocks(inode, fs->sectors_per_block);
            memset(buffer1, 0, fs->block_size);
        } else {
            *status = ext2_read_block(fs, double_block, buffer1);
            if (*status != SYS_OK) {
                kfree(buffer1);
                kfree(buffer2);
                return 0;
            }
        }

        uint32_t *level1 = (uint32_t *)(void *)buffer1;
        uint32_t single_block = level1[first_index];

        if (single_block == 0) {
            if (!allocate) {
                kfree(buffer1);
                kfree(buffer2);
                return 0;
            }

            *status = ext2_alloc_block(fs, &single_block);
            if (*status != SYS_OK) {
                kfree(buffer1);
                kfree(buffer2);
                return 0;
            }

            level1[first_index] = single_block;
            ext2_inode_add_blocks(inode, fs->sectors_per_block);
            *status = ext2_write_block(fs, double_block, buffer1);
            if (*status != SYS_OK) {
                kfree(buffer1);
                kfree(buffer2);
                return 0;
            }
            memset(buffer2, 0, fs->block_size);
        } else {
            *status = ext2_read_block(fs, single_block, buffer2);
            if (*status != SYS_OK) {
                kfree(buffer1);
                kfree(buffer2);
                return 0;
            }
        }

        uint32_t *level2 = (uint32_t *)(void *)buffer2;
        uint32_t block = level2[second_index];

        if (block == 0 && allocate) {
            *status = ext2_alloc_block(fs, &block);
            if (*status == SYS_OK) {
                level2[second_index] = block;
                *status = ext2_write_block(fs, single_block, buffer2);
                if (*status == SYS_OK) {
                    ext2_inode_add_blocks(inode, fs->sectors_per_block);
                } else {
                    block = 0;
                }
            }
        }

        kfree(buffer1);
        kfree(buffer2);
        return block;
    }

    *status = BSD_ERR(BSD_EFBIG);
    return 0;
}

static sys_status_t ext2_free_indirect(ext2_fs_t *fs, ext2_inode_t *inode,
                                       uint32_t block, uint32_t depth)
{
    if (block == 0) {
        return SYS_OK;
    }

    uint8_t *buffer = (uint8_t *)kalloc(fs->block_size);
    if (buffer == NULL) {
        return BSD_ERR(BSD_ENOMEM);
    }

    sys_status_t status = ext2_read_block(fs, block, buffer);

    if (status == SYS_OK) {
        uint32_t count = fs->block_size / sizeof(uint32_t);
        uint32_t *pointers = (uint32_t *)(void *)buffer;

        for (uint32_t i = 0; i < count; i++) {
            if (pointers[i] == 0) {
                continue;
            }

            if (depth == 1) {
                status = ext2_free_block(fs, pointers[i]);
                if (status != SYS_OK) {
                    break;
                }
                ext2_inode_sub_blocks(inode, fs->sectors_per_block);
            } else {
                status = ext2_free_indirect(fs, inode, pointers[i],
                                            depth - 1U);
                if (status != SYS_OK) {
                    break;
                }
            }
        }
    }

    kfree(buffer);

    if (status == SYS_OK) {
        status = ext2_free_block(fs, block);
        ext2_inode_sub_blocks(inode, fs->sectors_per_block);
    }

    return status;
}

static sys_status_t ext2_free_all_blocks(ext2_fs_t *fs, ext2_inode_t *inode)
{
    sys_status_t status = SYS_OK;

    for (uint32_t i = 0; i < EXT2_DIRECT_BLOCKS; i++) {
        if (inode->block[i] == 0) {
            continue;
        }

        status = ext2_free_block(fs, inode->block[i]);
        if (status != SYS_OK) {
            return status;
        }
        inode->block[i] = 0;
        ext2_inode_sub_blocks(inode, fs->sectors_per_block);
    }

    if (inode->block[EXT2_SINGLE_BLOCK] != 0) {
        status = ext2_free_indirect(fs, inode,
                                    inode->block[EXT2_SINGLE_BLOCK], 1);
        inode->block[EXT2_SINGLE_BLOCK] = 0;
        if (status != SYS_OK) {
            return status;
        }
    }

    if (inode->block[EXT2_DOUBLE_BLOCK] != 0) {
        status = ext2_free_indirect(fs, inode,
                                    inode->block[EXT2_DOUBLE_BLOCK], 2);
        inode->block[EXT2_DOUBLE_BLOCK] = 0;
        if (status != SYS_OK) {
            return status;
        }
    }

    if (inode->block[EXT2_TRIPLE_BLOCK] != 0) {
        status = ext2_free_indirect(fs, inode,
                                    inode->block[EXT2_TRIPLE_BLOCK], 3);
        inode->block[EXT2_TRIPLE_BLOCK] = 0;
        if (status != SYS_OK) {
            return status;
        }
    }

    return SYS_OK;
}

static bool ext2_dirent_valid(const ext2_dirent_t *entry, uint32_t offset,
                              uint32_t block_size)
{
    if (entry->rec_len < EXT2_DIRENT_MIN) {
        return false;
    }
    if ((entry->rec_len & 3U) != 0) {
        return false;
    }
    if (offset > block_size || entry->rec_len > block_size - offset) {
        return false;
    }
    if ((uint32_t)entry->name_len >
        (uint32_t)entry->rec_len - EXT2_DIRENT_MIN) {
        return false;
    }
    return true;
}

static bool ext2_name_is_dot(const ext2_dirent_t *entry)
{
    if (entry->name_len == 1 && entry->name[0] == '.') {
        return true;
    }
    if (entry->name_len == 2 && entry->name[0] == '.' &&
        entry->name[1] == '.') {
        return true;
    }
    return false;
}

static sys_status_t ext2_dir_find(ext2_fs_t *fs, ext2_inode_t *dir,
                                  const char *name, ext2_dir_scan_t *scan)
{
    size_t name_len = strlen(name);

    if (name_len == 0 || name_len > BSD_NAME_MAX || name_len > 255U) {
        return BSD_ERR(BSD_ENAMETOOLONG);
    }

    uint64_t blocks = (dir->size + fs->block_size - 1U) / fs->block_size;

    for (uint64_t index = 0; index < blocks; index++) {
        sys_status_t status = SYS_OK;
        uint32_t block = ext2_bmap(fs, dir, (uint32_t)index, false, &status);

        if (status != SYS_OK) {
            return status;
        }
        if (block == 0) {
            continue;
        }

        uint8_t *buffer = (uint8_t *)kalloc(fs->block_size);
        if (buffer == NULL) {
            return BSD_ERR(BSD_ENOMEM);
        }

        status = ext2_read_block(fs, block, buffer);
        if (status != SYS_OK) {
            kfree(buffer);
            return status;
        }

        uint32_t offset = 0;
        uint32_t previous = EXT2_DIR_PREV_NONE;

        while (offset < fs->block_size) {
            const ext2_dirent_t *entry =
                (const ext2_dirent_t *)(const void *)(buffer + offset);

            if (!ext2_dirent_valid(entry, offset, fs->block_size)) {
                kfree(buffer);
                return BSD_ERR(BSD_EIO);
            }

            if (entry->inode != 0 && entry->name_len == name_len &&
                memcmp(entry->name, name, name_len) == 0) {
                scan->block = block;
                scan->offset = offset;
                scan->prev_offset = previous;
                scan->ino = entry->inode;
                scan->type = entry->file_type;
                memcpy(scan->name, entry->name, name_len);
                scan->name[name_len] = '\0';

                kfree(buffer);
                return SYS_OK;
            }

            previous = offset;
            offset += entry->rec_len;
        }

        kfree(buffer);
    }

    return BSD_ERR(BSD_ENOENT);
}

static sys_status_t ext2_dir_remove(ext2_fs_t *fs, const ext2_dir_scan_t *scan)
{
    uint8_t *buffer = (uint8_t *)kalloc(fs->block_size);
    if (buffer == NULL) {
        return BSD_ERR(BSD_ENOMEM);
    }

    sys_status_t status = ext2_read_block(fs, scan->block, buffer);

    if (status == SYS_OK) {
        ext2_dirent_t *entry =
            (ext2_dirent_t *)(void *)(buffer + scan->offset);

        if (!ext2_dirent_valid(entry, scan->offset, fs->block_size)) {
            status = BSD_ERR(BSD_EIO);
        } else if (scan->prev_offset != EXT2_DIR_PREV_NONE) {
            ext2_dirent_t *previous =
                (ext2_dirent_t *)(void *)(buffer + scan->prev_offset);

            if (!ext2_dirent_valid(previous, scan->prev_offset,
                                   fs->block_size)) {
                status = BSD_ERR(BSD_EIO);
            } else {
                previous->rec_len =
                    (uint16_t)(previous->rec_len + entry->rec_len);
                status = ext2_write_block(fs, scan->block, buffer);
            }
        } else {
            entry->inode = 0;
            status = ext2_write_block(fs, scan->block, buffer);
        }
    }

    kfree(buffer);
    return status;
}

static sys_status_t ext2_dir_add(ext2_fs_t *fs, ext2_inode_t *dir,
                                 const char *name, uint32_t ino, uint8_t type,
                                 uint32_t *new_size)
{
    size_t name_len = strlen(name);

    if (name_len == 0 || name_len > BSD_NAME_MAX || name_len > 255U) {
        return BSD_ERR(BSD_ENAMETOOLONG);
    }
    if (dir->size > UINT32_MAX) {
        return BSD_ERR(BSD_EFBIG);
    }

    size_t needed = ALIGN_UP(EXT2_DIRENT_MIN + name_len, 4U);

    if (needed > fs->block_size) {
        return BSD_ERR(BSD_ENAMETOOLONG);
    }

    uint64_t blocks = (dir->size + fs->block_size - 1U) / fs->block_size;

    for (uint64_t index = 0; index < blocks; index++) {
        sys_status_t status = SYS_OK;
        uint32_t block = ext2_bmap(fs, dir, (uint32_t)index, false, &status);

        if (status != SYS_OK) {
            return status;
        }
        if (block == 0) {
            continue;
        }

        uint8_t *buffer = (uint8_t *)kalloc(fs->block_size);
        if (buffer == NULL) {
            return BSD_ERR(BSD_ENOMEM);
        }

        status = ext2_read_block(fs, block, buffer);
        if (status != SYS_OK) {
            kfree(buffer);
            return status;
        }

        uint32_t offset = 0;

        while (offset < fs->block_size) {
            ext2_dirent_t *entry =
                (ext2_dirent_t *)(void *)(buffer + offset);

            if (!ext2_dirent_valid(entry, offset, fs->block_size)) {
                kfree(buffer);
                return BSD_ERR(BSD_EIO);
            }

            if (entry->inode != 0) {
                size_t used =
                    ALIGN_UP(EXT2_DIRENT_MIN + entry->name_len, 4U);

                if (used <= entry->rec_len &&
                    entry->rec_len - used >= needed) {
                    ext2_dirent_t *added =
                        (ext2_dirent_t *)(void *)(buffer + offset + used);
                    size_t slack = entry->rec_len - used;

                    entry->rec_len = (uint16_t)used;
                    added->inode = ino;
                    added->rec_len = (uint16_t)slack;
                    added->name_len = (uint8_t)name_len;
                    added->file_type = type;
                    memcpy(added->name, name, name_len);

                    status = ext2_write_block(fs, block, buffer);
                    kfree(buffer);

                    if (status == SYS_OK) {
                        *new_size = (uint32_t)dir->size;
                    }
                    return status;
                }
            }

            offset += entry->rec_len;
        }

        kfree(buffer);
    }

    uint32_t file_block =
        (uint32_t)((dir->size + fs->block_size - 1U) / fs->block_size);
    sys_status_t status = SYS_OK;
    uint32_t block = ext2_bmap(fs, dir, file_block, true, &status);

    if (status != SYS_OK) {
        return status;
    }
    if (block == 0) {
        return BSD_ERR(BSD_ENOSPC);
    }

    uint8_t *buffer = (uint8_t *)kalloc(fs->block_size);
    if (buffer == NULL) {
        return BSD_ERR(BSD_ENOMEM);
    }

    memset(buffer, 0, fs->block_size);

    ext2_dirent_t *entry = (ext2_dirent_t *)(void *)buffer;
    entry->inode = ino;
    entry->rec_len = (uint16_t)fs->block_size;
    entry->name_len = (uint8_t)name_len;
    entry->file_type = type;
    memcpy(entry->name, name, name_len);

    status = ext2_write_block(fs, block, buffer);
    kfree(buffer);

    if (status != SYS_OK) {
        return status;
    }

    dir->size = ((uint64_t)file_block + 1U) * fs->block_size;
    *new_size = (uint32_t)dir->size;
    return SYS_OK;
}

static sys_status_t ext2_dir_is_empty(ext2_fs_t *fs, ext2_inode_t *dir,
                                      bool *empty)
{
    *empty = true;

    uint64_t blocks = (dir->size + fs->block_size - 1U) / fs->block_size;

    for (uint64_t index = 0; index < blocks; index++) {
        sys_status_t status = SYS_OK;
        uint32_t block = ext2_bmap(fs, dir, (uint32_t)index, false, &status);

        if (status != SYS_OK) {
            return status;
        }
        if (block == 0) {
            continue;
        }

        uint8_t *buffer = (uint8_t *)kalloc(fs->block_size);
        if (buffer == NULL) {
            return BSD_ERR(BSD_ENOMEM);
        }

        status = ext2_read_block(fs, block, buffer);
        if (status != SYS_OK) {
            kfree(buffer);
            return status;
        }

        uint32_t offset = 0;

        while (offset < fs->block_size) {
            const ext2_dirent_t *entry =
                (const ext2_dirent_t *)(const void *)(buffer + offset);

            if (!ext2_dirent_valid(entry, offset, fs->block_size)) {
                kfree(buffer);
                return BSD_ERR(BSD_EIO);
            }

            if (entry->inode != 0 && !ext2_name_is_dot(entry)) {
                *empty = false;
                kfree(buffer);
                return SYS_OK;
            }

            offset += entry->rec_len;
        }

        kfree(buffer);
    }

    return SYS_OK;
}

static sys_status_t ext2_make_vnode(ext2_fs_t *fs, uint32_t ino,
                                    vnode_t *parent, const char *name,
                                    vnode_t **out)
{
    ext2_inode_t inode;
    sys_status_t status = ext2_read_inode(fs, ino, &inode);

    if (status != SYS_OK) {
        return status;
    }
    if (inode.mode == 0) {
        return BSD_ERR(BSD_ENOENT);
    }

    vnode_t *vnode = (vnode_t *)kalloc(sizeof(vnode_t));
    ext2_node_t *node = (ext2_node_t *)kalloc(sizeof(ext2_node_t));

    if (vnode == NULL || node == NULL) {
        if (vnode != NULL) {
            kfree(vnode);
        }
        if (node != NULL) {
            kfree(node);
        }
        return BSD_ERR(BSD_ENOMEM);
    }

    memset(vnode, 0, sizeof(*vnode));
    memset(node, 0, sizeof(*node));

    node->fs = fs;
    node->ino = ino;

    ext2_open_entry_t *entry = ext2_open_get(fs, ino);
    if (entry == NULL) {
        kfree(vnode);
        kfree(node);
        return BSD_ERR(BSD_ENOMEM);
    }
    entry->refs++;
    node->open = entry;

    if (name != NULL) {
        size_t length = strlen(name);
        if (length > BSD_NAME_MAX) {
            length = BSD_NAME_MAX;
        }
        memcpy(node->name, name, length);
        node->name[length] = '\0';
    }

    vnode->type = (inode.mode & EXT2_S_IFMT) == EXT2_S_IFDIR ? VNODE_DIR
                                                             : VNODE_REG;
    vnode->mode = inode.mode;
    vnode->size = inode.size;
    vnode->ino = ino;
    vnode->ref_count = 1;
    vnode->ops = &ext2_vnode_ops;
    vnode->fs_data = node;
    vnode->parent = parent != NULL ? vfs_vnode_ref(parent) : NULL;
    vnode->name = node->name;

    *out = vnode;
    return SYS_OK;
}

static void ext2_release(vnode_t *vnode)
{
    ext2_node_t *node = (ext2_node_t *)vnode->fs_data;
    vnode_t *parent = vnode->parent;
    ext2_open_entry_t *entry = node != NULL ? node->open : NULL;

    vnode->fs_data = NULL;
    vnode->parent = NULL;

    if (node != NULL) {
        kfree(node);
    }

    if (entry != NULL) {
        spin_lock(&ext2_lock);

        if (entry->refs > 0) {
            entry->refs--;
        }

        if (entry->refs == 0) {
            if (entry->unlinked) {
                ext2_inode_t inode;

                if (ext2_read_inode(entry->fs, entry->ino, &inode) == SYS_OK &&
                    inode.mode != 0) {
                    bool is_dir =
                        (inode.mode & EXT2_S_IFMT) == EXT2_S_IFDIR;

                    if (ext2_free_all_blocks(entry->fs, &inode) == SYS_OK) {
                        (void)ext2_write_inode(entry->fs, entry->ino, &inode);
                    }
                    (void)ext2_free_inode(entry->fs, entry->ino, is_dir);
                }
            }

            ext2_open_remove(entry);
        }

        spin_unlock(&ext2_lock);
    }

    if (parent != NULL) {
        vfs_vnode_unref(parent);
    }
    kfree(vnode);
}

static sys_status_t ext2_read(vnode_t *vnode, void *buf, size_t len,
                              uint64_t offset, size_t *done)
{
    if (vnode == NULL || done == NULL || (buf == NULL && len != 0)) {
        return BSD_ERR(BSD_EINVAL);
    }

    *done = 0;

    ext2_node_t *node = (ext2_node_t *)vnode->fs_data;
    if (node == NULL) {
        return BSD_ERR(BSD_EINVAL);
    }

    ext2_fs_t *fs = node->fs;

    spin_lock(&ext2_lock);

    ext2_inode_t inode;
    sys_status_t status = ext2_read_inode(fs, node->ino, &inode);

    if (status != SYS_OK) {
        spin_unlock(&ext2_lock);
        return status;
    }
    if ((inode.mode & EXT2_S_IFMT) == EXT2_S_IFDIR) {
        spin_unlock(&ext2_lock);
        return BSD_ERR(BSD_EISDIR);
    }

    vnode->size = inode.size;

    if (offset >= inode.size || len == 0) {
        spin_unlock(&ext2_lock);
        return SYS_OK;
    }

    size_t available = (size_t)(inode.size - offset);
    size_t count = len < available ? len : available;
    uint8_t *out = (uint8_t *)buf;
    uint8_t *buffer = (uint8_t *)kalloc(fs->block_size);

    if (buffer == NULL) {
        spin_unlock(&ext2_lock);
        return BSD_ERR(BSD_ENOMEM);
    }

    size_t transferred = 0;

    while (transferred < count) {
        uint64_t absolute = offset + transferred;
        uint64_t block_index = absolute / fs->block_size;

        if (block_index > UINT32_MAX) {
            status = BSD_ERR(BSD_EFBIG);
            break;
        }

        uint32_t file_block = (uint32_t)block_index;
        uint32_t in_block = (uint32_t)(absolute % fs->block_size);
        size_t chunk = fs->block_size - in_block;

        if (chunk > count - transferred) {
            chunk = count - transferred;
        }

        uint32_t block = ext2_bmap(fs, &inode, file_block, false, &status);

        if (status != SYS_OK) {
            break;
        }

        if (block == 0) {
            memset(out + transferred, 0, chunk);
        } else {
            status = ext2_read_block(fs, block, buffer);
            if (status != SYS_OK) {
                break;
            }
            memcpy(out + transferred, buffer + in_block, chunk);
        }

        transferred += chunk;
    }

    kfree(buffer);
    spin_unlock(&ext2_lock);

    *done = transferred;
    return status;
}

static sys_status_t ext2_write(vnode_t *vnode, const void *buf, size_t len,
                               uint64_t offset, size_t *done)
{
    if (vnode == NULL || done == NULL || (buf == NULL && len != 0)) {
        return BSD_ERR(BSD_EINVAL);
    }

    *done = 0;

    ext2_node_t *node = (ext2_node_t *)vnode->fs_data;
    if (node == NULL) {
        return BSD_ERR(BSD_EINVAL);
    }

    ext2_fs_t *fs = node->fs;

    if (offset > UINT64_MAX - len) {
        return BSD_ERR(BSD_EFBIG);
    }

    spin_lock(&ext2_lock);

    ext2_inode_t inode;
    sys_status_t status = ext2_read_inode(fs, node->ino, &inode);

    if (status != SYS_OK) {
        spin_unlock(&ext2_lock);
        return status;
    }
    if ((inode.mode & EXT2_S_IFMT) == EXT2_S_IFDIR) {
        spin_unlock(&ext2_lock);
        return BSD_ERR(BSD_EISDIR);
    }

    const uint8_t *in = (const uint8_t *)buf;
    uint8_t *buffer = (uint8_t *)kalloc(fs->block_size);

    if (buffer == NULL) {
        spin_unlock(&ext2_lock);
        return BSD_ERR(BSD_ENOMEM);
    }

    size_t transferred = 0;

    while (transferred < len) {
        uint64_t absolute = offset + transferred;
        uint64_t block_index = absolute / fs->block_size;

        if (block_index > UINT32_MAX) {
            status = BSD_ERR(BSD_EFBIG);
            break;
        }

        uint32_t file_block = (uint32_t)block_index;
        uint32_t in_block = (uint32_t)(absolute % fs->block_size);
        size_t chunk = fs->block_size - in_block;

        if (chunk > len - transferred) {
            chunk = len - transferred;
        }

        uint32_t block = ext2_bmap(fs, &inode, file_block, true, &status);

        if (status != SYS_OK) {
            break;
        }
        if (block == 0) {
            status = BSD_ERR(BSD_EIO);
            break;
        }

        bool full_block = in_block == 0 && chunk == fs->block_size;

        if (!full_block) {
            status = ext2_read_block(fs, block, buffer);
            if (status != SYS_OK) {
                break;
            }
        }

        memcpy(buffer + in_block, in + transferred, chunk);

        status = ext2_write_block(fs, block, buffer);
        if (status != SYS_OK) {
            break;
        }

        transferred += chunk;
    }

    kfree(buffer);

    uint64_t new_size = inode.size;

    if (offset + transferred > new_size) {
        new_size = offset + transferred;
    }

    inode.size = new_size;
    vnode->size = new_size;

    if (transferred != 0 || status != SYS_OK) {
        sys_status_t write_status = ext2_write_inode(fs, node->ino, &inode);
        if (status == SYS_OK) {
            status = write_status;
        }
    }

    spin_unlock(&ext2_lock);

    *done = transferred;
    return status;
}

static sys_status_t ext2_lookup(vnode_t *dir, const char *name,
                                vnode_t **out)
{
    if (dir == NULL || name == NULL || out == NULL) {
        return BSD_ERR(BSD_EINVAL);
    }
    if (dir->type != VNODE_DIR) {
        return BSD_ERR(BSD_ENOTDIR);
    }

    ext2_node_t *node = (ext2_node_t *)dir->fs_data;
    if (node == NULL) {
        return BSD_ERR(BSD_EINVAL);
    }

    ext2_fs_t *fs = node->fs;
    ext2_dir_scan_t scan;

    spin_lock(&ext2_lock);

    ext2_inode_t inode;
    sys_status_t status = ext2_read_inode(fs, node->ino, &inode);

    if (status == SYS_OK) {
        status = ext2_dir_find(fs, &inode, name, &scan);
    }

    if (status == SYS_OK) {
        status = ext2_make_vnode(fs, scan.ino, dir, name, out);
    }

    spin_unlock(&ext2_lock);

    return status;
}

static sys_status_t ext2_readdir(vnode_t *dir, uint64_t index,
                                 bsd_dirent_t *entry)
{
    if (dir == NULL || entry == NULL) {
        return BSD_ERR(BSD_EINVAL);
    }
    if (dir->type != VNODE_DIR) {
        return BSD_ERR(BSD_ENOTDIR);
    }

    ext2_node_t *node = (ext2_node_t *)dir->fs_data;
    if (node == NULL) {
        return BSD_ERR(BSD_EINVAL);
    }

    ext2_fs_t *fs = node->fs;

    spin_lock(&ext2_lock);

    ext2_inode_t inode;
    sys_status_t status = ext2_read_inode(fs, node->ino, &inode);

    if (status != SYS_OK) {
        spin_unlock(&ext2_lock);
        return status;
    }

    uint64_t blocks = (inode.size + fs->block_size - 1U) / fs->block_size;
    uint64_t seen = 0;

    for (uint64_t block_index = 0; block_index < blocks; block_index++) {
        uint32_t block =
            ext2_bmap(fs, &inode, (uint32_t)block_index, false, &status);

        if (status != SYS_OK) {
            break;
        }
        if (block == 0) {
            continue;
        }

        uint8_t *buffer = (uint8_t *)kalloc(fs->block_size);
        if (buffer == NULL) {
            status = BSD_ERR(BSD_ENOMEM);
            break;
        }

        status = ext2_read_block(fs, block, buffer);
        if (status != SYS_OK) {
            kfree(buffer);
            break;
        }

        uint32_t offset = 0;

        while (offset < fs->block_size) {
            const ext2_dirent_t *item =
                (const ext2_dirent_t *)(const void *)(buffer + offset);

            if (!ext2_dirent_valid(item, offset, fs->block_size)) {
                status = BSD_ERR(BSD_EIO);
                break;
            }

            if (item->inode != 0 && !ext2_name_is_dot(item)) {
                if (seen == index) {
                    size_t name_len = item->name_len;

                    memset(entry, 0, sizeof(*entry));
                    memcpy(entry->d_name, item->name, name_len);
                    entry->d_name[name_len] = '\0';
                    entry->d_namlen = (uint8_t)name_len;
                    entry->d_ino = item->inode;
                    entry->d_type = item->file_type == EXT2_FT_DIR
                                        ? BSD_DT_DIR
                                        : BSD_DT_REG;
                    entry->d_reclen = (uint16_t)sizeof(*entry);

                    kfree(buffer);
                    spin_unlock(&ext2_lock);
                    return SYS_OK;
                }
                seen++;
            }

            offset += item->rec_len;
        }

        kfree(buffer);

        if (status != SYS_OK) {
            break;
        }
    }

    spin_unlock(&ext2_lock);

    return status == SYS_OK ? BSD_ERR(BSD_ENOENT) : status;
}

static uint32_t ext2_mode_defaults(uint32_t mode, uint32_t fallback)
{
    uint32_t permissions = mode & 0777U;

    if (permissions == 0) {
        permissions = fallback;
    }

    return permissions;
}

static void ext2_discard_inode(ext2_fs_t *fs, uint32_t ino, bool is_dir)
{
    ext2_inode_t dead;

    if (ext2_read_inode(fs, ino, &dead) == SYS_OK && dead.mode != 0) {
        if (ext2_free_all_blocks(fs, &dead) == SYS_OK) {
            (void)ext2_write_inode(fs, ino, &dead);
        }
    }

    (void)ext2_free_inode(fs, ino, is_dir);
}

static sys_status_t ext2_create(vnode_t *dir, const char *name, uint32_t mode,
                                vnode_t **out)
{
    if (dir == NULL || name == NULL || out == NULL) {
        return BSD_ERR(BSD_EINVAL);
    }
    if (dir->type != VNODE_DIR) {
        return BSD_ERR(BSD_ENOTDIR);
    }

    ext2_node_t *node = (ext2_node_t *)dir->fs_data;
    if (node == NULL) {
        return BSD_ERR(BSD_EINVAL);
    }

    ext2_fs_t *fs = node->fs;
    uint16_t file_mode =
        (uint16_t)(EXT2_S_IFREG | ext2_mode_defaults(mode, 0644U));

    spin_lock(&ext2_lock);

    ext2_inode_t parent;
    sys_status_t status = ext2_read_inode(fs, node->ino, &parent);

    if (status != SYS_OK) {
        spin_unlock(&ext2_lock);
        return status;
    }

    ext2_dir_scan_t existing;
    status = ext2_dir_find(fs, &parent, name, &existing);

    if (status == SYS_OK) {
        spin_unlock(&ext2_lock);
        return BSD_ERR(BSD_EEXIST);
    }
    if (status != BSD_ERR(BSD_ENOENT)) {
        spin_unlock(&ext2_lock);
        return status;
    }

    uint32_t ino = 0;
    status = ext2_alloc_inode(fs, file_mode, &ino);

    if (status != SYS_OK) {
        spin_unlock(&ext2_lock);
        return status;
    }

    vnode_t *created = NULL;

    status = ext2_make_vnode(fs, ino, dir, name, &created);
    if (status != SYS_OK) {
        (void)ext2_free_inode(fs, ino, false);
        spin_unlock(&ext2_lock);
        return status;
    }

    uint32_t new_size = (uint32_t)parent.size;

    status = ext2_dir_add(fs, &parent, name, ino, EXT2_FT_REG_FILE,
                          &new_size);

    if (status != SYS_OK) {
        (void)ext2_free_inode(fs, ino, false);
        spin_unlock(&ext2_lock);
        vfs_vnode_unref(created);
        return status;
    }

    parent.size = new_size;
    sys_status_t write_status = ext2_write_inode(fs, node->ino, &parent);

    if (write_status != SYS_OK) {
        /* The dentry is already durable; keep the inode linked. */
        spin_unlock(&ext2_lock);
        vfs_vnode_unref(created);
        return write_status;
    }

    dir->size = parent.size;

    spin_unlock(&ext2_lock);

    *out = created;
    return SYS_OK;
}

static sys_status_t ext2_mkdir(vnode_t *dir, const char *name, uint32_t mode,
                               vnode_t **out)
{
    if (dir == NULL || name == NULL || out == NULL) {
        return BSD_ERR(BSD_EINVAL);
    }
    if (dir->type != VNODE_DIR) {
        return BSD_ERR(BSD_ENOTDIR);
    }

    ext2_node_t *node = (ext2_node_t *)dir->fs_data;
    if (node == NULL) {
        return BSD_ERR(BSD_EINVAL);
    }

    ext2_fs_t *fs = node->fs;
    uint16_t file_mode =
        (uint16_t)(EXT2_S_IFDIR | ext2_mode_defaults(mode, 0755U));

    spin_lock(&ext2_lock);

    ext2_inode_t parent;
    sys_status_t status = ext2_read_inode(fs, node->ino, &parent);

    if (status != SYS_OK) {
        spin_unlock(&ext2_lock);
        return status;
    }

    ext2_dir_scan_t existing;
    status = ext2_dir_find(fs, &parent, name, &existing);

    if (status == SYS_OK) {
        spin_unlock(&ext2_lock);
        return BSD_ERR(BSD_EEXIST);
    }
    if (status != BSD_ERR(BSD_ENOENT)) {
        spin_unlock(&ext2_lock);
        return status;
    }

    uint32_t ino = 0;
    status = ext2_alloc_inode(fs, file_mode, &ino);

    if (status != SYS_OK) {
        spin_unlock(&ext2_lock);
        return status;
    }

    ext2_inode_t child;
    status = ext2_read_inode(fs, ino, &child);

    uint32_t block = 0;

    if (status == SYS_OK) {
        block = ext2_bmap(fs, &child, 0, true, &status);
    }

    uint8_t *buffer = NULL;

    if (status == SYS_OK) {
        buffer = (uint8_t *)kalloc(fs->block_size);
        if (buffer == NULL) {
            status = BSD_ERR(BSD_ENOMEM);
        }
    }

    if (status == SYS_OK) {
        memset(buffer, 0, fs->block_size);

        ext2_dirent_t *dot = (ext2_dirent_t *)(void *)buffer;
        dot->inode = ino;
        dot->rec_len = 12;
        dot->name_len = 1;
        dot->file_type = EXT2_FT_DIR;
        dot->name[0] = '.';

        ext2_dirent_t *dotdot = (ext2_dirent_t *)(void *)(buffer + 12);
        dotdot->inode = node->ino;
        dotdot->rec_len = (uint16_t)(fs->block_size - 12U);
        dotdot->name_len = 2;
        dotdot->file_type = EXT2_FT_DIR;
        dotdot->name[0] = '.';
        dotdot->name[1] = '.';

        status = ext2_write_block(fs, block, buffer);
    }

    if (buffer != NULL) {
        kfree(buffer);
    }

    if (status == SYS_OK) {
        child.size = fs->block_size;
        status = ext2_write_inode(fs, ino, &child);
    }

    vnode_t *created = NULL;

    if (status == SYS_OK) {
        status = ext2_make_vnode(fs, ino, dir, name, &created);
    }
    if (status != SYS_OK) {
        ext2_discard_inode(fs, ino, true);
        spin_unlock(&ext2_lock);
        return status;
    }

    uint32_t new_size = (uint32_t)parent.size;

    status = ext2_dir_add(fs, &parent, name, ino, EXT2_FT_DIR, &new_size);
    if (status != SYS_OK) {
        ext2_discard_inode(fs, ino, true);
        spin_unlock(&ext2_lock);
        vfs_vnode_unref(created);
        return status;
    }

    parent.size = new_size;
    if (parent.links_count < UINT16_MAX) {
        parent.links_count++;
    }

    sys_status_t write_status = ext2_write_inode(fs, node->ino, &parent);

    if (write_status != SYS_OK) {
        /* The dentry is already durable; keep the inode linked. */
        spin_unlock(&ext2_lock);
        vfs_vnode_unref(created);
        return write_status;
    }

    dir->size = parent.size;

    spin_unlock(&ext2_lock);

    *out = created;
    return SYS_OK;
}

static sys_status_t ext2_unlink(vnode_t *dir, const char *name)
{
    if (dir == NULL || name == NULL) {
        return BSD_ERR(BSD_EINVAL);
    }
    if (dir->type != VNODE_DIR) {
        return BSD_ERR(BSD_ENOTDIR);
    }

    ext2_node_t *node = (ext2_node_t *)dir->fs_data;
    if (node == NULL) {
        return BSD_ERR(BSD_EINVAL);
    }

    ext2_fs_t *fs = node->fs;

    spin_lock(&ext2_lock);

    ext2_inode_t parent;
    sys_status_t status = ext2_read_inode(fs, node->ino, &parent);

    if (status != SYS_OK) {
        spin_unlock(&ext2_lock);
        return status;
    }

    ext2_dir_scan_t scan;
    status = ext2_dir_find(fs, &parent, name, &scan);

    if (status != SYS_OK) {
        spin_unlock(&ext2_lock);
        return status;
    }
    if (scan.ino == EXT2_ROOT_INO) {
        spin_unlock(&ext2_lock);
        return BSD_ERR(BSD_EINVAL);
    }

    ext2_inode_t target;
    status = ext2_read_inode(fs, scan.ino, &target);

    if (status != SYS_OK) {
        spin_unlock(&ext2_lock);
        return status;
    }
    if ((target.mode & EXT2_S_IFMT) == EXT2_S_IFDIR) {
        spin_unlock(&ext2_lock);
        return BSD_ERR(BSD_EISDIR);
    }

    status = ext2_dir_remove(fs, &scan);

    if (status == SYS_OK) {
        if (target.links_count > 0) {
            target.links_count--;
        }

        if (target.links_count == 0) {
            ext2_open_entry_t *entry = ext2_open_find(fs, scan.ino);

            if (entry != NULL && entry->refs > 0) {
                entry->unlinked = true;
                status = ext2_write_inode(fs, scan.ino, &target);
            } else {
                status = ext2_free_all_blocks(fs, &target);
                if (status == SYS_OK) {
                    status = ext2_free_inode(fs, scan.ino, false);
                }
            }
        } else {
            status = ext2_write_inode(fs, scan.ino, &target);
        }
    }

    spin_unlock(&ext2_lock);

    return status;
}

static sys_status_t ext2_rmdir(vnode_t *dir, const char *name)
{
    if (dir == NULL || name == NULL) {
        return BSD_ERR(BSD_EINVAL);
    }
    if (dir->type != VNODE_DIR) {
        return BSD_ERR(BSD_ENOTDIR);
    }

    ext2_node_t *node = (ext2_node_t *)dir->fs_data;
    if (node == NULL) {
        return BSD_ERR(BSD_EINVAL);
    }

    ext2_fs_t *fs = node->fs;

    spin_lock(&ext2_lock);

    ext2_inode_t parent;
    sys_status_t status = ext2_read_inode(fs, node->ino, &parent);

    if (status != SYS_OK) {
        spin_unlock(&ext2_lock);
        return status;
    }

    ext2_dir_scan_t scan;
    status = ext2_dir_find(fs, &parent, name, &scan);

    if (status != SYS_OK) {
        spin_unlock(&ext2_lock);
        return status;
    }
    if (scan.ino == EXT2_ROOT_INO || scan.ino == node->ino) {
        spin_unlock(&ext2_lock);
        return BSD_ERR(BSD_EINVAL);
    }

    ext2_inode_t child;
    status = ext2_read_inode(fs, scan.ino, &child);

    if (status != SYS_OK) {
        spin_unlock(&ext2_lock);
        return status;
    }
    if ((child.mode & EXT2_S_IFMT) != EXT2_S_IFDIR) {
        spin_unlock(&ext2_lock);
        return BSD_ERR(BSD_ENOTDIR);
    }

    bool empty = false;
    status = ext2_dir_is_empty(fs, &child, &empty);

    if (status != SYS_OK) {
        spin_unlock(&ext2_lock);
        return status;
    }
    if (!empty) {
        spin_unlock(&ext2_lock);
        return BSD_ERR(BSD_ENOTEMPTY);
    }

    status = ext2_dir_remove(fs, &scan);

    if (status == SYS_OK) {
        if (parent.links_count > 0) {
            parent.links_count--;
        }
        status = ext2_write_inode(fs, node->ino, &parent);
    }

    if (status == SYS_OK) {
        ext2_open_entry_t *entry = ext2_open_find(fs, scan.ino);

        if (entry != NULL && entry->refs > 0) {
            entry->unlinked = true;
            child.links_count = 0;
            status = ext2_write_inode(fs, scan.ino, &child);
        } else {
            status = ext2_free_all_blocks(fs, &child);
            if (status == SYS_OK) {
                status = ext2_free_inode(fs, scan.ino, true);
            }
        }
    }

    spin_unlock(&ext2_lock);

    return status;
}

static sys_status_t ext2_get_size(vnode_t *vnode, uint64_t *size)
{
    if (vnode == NULL || size == NULL) {
        return BSD_ERR(BSD_EINVAL);
    }

    ext2_node_t *node = (ext2_node_t *)vnode->fs_data;
    if (node == NULL) {
        return BSD_ERR(BSD_EINVAL);
    }

    ext2_fs_t *fs = node->fs;

    spin_lock(&ext2_lock);

    ext2_inode_t inode;
    sys_status_t status = ext2_read_inode(fs, node->ino, &inode);

    if (status == SYS_OK) {
        vnode->size = inode.size;
        *size = inode.size;
    }

    spin_unlock(&ext2_lock);
    return status;
}

static sys_status_t ext2_truncate(vnode_t *vnode)
{
    if (vnode == NULL) {
        return BSD_ERR(BSD_EINVAL);
    }

    ext2_node_t *node = (ext2_node_t *)vnode->fs_data;
    if (node == NULL) {
        return BSD_ERR(BSD_EINVAL);
    }

    ext2_fs_t *fs = node->fs;

    spin_lock(&ext2_lock);

    ext2_inode_t inode;
    sys_status_t status = ext2_read_inode(fs, node->ino, &inode);

    if (status == SYS_OK) {
        if ((inode.mode & EXT2_S_IFMT) == EXT2_S_IFDIR) {
            status = BSD_ERR(BSD_EISDIR);
        } else {
            status = ext2_free_all_blocks(fs, &inode);
        }
    }

    if (status == SYS_OK) {
        inode.size = 0;
        inode.blocks = 0;
        status = ext2_write_inode(fs, node->ino, &inode);
        if (status == SYS_OK) {
            vnode->size = 0;
        }
    }

    spin_unlock(&ext2_lock);
    return status;
}

sys_status_t ext2_mount(blkdev_t *dev)
{
    if (dev == NULL || dev->read == NULL || dev->write == NULL) {
        return BSD_ERR(BSD_EINVAL);
    }

    if (ext2_fs.mounted) {
        return SYS_OK;
    }

    spinlock_init(&ext2_lock);
    spin_lock(&ext2_lock);

    memset(&ext2_fs, 0, sizeof(ext2_fs));
    ext2_open_head = NULL;
    ext2_fs.dev = dev;

    uint8_t *buffer = (uint8_t *)kalloc(EXT2_SUPER_BYTES);
    if (buffer == NULL) {
        spin_unlock(&ext2_lock);
        return BSD_ERR(BSD_ENOMEM);
    }

    sys_status_t status = ext2_io(ext2_lba_transfer(
        &ext2_fs, EXT2_SUPER_OFFSET / BLK_SECTOR_SIZE,
        EXT2_SUPER_BYTES / BLK_SECTOR_SIZE, buffer, false));

    if (status != SYS_OK) {
        kfree(buffer);
        spin_unlock(&ext2_lock);
        return status;
    }

    memcpy(ext2_fs.sb_raw, buffer, EXT2_SUPER_BYTES);
    memcpy(&ext2_fs.sb, ext2_fs.sb_raw, sizeof(ext2_fs.sb));
    kfree(buffer);

    if (ext2_fs.sb.s_magic != EXT2_SUPER_MAGIC) {
        klog(KLOG_WARN, "ext2: superblock magic is 0x%04x",
             (uint64_t)ext2_fs.sb.s_magic);
        spin_unlock(&ext2_lock);
        return BSD_ERR(BSD_EINVAL);
    }

    if (ext2_fs.sb.s_log_block_size > 5U) {
        spin_unlock(&ext2_lock);
        return BSD_ERR(BSD_EINVAL);
    }

    uint32_t block_size = 1024U << ext2_fs.sb.s_log_block_size;

    if (block_size > EXT2_MAX_BLOCK_SIZE) {
        spin_unlock(&ext2_lock);
        return BSD_ERR(BSD_EINVAL);
    }
    if (ext2_fs.sb.s_blocks_per_group == 0 ||
        ext2_fs.sb.s_inodes_per_group == 0 ||
        ext2_fs.sb.s_blocks_count == 0 ||
        ext2_fs.sb.s_inodes_count == 0) {
        spin_unlock(&ext2_lock);
        return BSD_ERR(BSD_EINVAL);
    }

    uint32_t inode_size = ext2_fs.sb.s_rev_level == 0
                              ? EXT2_INODE_BYTES
                              : ext2_fs.sb.s_inode_size;

    if (inode_size < EXT2_INODE_BYTES || inode_size > block_size ||
        (inode_size & (inode_size - 1U)) != 0 ||
        (block_size % inode_size) != 0) {
        spin_unlock(&ext2_lock);
        return BSD_ERR(BSD_EINVAL);
    }

    if ((ext2_fs.sb.s_feature_incompat &
         ~(uint32_t)EXT2_FEATURE_INCOMPAT_FILETYPE) != 0) {
        klog(KLOG_WARN, "ext2: unsupported incompat features 0x%lx",
             (uint64_t)ext2_fs.sb.s_feature_incompat);
        spin_unlock(&ext2_lock);
        return BSD_ERR(BSD_EINVAL);
    }
    if ((ext2_fs.sb.s_feature_incompat &
         EXT2_FEATURE_INCOMPAT_FILETYPE) == 0) {
        spin_unlock(&ext2_lock);
        return BSD_ERR(BSD_EINVAL);
    }
    if ((ext2_fs.sb.s_feature_ro_compat & EXT2_FEATURE_RO_COMPAT_CSUM) != 0) {
        klog(KLOG_WARN, "ext2: metadata checksums are not supported");
        spin_unlock(&ext2_lock);
        return BSD_ERR(BSD_EINVAL);
    }

    if (ext2_fs.sb.s_rev_level > 1U) {
        spin_unlock(&ext2_lock);
        return BSD_ERR(BSD_EINVAL);
    }

    ext2_fs.block_size = block_size;
    ext2_fs.sectors_per_block = block_size / BLK_SECTOR_SIZE;
    ext2_fs.inode_size = inode_size;
    ext2_fs.first_data_block = ext2_fs.sb.s_first_data_block;
    ext2_fs.blocks_per_group = ext2_fs.sb.s_blocks_per_group;
    ext2_fs.inodes_per_group = ext2_fs.sb.s_inodes_per_group;
    ext2_fs.inodes_count = ext2_fs.sb.s_inodes_count;
    ext2_fs.blocks_count = ext2_fs.sb.s_blocks_count;

    /* A group bitmap is one block; a larger geometry would make the
     * allocator index outside the bitmap buffer. */
    uint32_t bitmap_bits = block_size * 8U;

    if (ext2_fs.blocks_per_group > bitmap_bits ||
        ext2_fs.inodes_per_group > bitmap_bits) {
        spin_unlock(&ext2_lock);
        return BSD_ERR(BSD_EINVAL);
    }

    uint32_t expected_first = block_size == 1024U ? 1U : 0U;

    if (ext2_fs.first_data_block != expected_first ||
        ext2_fs.blocks_count <= ext2_fs.first_data_block) {
        spin_unlock(&ext2_lock);
        return BSD_ERR(BSD_EINVAL);
    }

    if ((uint64_t)ext2_fs.blocks_count * ext2_fs.sectors_per_block >
            dev->capacity_sectors ||
        (uint64_t)ext2_fs.inodes_count * inode_size >
            (uint64_t)ext2_fs.blocks_count * block_size) {
        spin_unlock(&ext2_lock);
        return BSD_ERR(BSD_EINVAL);
    }

    uint32_t first_ino = (ext2_fs.sb.s_rev_level == 0 ||
                          ext2_fs.sb.s_first_ino == 0)
                             ? 11U
                             : ext2_fs.sb.s_first_ino;

    if (first_ino > ext2_fs.inodes_count + 1U) {
        spin_unlock(&ext2_lock);
        return BSD_ERR(BSD_EINVAL);
    }
    ext2_fs.first_ino = first_ino;

    ext2_fs.inode_table_blocks =
        (uint32_t)(((uint64_t)ext2_fs.inodes_per_group * inode_size +
                    block_size - 1U) / block_size);

    if ((ext2_fs.sb.s_feature_compat &
         EXT2_FEATURE_COMPAT_RESIZE_INODE) != 0) {
        uint16_t reserved = 0;

        memcpy(&reserved, ext2_fs.sb_raw + EXT2_SB_RESERVED_GDT_OFFSET,
               sizeof(reserved));
        ext2_fs.reserved_gdt_blocks = reserved;
    }

    uint64_t groups_by_inodes =
        ((uint64_t)ext2_fs.inodes_count + ext2_fs.inodes_per_group - 1U) /
        ext2_fs.inodes_per_group;
    uint64_t groups_by_blocks =
        ((uint64_t)ext2_fs.blocks_count - ext2_fs.first_data_block +
         ext2_fs.blocks_per_group - 1U) /
        ext2_fs.blocks_per_group;

    if (groups_by_inodes == 0 || groups_by_inodes > EXT2_MAX_GROUPS ||
        groups_by_blocks == 0 || groups_by_blocks > EXT2_MAX_GROUPS) {
        spin_unlock(&ext2_lock);
        return BSD_ERR(BSD_EINVAL);
    }

    ext2_fs.group_count = (uint32_t)(groups_by_inodes > groups_by_blocks
                                         ? groups_by_inodes
                                         : groups_by_blocks);

    ext2_fs.group_table_block = ext2_fs.first_data_block + 1U;
    ext2_fs.group_table_blocks =
        (uint32_t)(((uint64_t)ext2_fs.group_count *
                        sizeof(ext2_group_desc_t) +
                    block_size - 1U) / block_size);

    if ((uint64_t)ext2_fs.group_table_block +
            ext2_fs.group_table_blocks +
            ext2_fs.reserved_gdt_blocks >
        ext2_fs.blocks_count) {
        spin_unlock(&ext2_lock);
        return BSD_ERR(BSD_EINVAL);
    }

    size_t table_bytes =
        (size_t)ext2_fs.group_table_blocks * block_size;
    ext2_fs.groups = (ext2_group_desc_t *)kalloc(table_bytes);

    if (ext2_fs.groups == NULL) {
        spin_unlock(&ext2_lock);
        return BSD_ERR(BSD_ENOMEM);
    }
    memset(ext2_fs.groups, 0, table_bytes);

    status = ext2_io(ext2_lba_transfer(
        &ext2_fs,
        (uint64_t)ext2_fs.group_table_block * ext2_fs.sectors_per_block,
        ext2_fs.group_table_blocks * ext2_fs.sectors_per_block,
        ext2_fs.groups, false));

    if (status != SYS_OK) {
        kfree(ext2_fs.groups);
        ext2_fs.groups = NULL;
        spin_unlock(&ext2_lock);
        return status;
    }

    for (uint32_t group = 0; group < ext2_fs.group_count; group++) {
        const ext2_group_desc_t *desc = &ext2_fs.groups[group];
        uint64_t group_first = (uint64_t)ext2_fs.first_data_block +
                               ((uint64_t)group * ext2_fs.blocks_per_group);
        uint64_t group_last = group_first + ext2_fs.blocks_per_group;

        if (group_last > ext2_fs.blocks_count) {
            group_last = ext2_fs.blocks_count;
        }

        if (desc->bg_block_bitmap == 0 || desc->bg_inode_bitmap == 0 ||
            desc->bg_inode_table == 0 ||
            desc->bg_free_blocks_count > ext2_fs.blocks_per_group ||
            desc->bg_free_inodes_count > ext2_fs.inodes_per_group) {
            kfree(ext2_fs.groups);
            ext2_fs.groups = NULL;
            spin_unlock(&ext2_lock);
            return BSD_ERR(BSD_EIO);
        }

        if ((uint64_t)desc->bg_block_bitmap + 1U > ext2_fs.blocks_count ||
            (uint64_t)desc->bg_inode_bitmap + 1U > ext2_fs.blocks_count ||
            (uint64_t)desc->bg_inode_table +
                    ext2_fs.inode_table_blocks >
                ext2_fs.blocks_count) {
            kfree(ext2_fs.groups);
            ext2_fs.groups = NULL;
            spin_unlock(&ext2_lock);
            return BSD_ERR(BSD_EIO);
        }

        if ((uint64_t)desc->bg_block_bitmap < group_first ||
            (uint64_t)desc->bg_block_bitmap >= group_last ||
            (uint64_t)desc->bg_inode_bitmap < group_first ||
            (uint64_t)desc->bg_inode_bitmap >= group_last ||
            (uint64_t)desc->bg_inode_table < group_first ||
            (uint64_t)desc->bg_inode_table +
                    ext2_fs.inode_table_blocks >
                group_last) {
            kfree(ext2_fs.groups);
            ext2_fs.groups = NULL;
            spin_unlock(&ext2_lock);
            return BSD_ERR(BSD_EIO);
        }
    }

    ext2_inode_t root_inode;
    status = ext2_read_inode(&ext2_fs, EXT2_ROOT_INO, &root_inode);

    if (status != SYS_OK ||
        (root_inode.mode & EXT2_S_IFMT) != EXT2_S_IFDIR) {
        kfree(ext2_fs.groups);
        ext2_fs.groups = NULL;
        spin_unlock(&ext2_lock);
        return status != SYS_OK ? status : BSD_ERR(BSD_EIO);
    }

    vnode_t *root = NULL;
    status = ext2_make_vnode(&ext2_fs, EXT2_ROOT_INO, NULL, "", &root);

    if (status != SYS_OK) {
        kfree(ext2_fs.groups);
        ext2_fs.groups = NULL;
        spin_unlock(&ext2_lock);
        return status;
    }

    status = vfs_set_root(root);

    if (status != SYS_OK) {
        kfree(ext2_fs.groups);
        ext2_fs.groups = NULL;
        spin_unlock(&ext2_lock);
        vfs_vnode_unref(root);
        return status;
    }

    ext2_fs.mounted = true;

    spin_unlock(&ext2_lock);

    klog(KLOG_INFO,
         "ext2: mounted at / (%lu-byte blocks, %lu inodes, %lu blocks, "
         "%lu free blocks, %lu free inodes, %lu group(s))",
         (uint64_t)ext2_fs.block_size, (uint64_t)ext2_fs.inodes_count,
         (uint64_t)ext2_fs.blocks_count,
         (uint64_t)ext2_fs.sb.s_free_blocks_count,
         (uint64_t)ext2_fs.sb.s_free_inodes_count,
         (uint64_t)ext2_fs.group_count);

    return SYS_OK;
}

bool ext2_mounted(void)
{
    return ext2_fs.mounted;
}