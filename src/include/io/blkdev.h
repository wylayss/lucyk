#ifndef LUCYK_IO_BLKDEV_H
#define LUCYK_IO_BLKDEV_H

#include <types.h>

#define BLK_SECTOR_SIZE 512U

/* Probes the PCI bus for a transitional virtio-blk function, resets it and
 * brings up one legacy split virtqueue. Returns false when no device exists. */
bool blk_init(void);
bool blk_present(void);
uint64_t blk_capacity_sectors(void);
uint32_t blk_irq_event_count(void);

/* Serialized sector transfers. The buffer is an ordinary kernel pointer; the
 * driver stages it through a physically contiguous DMA bounce buffer. */
sys_status_t blk_read_sectors(uint64_t lba, uint32_t count, void *buffer);
sys_status_t blk_write_sectors(uint64_t lba, uint32_t count,
                               const void *buffer);

/* Filesystem-facing block device. The backing driver only requires that a
 * single request stays within BLK_MAX_REQUEST_SECTORS sectors. */
#define BLK_MAX_REQUEST_SECTORS 128U

typedef struct blkdev blkdev_t;

struct blkdev {
    uint64_t capacity_sectors;
    sys_status_t (*read)(blkdev_t *dev, uint64_t lba, uint32_t count,
                         void *buffer);
    sys_status_t (*write)(blkdev_t *dev, uint64_t lba, uint32_t count,
                          const void *buffer);
};

/* Returns the primary block device, or NULL when no disk is attached. */
blkdev_t *blkdev_primary(void);

#endif /* LUCYK_IO_BLKDEV_H */