#include <idt.h>
#include <io.h>
#include <io/blkdev.h>
#include <io/pci.h>
#include <ioapic.h>
#include <isr.h>
#include <klog.h>
#include <lapic.h>
#include <pmm.h>
#include <string.h>
#include <types.h>

#define VIRTIO_PCI_VENDOR           0x1AF4U
#define VIRTIO_PCI_DEVICE_BLK       0x1001U

#define VIRTIO_PCI_HOST_FEATURES    0x00U
#define VIRTIO_PCI_GUEST_FEATURES   0x04U
#define VIRTIO_PCI_QUEUE_ADDR       0x08U
#define VIRTIO_PCI_QUEUE_SIZE       0x0CU
#define VIRTIO_PCI_QUEUE_SELECT     0x0EU
#define VIRTIO_PCI_QUEUE_NOTIFY     0x10U
#define VIRTIO_PCI_STATUS           0x12U
#define VIRTIO_PCI_ISR              0x13U
#define VIRTIO_PCI_CONFIG           0x14U

#define VIRTIO_STATUS_ACKNOWLEDGE   0x01U
#define VIRTIO_STATUS_DRIVER        0x02U
#define VIRTIO_STATUS_DRIVER_OK     0x04U
#define VIRTIO_STATUS_FAILED        0x80U

#define VIRTIO_BLK_T_IN             0x00U
#define VIRTIO_BLK_T_OUT            0x01U

#define VIRTIO_BLK_S_OK             0x00U

#define VIRTQ_DESC_F_NEXT           0x0001U
#define VIRTQ_DESC_F_WRITE          0x0002U

#define VIRTIO_BLK_IRQ_VECTOR       0x40U
#define VIRTIO_BLK_MAX_SECTORS      128U

#define VIRTIO_BLK_IRQ_SPINS        5000000ULL
#define VIRTIO_BLK_POLL_SPINS       200000000ULL
#define VIRTIO_BLK_RESET_SPINS      1000000ULL

typedef struct PACKED virtq_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} virtq_desc_t;

typedef struct PACKED virtq_avail {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[];
} virtq_avail_t;

typedef struct PACKED virtq_used_elem {
    uint32_t id;
    uint32_t len;
} virtq_used_elem_t;

typedef struct PACKED virtq_used {
    uint16_t flags;
    uint16_t idx;
    virtq_used_elem_t ring[];
} virtq_used_t;

typedef struct PACKED virtio_blk_hdr {
    uint32_t type;
    uint32_t reserved;
    uint64_t sector;
} virtio_blk_hdr_t;

typedef struct virtio_queue {
    uint16_t      size;
    virtq_desc_t *desc;
    virtq_avail_t *avail;
    virtq_used_t *used;
    uint16_t      last_used;
} virtio_queue_t;

static struct {
    bool          initialized;
    bool          present;
    bool          failed;
    bool          irq_routed;
    uint16_t      io_base;
    uint8_t       vector;
    uint8_t       irq_line;
    uint8_t       irq_pin;
    uint8_t       gsi;
    uint64_t      capacity;
    uint64_t      ring_phys;
    paddr_t       ctl_phys;
    paddr_t       bounce_phys;
    virtio_blk_hdr_t *hdr;
    uint8_t      *status;
    uint8_t      *bounce;
    virtio_queue_t queue;
} blk;

static bool blk_busy;
static volatile uint32_t blk_irq_events;

static uint8_t vp_read8(uint16_t offset)
{
    return inb((uint16_t)(blk.io_base + offset));
}

static uint16_t vp_read16(uint16_t offset)
{
    return inw((uint16_t)(blk.io_base + offset));
}

static uint32_t vp_read32(uint16_t offset)
{
    return inl((uint16_t)(blk.io_base + offset));
}

static void vp_write8(uint16_t offset, uint8_t value)
{
    outb((uint16_t)(blk.io_base + offset), value);
}

static void vp_write16(uint16_t offset, uint16_t value)
{
    outw((uint16_t)(blk.io_base + offset), value);
}

static void vp_write32(uint16_t offset, uint32_t value)
{
    outl((uint16_t)(blk.io_base + offset), value);
}

static void virtio_blk_irq(interrupt_frame_t *frame)
{
    UNUSED(frame);

    (void)vp_read8(VIRTIO_PCI_ISR);
    __atomic_fetch_add(&blk_irq_events, 1U, __ATOMIC_RELAXED);
}

static bool blk_wait_reset(void)
{
    for (uint64_t spins = 0; spins < VIRTIO_BLK_RESET_SPINS; spins++) {
        if (vp_read8(VIRTIO_PCI_STATUS) == 0) {
            return true;
        }
        __builtin_ia32_pause();
    }

    return false;
}

static bool blk_queue_layout(uint16_t qsize, size_t *used_offset,
                             size_t *total_size)
{
    uint64_t desc_bytes = (uint64_t)qsize * sizeof(virtq_desc_t);
    uint64_t avail_bytes = 4ULL + ((uint64_t)qsize * 2ULL) + 2ULL;
    uint64_t used = ALIGN_UP(desc_bytes + avail_bytes, PAGE_SIZE);
    uint64_t total = used + 4ULL + ((uint64_t)qsize * 8ULL) + 2ULL;

    if (used > SIZE_MAX || total > SIZE_MAX) {
        return false;
    }

    *used_offset = (size_t)used;
    *total_size = (size_t)total;
    return true;
}

static void blk_acquire(void)
{
    while (__atomic_test_and_set(&blk_busy, __ATOMIC_ACQUIRE)) {
        __builtin_ia32_pause();
    }
}

static void blk_release(void)
{
    __atomic_clear(&blk_busy, __ATOMIC_RELEASE);
}

bool blk_init(void)
{
    if (blk.initialized) {
        return blk.present;
    }
    blk.initialized = true;
    blk.failed = false;
    blk_busy = false;
    blk_irq_events = 0;

    pci_device_t *dev =
        pci_find_device(VIRTIO_PCI_VENDOR, VIRTIO_PCI_DEVICE_BLK);

    if (dev == NULL) {
        klog(KLOG_INFO, "virtio-blk: no transitional PCI function found");
        return false;
    }

    if (!dev->bar_io[0] || dev->bar[0] == 0 || dev->bar[0] > 0xFFFFULL) {
        klog(KLOG_WARN, "virtio-blk: BAR0 is not a usable I/O window");
        return false;
    }

    blk.io_base = (uint16_t)dev->bar[0];
    blk.irq_line = dev->interrupt_line;
    blk.irq_pin = dev->interrupt_pin;

    pci_enable_bus_master(dev);

    vp_write8(VIRTIO_PCI_STATUS, 0);
    if (!blk_wait_reset()) {
        klog(KLOG_WARN, "virtio-blk: device did not reset");
        return false;
    }

    vp_write8(VIRTIO_PCI_STATUS, VIRTIO_STATUS_ACKNOWLEDGE);
    vp_write8(VIRTIO_PCI_STATUS,
              VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);

    uint32_t host_features = vp_read32(VIRTIO_PCI_HOST_FEATURES);
    UNUSED(host_features);
    vp_write32(VIRTIO_PCI_GUEST_FEATURES, 0);

    vp_write16(VIRTIO_PCI_QUEUE_SELECT, 0);
    uint16_t qsize = vp_read16(VIRTIO_PCI_QUEUE_SIZE);

    if (qsize < 2 || qsize > 1024) {
        klog(KLOG_WARN, "virtio-blk: invalid queue size %u", (uint64_t)qsize);
        vp_write8(VIRTIO_PCI_STATUS, VIRTIO_STATUS_FAILED);
        return false;
    }

    size_t used_offset = 0;
    size_t total_size = 0;

    if (!blk_queue_layout(qsize, &used_offset, &total_size)) {
        vp_write8(VIRTIO_PCI_STATUS, VIRTIO_STATUS_FAILED);
        return false;
    }

    size_t ring_pages = (total_size + (size_t)PAGE_SIZE - 1U) / (size_t)PAGE_SIZE;
    uint64_t ring_phys = pmm_alloc_frames(ring_pages);

    if (ring_phys == 0) {
        klog(KLOG_WARN, "virtio-blk: cannot allocate virtqueue pages");
        vp_write8(VIRTIO_PCI_STATUS, VIRTIO_STATUS_FAILED);
        return false;
    }

    blk.ring_phys = ring_phys;
    blk.queue.size = qsize;
    blk.queue.last_used = 0;

    uint8_t *ring = (uint8_t *)pmm_phys_to_virt(ring_phys);
    blk.queue.desc = (virtq_desc_t *)ring;
    blk.queue.avail =
        (virtq_avail_t *)(ring + ((size_t)qsize * sizeof(virtq_desc_t)));
    blk.queue.used = (virtq_used_t *)(ring + used_offset);

    if ((ring_phys >> 12) > UINT32_MAX) {
        klog(KLOG_WARN,
             "virtio-blk: virtqueue is above the legacy 32-bit PFN range");
        pmm_free_frames(ring_phys, ring_pages);
        vp_write8(VIRTIO_PCI_STATUS, VIRTIO_STATUS_FAILED);
        return false;
    }

    vp_write32(VIRTIO_PCI_QUEUE_ADDR, (uint32_t)(ring_phys >> 12));

    blk.ctl_phys = pmm_alloc_frames(1);
    if (blk.ctl_phys == 0) {
        klog(KLOG_WARN, "virtio-blk: cannot allocate request control page");
        pmm_free_frames(ring_phys, ring_pages);
        vp_write8(VIRTIO_PCI_STATUS, VIRTIO_STATUS_FAILED);
        return false;
    }

    blk.hdr = (virtio_blk_hdr_t *)pmm_phys_to_virt(blk.ctl_phys);
    blk.status = (uint8_t *)blk.hdr + sizeof(virtio_blk_hdr_t);

    size_t bounce_bytes = (size_t)VIRTIO_BLK_MAX_SECTORS * BLK_SECTOR_SIZE;
    size_t bounce_pages = (bounce_bytes + (size_t)PAGE_SIZE - 1U) /
                          (size_t)PAGE_SIZE;

    blk.bounce_phys = pmm_alloc_frames(bounce_pages);
    if (blk.bounce_phys == 0) {
        klog(KLOG_WARN, "virtio-blk: cannot allocate DMA bounce buffer");
        pmm_free_frame(blk.ctl_phys);
        pmm_free_frames(ring_phys, ring_pages);
        vp_write8(VIRTIO_PCI_STATUS, VIRTIO_STATUS_FAILED);
        return false;
    }

    blk.bounce = (uint8_t *)pmm_phys_to_virt(blk.bounce_phys);

    uint64_t capacity_low = vp_read32(VIRTIO_PCI_CONFIG);
    uint64_t capacity_high = vp_read32(VIRTIO_PCI_CONFIG + 4U);

    blk.capacity = (capacity_high << 32) | capacity_low;

    blk.vector = VIRTIO_BLK_IRQ_VECTOR;
    idt_register_handler(blk.vector, virtio_blk_irq);

    if (blk.irq_pin != 0) {
        bool active_low = false;
        bool level_triggered = false;
        uint8_t gsi = 0;

        if (!ioapic_resolve_irq(blk.irq_line, &gsi, &active_low,
                                &level_triggered)) {
            klog(KLOG_WARN,
                 "virtio-blk: cannot resolve irq line %u",
                 (uint64_t)blk.irq_line);
        } else {
            blk.gsi = gsi;

            if (ioapic_route_irq(gsi, blk.vector, lapic_id(), active_low,
                                 level_triggered) == SYS_OK) {
                blk.irq_routed = true;
            } else {
                klog(KLOG_WARN,
                     "virtio-blk: cannot route gsi %u (irq line %u)",
                     (uint64_t)gsi, (uint64_t)blk.irq_line);
            }
        }
    }

    (void)vp_read8(VIRTIO_PCI_ISR);

    vp_write8(VIRTIO_PCI_STATUS, VIRTIO_STATUS_ACKNOWLEDGE |
                                     VIRTIO_STATUS_DRIVER |
                                     VIRTIO_STATUS_DRIVER_OK);

    blk.present = true;

    klog(KLOG_INFO,
         "virtio-blk: capacity=%lu sectors, queue=%u, irq line=%u pin=%u "
         "gsi=%u vector=0x%lx routed=%u",
         blk.capacity, (uint64_t)blk.queue.size, (uint64_t)blk.irq_line,
         (uint64_t)blk.irq_pin, (uint64_t)blk.gsi, (uint64_t)blk.vector,
         (uint64_t)(blk.irq_routed ? 1U : 0U));

    return true;
}

bool blk_present(void)
{
    return blk.present;
}

uint64_t blk_capacity_sectors(void)
{
    return blk.capacity;
}

uint32_t blk_irq_event_count(void)
{
    return __atomic_load_n(&blk_irq_events, __ATOMIC_ACQUIRE);
}

static sys_status_t blk_wait_completion(uint16_t previous_used)
{
    volatile virtq_used_t *used = (volatile virtq_used_t *)blk.queue.used;
    uint32_t irq_base = __atomic_load_n(&blk_irq_events, __ATOMIC_ACQUIRE);
    uint64_t spins = 0;

    /* First wait for the device interrupt, which is the normal completion
     * path. If it never arrives (masked vector, polling context or a lost
     * edge), fall back to a longer bounded poll of the used ring. */
    while (spins < VIRTIO_BLK_IRQ_SPINS) {
        __atomic_thread_fence(__ATOMIC_SEQ_CST);

        if (used->idx != previous_used) {
            blk.queue.last_used = used->idx;
            return SYS_OK;
        }
        if (__atomic_load_n(&blk_irq_events, __ATOMIC_ACQUIRE) != irq_base) {
            __atomic_thread_fence(__ATOMIC_SEQ_CST);
            if (used->idx != previous_used) {
                blk.queue.last_used = used->idx;
                return SYS_OK;
            }
        }

        spins++;
        __builtin_ia32_pause();
    }

    while (spins < VIRTIO_BLK_POLL_SPINS) {
        __atomic_thread_fence(__ATOMIC_SEQ_CST);

        if (used->idx != previous_used) {
            blk.queue.last_used = used->idx;
            return SYS_OK;
        }

        spins++;
        __builtin_ia32_pause();
    }

    return SYS_ERR_DEAD;
}

static sys_status_t blk_transfer(uint64_t lba, uint32_t count,
                                 const void *write_buffer, void *read_buffer)
{
    if (!blk.present || count == 0 || count > VIRTIO_BLK_MAX_SECTORS ||
        lba >= blk.capacity || count > blk.capacity - lba) {
        return SYS_ERR_INVAL;
    }

    if (write_buffer == NULL && read_buffer == NULL) {
        return SYS_ERR_INVAL;
    }
    if (blk.failed) {
        return SYS_ERR_DEAD;
    }

    bool is_read = read_buffer != NULL;
    size_t bytes = (size_t)count * BLK_SECTOR_SIZE;

    blk_acquire();

    if (is_read) {
        memset(read_buffer, 0, bytes);
    } else {
        memcpy(blk.bounce, write_buffer, bytes);
    }

    blk.hdr->type = is_read ? VIRTIO_BLK_T_IN : VIRTIO_BLK_T_OUT;
    blk.hdr->reserved = 0;
    blk.hdr->sector = lba;
    *blk.status = 0xFF;

    blk.queue.desc[0].addr = blk.ctl_phys;
    blk.queue.desc[0].len = (uint32_t)sizeof(virtio_blk_hdr_t);
    blk.queue.desc[0].flags = VIRTQ_DESC_F_NEXT;
    blk.queue.desc[0].next = 1;

    blk.queue.desc[1].addr = blk.bounce_phys;
    blk.queue.desc[1].len = (uint32_t)bytes;
    blk.queue.desc[1].flags =
        (uint16_t)(VIRTQ_DESC_F_NEXT |
                   (is_read ? VIRTQ_DESC_F_WRITE : 0U));
    blk.queue.desc[1].next = 2;

    blk.queue.desc[2].addr = blk.ctl_phys + sizeof(virtio_blk_hdr_t);
    blk.queue.desc[2].len = 1;
    blk.queue.desc[2].flags = VIRTQ_DESC_F_WRITE;
    blk.queue.desc[2].next = 0;

    volatile virtq_avail_t *avail =
        (volatile virtq_avail_t *)blk.queue.avail;
    uint16_t head = avail->idx;

    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    avail->ring[head % blk.queue.size] = 0;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    avail->idx = (uint16_t)(head + 1U);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);

    vp_write16(VIRTIO_PCI_QUEUE_NOTIFY, 0);

    uint16_t previous_used = blk.queue.last_used;
    sys_status_t status = blk_wait_completion(previous_used);

    if (status == SYS_OK) {
        volatile virtq_used_t *used =
            (volatile virtq_used_t *)blk.queue.used;
        uint16_t completed = previous_used % blk.queue.size;

        __atomic_thread_fence(__ATOMIC_SEQ_CST);

        if (used->ring[completed].id != 0) {
            status = SYS_ERR_DEAD;
        } else if (*blk.status != VIRTIO_BLK_S_OK) {
            status = SYS_ERR_DEAD;
        } else if (is_read) {
            memcpy(read_buffer, blk.bounce, bytes);
        }
    }

    if (status != SYS_OK) {
        /* A timed-out request may still be owned by the device; reusing the
         * descriptors would corrupt unrelated memory. Latch the failure. */
        blk.failed = true;
    }

    blk_release();
    return status;
}

sys_status_t blk_read_sectors(uint64_t lba, uint32_t count, void *buffer)
{
    if (buffer == NULL) {
        return SYS_ERR_INVAL;
    }

    return blk_transfer(lba, count, NULL, buffer);
}

sys_status_t blk_write_sectors(uint64_t lba, uint32_t count,
                               const void *buffer)
{
    if (buffer == NULL) {
        return SYS_ERR_INVAL;
    }

    return blk_transfer(lba, count, buffer, NULL);
}

static sys_status_t blkdev_read(blkdev_t *dev, uint64_t lba, uint32_t count,
                                void *buffer)
{
    UNUSED(dev);
    return blk_read_sectors(lba, count, buffer);
}

static sys_status_t blkdev_write(blkdev_t *dev, uint64_t lba, uint32_t count,
                                 const void *buffer)
{
    UNUSED(dev);
    return blk_write_sectors(lba, count, buffer);
}

static blkdev_t virtio_blkdev = {
    .capacity_sectors = 0,
    .read = blkdev_read,
    .write = blkdev_write,
};

blkdev_t *blkdev_primary(void)
{
    if (!blk.present) {
        return NULL;
    }

    virtio_blkdev.capacity_sectors = blk.capacity;
    return &virtio_blkdev;
}