#include <io/ahci.h>

#include <io/blkdev.h>
#include <io/pci.h>
#include <klog.h>
#include <pmm.h>
#include <string.h>
#include <types.h>
#include <vspace.h>

#define AHCI_MMIO_VA_BASE 0xFFFF900000200000ULL
#define AHCI_MMIO_STRIDE 0x10000ULL

#define PCI_CLASS_MASS_STORAGE 0x01U
#define PCI_SUBCLASS_SATA      0x06U
#define PCI_PROGIF_AHCI        0x01U

#define PCI_REG_COMMAND 0x04U
#define PCI_CMD_MEMORY   (1U << 1)
#define PCI_CMD_BUS_MASTER (1U << 2)

#define HBA_CAP   0x00U
#define HBA_GHC   0x04U
#define HBA_PI    0x0CU
#define HBA_BOHC  0x28U

#define GHC_HR (1U << 0)
#define GHC_AE (1U << 31)

#define CAP_SO (1U << 18)

#define BOHC_BOS (1U << 0)
#define BOHC_OOS (1U << 1)
#define BOHC_BB  (1U << 4)

#define AHCI_MAX_PORTS 32U

#define PORT_CLB  0x00U
#define PORT_CLBU 0x04U
#define PORT_FB   0x08U
#define PORT_FBU  0x0CU
#define PORT_IS   0x10U
#define PORT_IE   0x14U
#define PORT_CMD  0x18U
#define PORT_TFD  0x20U
#define PORT_SIG  0x24U
#define PORT_SSTS 0x28U
#define PORT_SERR 0x30U
#define PORT_CI   0x38U

#define PORT_CMD_ST  (1U << 0)
#define PORT_CMD_FRE (1U << 4)
#define PORT_CMD_FR  (1U << 14)
#define PORT_CMD_CR  (1U << 15)

#define TFD_BSY 0x80U
#define TFD_DRQ 0x08U
#define TFD_ERR 0x01U

#define SSTS_DET_MASK 0x0FU
#define SSTS_DET_PRESENT 3U
#define SATA_SIG_ATA 0x00000101U

#define FIS_TYPE_REG_H2D 0x27U

#define ATA_CMD_IDENTIFY 0xECU
#define ATA_CMD_READ_DMA_EXT 0x25U
#define ATA_CMD_WRITE_DMA_EXT 0x35U

#define CMD_TABLE_ALIGN 128U
#define CMD_LIST_ALIGN 1024U
#define FIS_ALIGN 256U

#define AHCI_TIMEOUT_MS 2000U
#define AHCI_POLL_BOUND 10000000ULL

#define AHCI_SECTOR 512U
#define AHCI_MAX_TRANSFER_SECTORS 128U

struct cmd_header {
    uint16_t opts;     /* CFL + W + A + P */
    uint16_t prdtl;
    uint32_t prdbc;
    uint32_t ctba;
    uint32_t ctbau;
    uint32_t rsv[4];
} __attribute__((packed));

struct prdt_entry {
    uint32_t dba;
    uint32_t dbau;
    uint32_t rsv;
    uint32_t dbc;      /* byte count - 1 */
} __attribute__((packed));

struct fis_reg_h2d {
    uint8_t fis_type;
    uint8_t flags;
    uint8_t command;
    uint8_t featurel;
    uint8_t lba0, lba1, lba2;
    uint8_t device;
    uint8_t lba3, lba4, lba5;
    uint8_t featureh;
    uint8_t countl, counth;
    uint8_t icc;
    uint8_t control;
    uint8_t rsv[4];
} __attribute__((packed));

struct ahci_port_state {
    bool active;
    uint8_t port;
    uint64_t sectors;
    struct cmd_header *cl;
    uint8_t *fis;
    uint8_t *cmd_table;     /* +128-byte aligned; 1 PRDT */
    paddr_t cl_phys, fis_phys, cmd_table_phys;
    uint8_t *dma;           /* bounce buffer, contiguous, 1 MiB max */
    paddr_t dma_phys;
    uint32_t dma_pages;
};

static volatile uint32_t *hba; /* ABAR */
static bool ahci_present_flag;
static uint64_t ahci_mmio_va;
static struct ahci_port_state ports[AHCI_MAX_PORTS];
static blkdev_t ahci_blkdev;

static uint32_t hba_read(uint32_t off)
{
    return hba[off / 4U];
}

static void hba_write(uint32_t off, uint32_t value)
{
    hba[off / 4U] = value;
}

static volatile uint32_t *port_regs(uint8_t port)
{
    return (volatile uint32_t *)((uint8_t *)hba + 0x100U + (uint32_t)port * 0x80U);
}

static uint32_t port_read(uint8_t port, uint32_t off)
{
    return port_regs(port)[off / 4U];
}

static void port_write(uint8_t port, uint32_t off, uint32_t value)
{
    port_regs(port)[off / 4U] = value;
}

static bool port_wait_not_busy(uint8_t port)
{
    for (uint64_t i = 0; i < AHCI_POLL_BOUND; i++) {
        uint32_t tfd = port_read(port, PORT_TFD);
        if ((tfd & (TFD_BSY | TFD_DRQ)) == 0) {
            return true;
        }
    }
    return false;
}

static void port_stop(uint8_t port)
{
    uint32_t cmd = port_read(port, PORT_CMD);
    cmd &= ~(PORT_CMD_ST | PORT_CMD_FRE);
    port_write(port, PORT_CMD, cmd);

    for (uint64_t i = 0; i < AHCI_POLL_BOUND; i++) {
        if ((port_read(port, PORT_CMD) & (PORT_CMD_CR | PORT_CMD_FR)) == 0) {
            break;
        }
    }
}

static void port_start(uint8_t port)
{
    while ((port_read(port, PORT_CMD) & PORT_CMD_CR) != 0) { }

    uint32_t cmd = port_read(port, PORT_CMD);
    cmd |= PORT_CMD_FRE;
    port_write(port, PORT_CMD, cmd);
    cmd |= PORT_CMD_ST;
    port_write(port, PORT_CMD, cmd);
}

static bool ahci_submit(struct ahci_port_state *p, uint8_t command,
                        uint64_t lba, uint16_t count, uint8_t *buf,
                        paddr_t buf_phys, uint32_t bytes, bool write)
{
    uint8_t port = p->port;

    if (!port_wait_not_busy(port)) {
        return false;
    }

    struct cmd_header *hdr = &p->cl[0];
    memset(hdr, 0, sizeof(*hdr));

    uint16_t opts = 5U; /* CFL = 5 dwords (H2D FIS) */
    if (write) {
        opts |= 0x40U; /* W */
    }
    hdr->opts = opts;
    hdr->prdtl = 1U;
    hdr->prdbc = 0;
    hdr->ctba = (uint32_t)(p->cmd_table_phys & 0xFFFFFFFFULL);
    hdr->ctbau = (uint32_t)(p->cmd_table_phys >> 32);

    uint8_t *table = p->cmd_table;
    memset(table, 0, CMD_TABLE_ALIGN + sizeof(struct prdt_entry));

    struct fis_reg_h2d *fis = (struct fis_reg_h2d *)table;
    fis->fis_type = FIS_TYPE_REG_H2D;
    fis->flags = 0x80U; /* C */
    fis->command = command;
    fis->device = 0x40U; /* LBA mode */

    fis->lba0 = (uint8_t)(lba & 0xFFU);
    fis->lba1 = (uint8_t)((lba >> 8) & 0xFFU);
    fis->lba2 = (uint8_t)((lba >> 16) & 0xFFU);
    fis->lba3 = (uint8_t)((lba >> 24) & 0xFFU);
    fis->lba4 = (uint8_t)((lba >> 32) & 0xFFU);
    fis->lba5 = (uint8_t)((lba >> 40) & 0xFFU);

    fis->countl = (uint8_t)(count & 0xFFU);
    fis->counth = (uint8_t)((count >> 8) & 0xFFU);

    struct prdt_entry *prdt = (struct prdt_entry *)(table + CMD_TABLE_ALIGN);
    prdt->dba = (uint32_t)(buf_phys & 0xFFFFFFFFULL);
    prdt->dbau = (uint32_t)(buf_phys >> 32);
    prdt->rsv = 0;
    prdt->dbc = bytes - 1U;

    (void)buf;

    /* Clear interrupt status, issue slot 0. */
    port_write(port, PORT_IS, 0xFFFFFFFFU);
    port_write(port, PORT_SERR, 0xFFFFFFFFU);
    __asm__ volatile("mfence" ::: "memory");
    port_write(port, PORT_CI, 1U);

    for (uint64_t i = 0; i < AHCI_POLL_BOUND; i++) {
        uint32_t ci = port_read(port, PORT_CI);
        uint32_t is = port_read(port, PORT_IS);
        if ((ci & 1U) == 0) {
            if ((is & (1U << 30)) != 0 || (port_read(port, PORT_TFD) & TFD_ERR) != 0) {
                return false;
            }
            return true;
        }
    }

    return false;
}

static bool port_read_sectors(struct ahci_port_state *p, uint64_t lba,
                              uint32_t count, void *buffer)
{
    if (count == 0 || count > AHCI_MAX_TRANSFER_SECTORS) {
        return false;
    }

    uint32_t bytes = count * AHCI_SECTOR;
    uint8_t *dst = (uint8_t *)buffer;

    /* Use contiguous DMA; bounce through p->dma for arbitrary callers. */
    uint32_t chunk_sectors = count;
    uint64_t dma_bytes = (uint64_t)chunk_sectors * AHCI_SECTOR;

    if (p->dma == NULL || (uint64_t)p->dma_pages * 4096ULL < dma_bytes) {
        return false;
    }

    paddr_t phys = p->dma_phys;
    uint8_t *dma = p->dma;
    memset(dma, 0, p->dma_pages * 4096U);

    if (!ahci_submit(p, ATA_CMD_READ_DMA_EXT, lba, (uint16_t)chunk_sectors,
                     dma, phys, bytes, false)) {
        return false;
    }

    memcpy(dst, dma, bytes);
    return true;
}

static bool port_write_sectors(struct ahci_port_state *p, uint64_t lba,
                               uint32_t count, const void *buffer)
{
    if (count == 0 || count > AHCI_MAX_TRANSFER_SECTORS) {
        return false;
    }

    uint32_t bytes = count * AHCI_SECTOR;
    const uint8_t *src = (const uint8_t *)buffer;

    if (p->dma == NULL || (uint64_t)p->dma_pages * 4096ULL < bytes) {
        return false;
    }

    memcpy(p->dma, src, bytes);
    __asm__ volatile("mfence" ::: "memory");

    if (!ahci_submit(p, ATA_CMD_WRITE_DMA_EXT, lba, (uint16_t)count,
                     p->dma, p->dma_phys, bytes, true)) {
        return false;
    }

    return true;
}

static sys_status_t ahci_blk_read(blkdev_t *dev, uint64_t lba, uint32_t count,
                                  void *buffer)
{
    (void)dev;

    for (uint32_t i = 0; i < AHCI_MAX_PORTS; i++) {
        if (ports[i].active) {
            return port_read_sectors(&ports[i], lba, count, buffer)
                       ? SYS_OK
                       : SYS_ERR_BUSY;
        }
    }
    return SYS_ERR_DEAD;
}

static sys_status_t ahci_blk_write(blkdev_t *dev, uint64_t lba, uint32_t count,
                                   const void *buffer)
{
    (void)dev;
    for (uint32_t i = 0; i < AHCI_MAX_PORTS; i++) {
        if (ports[i].active) {
            return port_write_sectors(&ports[i], lba, count, buffer)
                       ? SYS_OK
                       : SYS_ERR_BUSY;
        }
    }
    return SYS_ERR_DEAD;
}

static bool port_setup_dma(struct ahci_port_state *p)
{
    /* Command list 1 KiB aligned, FIS 256 aligned, cmd table 128 aligned. */
    paddr_t cl = pmm_alloc_contiguous(1);
    paddr_t fis = pmm_alloc_contiguous(1);
    paddr_t ct = pmm_alloc_contiguous(1);

    if (cl == 0 || fis == 0 || ct == 0) {
        return false;
    }

    /* pmm pages are 4 KiB aligned which satisfies all alignments. */
    p->cl = (struct cmd_header *)pmm_phys_to_virt(cl);
    p->fis = (uint8_t *)pmm_phys_to_virt(fis);
    p->cmd_table = (uint8_t *)pmm_phys_to_virt(ct);
    p->cl_phys = cl;
    p->fis_phys = fis;
    p->cmd_table_phys = ct;

    /* 32 * 32-byte headers = 1 KiB command list. */
    memset(p->cl, 0, 1024U);
    memset(p->fis, 0, 256U);
    memset(p->cmd_table, 0, 4096U);

    p->dma_pages = (AHCI_MAX_TRANSFER_SECTORS * AHCI_SECTOR + 4095U) / 4096U;
    paddr_t dma = pmm_alloc_contiguous(p->dma_pages);
    if (dma == 0) {
        return false;
    }
    p->dma_phys = dma;
    p->dma = (uint8_t *)pmm_phys_to_virt(dma);

    port_stop(p->port);

    port_write(p->port, PORT_CLB, (uint32_t)(cl & 0xFFFFFFFFULL));
    port_write(p->port, PORT_CLBU, (uint32_t)(cl >> 32));
    port_write(p->port, PORT_FB, (uint32_t)(fis & 0xFFFFFFFFULL));
    port_write(p->port, PORT_FBU, (uint32_t)(fis >> 32));

    port_write(p->port, PORT_IS, 0xFFFFFFFFU);
    port_write(p->port, PORT_SERR, 0xFFFFFFFFU);
    port_write(p->port, PORT_IE, 0U); /* polling */

    port_start(p->port);
    return true;
}

static bool ahci_identify(struct ahci_port_state *p)
{
    if (!port_wait_not_busy(p->port)) {
        return false;
    }

    uint32_t bytes = 512U;
    paddr_t phys = p->dma_phys;
    uint8_t *buf = p->dma;
    memset(buf, 0, 4096U);

    if (!ahci_submit(p, ATA_CMD_IDENTIFY, 0, 1, buf, phys, bytes, false)) {
        return false;
    }

    /* Words 100..103 (48-bit LBA capacity). */
    uint16_t *w = (uint16_t *)buf;
    uint64_t sectors =
        ((uint64_t)w[103] << 48) | ((uint64_t)w[102] << 32) |
        ((uint64_t)w[101] << 16) | (uint64_t)w[100];
    if (sectors == 0) {
        sectors = ((uint64_t)w[61] << 16) | (uint64_t)w[60];
    }
    p->sectors = sectors;
    return sectors != 0;
}

static bool ahci_map_abar(paddr_t phys)
{
    paddr_t aligned = phys & ~0xFFFULL;
    vaddr_t window = AHCI_MMIO_VA_BASE;

    uint64_t flags = VSPACE_PROT_WRITE | VSPACE_PROT_DEVICE |
                     VSPACE_PROT_PCD | VSPACE_PROT_PWT;

    if (vspace_map_kernel(window, aligned, flags) != SYS_OK) {
        return false;
    }

    ahci_mmio_va = window + (phys & 0xFFFULL);
    hba = (volatile uint32_t *)(uintptr_t)ahci_mmio_va;
    return true;
}

bool ahci_init(void)
{
    memset(ports, 0, sizeof(ports));
    ahci_present_flag = false;

    size_t n = pci_device_count();
    pci_device_t dev;

    for (size_t i = 0; i < n; i++) {
        if (!pci_device_snapshot(i, &dev)) {
            continue;
        }
        if (dev.class_code != PCI_CLASS_MASS_STORAGE ||
            dev.subclass != PCI_SUBCLASS_SATA ||
            dev.prog_if != PCI_PROGIF_AHCI) {
            continue;
        }

        /* Enable PCI memory space + bus master. */
        uint16_t cmd = pci_cfg_read16(dev.bus, dev.slot, dev.func,
                                      PCI_REG_COMMAND);
        cmd = (uint16_t)(cmd | PCI_CMD_MEMORY | PCI_CMD_BUS_MASTER);
        pci_cfg_write16(dev.bus, dev.slot, dev.func, PCI_REG_COMMAND, cmd);
        pci_enable_bus_master(&dev);
        /* Kernel owns this storage controller; block Ring 3 SYS_PCI_*. */
        pci_kernel_claim(dev.bus, dev.slot, dev.func);

        /* BAR5 = ABAR. */
        if (dev.bar_io[5] || dev.bar[5] == 0) {
            klog(KLOG_WARN, "ahci: no ABAR on %u:%u.%u", dev.bus, dev.slot,
                 dev.func);
            continue;
        }

        if (!ahci_map_abar(dev.bar[5])) {
            klog(KLOG_WARN, "ahci: cannot map ABAR");
            continue;
        }

        /* BIOS/OS ownership handoff. */
        uint32_t cap = hba_read(HBA_CAP);
        if ((cap & CAP_SO) != 0) {
            uint32_t bohc = hba_read(HBA_BOHC);
            bohc |= BOHC_OOS;
            hba_write(HBA_BOHC, bohc);
            for (uint64_t t = 0; t < AHCI_POLL_BOUND; t++) {
                if ((hba_read(HBA_BOHC) & BOHC_BB) == 0) {
                    break;
                }
            }
            hba_write(HBA_BOHC, hba_read(HBA_BOHC) | BOHC_OOS);
            hba_write(HBA_BOHC, hba_read(HBA_BOHC) & ~BOHC_BOS);
        }

        /* Enable AHCI. */
        hba_write(HBA_GHC, hba_read(HBA_GHC) | GHC_AE);

        uint32_t pi = hba_read(HBA_PI);
        for (uint32_t port = 0; port < AHCI_MAX_PORTS; port++) {
            if ((pi & (1U << port)) == 0) {
                continue;
            }

            uint32_t ssts = port_read((uint8_t)port, PORT_SSTS);
            if ((ssts & SSTS_DET_MASK) != SSTS_DET_PRESENT) {
                continue;
            }
            if (port_read((uint8_t)port, PORT_SIG) != SATA_SIG_ATA) {
                continue;
            }

            struct ahci_port_state *p = &ports[port];
            memset(p, 0, sizeof(*p));
            p->port = (uint8_t)port;

            if (!port_setup_dma(p)) {
                continue;
            }
            if (!ahci_identify(p)) {
                continue;
            }

            p->active = true;
            ahci_present_flag = true;

            klog(KLOG_INFO,
                 "ahci: port %u SATA %lu sectors (%lu MiB) on %u:%u.%u",
                 port, (unsigned long)p->sectors,
                 (unsigned long)(p->sectors / 2048UL), dev.bus, dev.slot,
                 dev.func);

            /* Bounce `ahci_blkdev` selection uses the active port. */
            break;
        }

        if (ahci_present_flag) {
            break;
        }
    }

    if (ahci_present_flag) {
        ahci_blkdev.capacity_sectors = 0;
        ahci_blkdev.read = ahci_blk_read;
        ahci_blkdev.write = ahci_blk_write;
        for (uint32_t i = 0; i < AHCI_MAX_PORTS; i++) {
            if (ports[i].active) {
                ahci_blkdev.capacity_sectors = ports[i].sectors;
                break;
            }
        }
    }

    return ahci_present_flag;
}

bool ahci_present(void)
{
    return ahci_present_flag;
}

struct blkdev *ahci_blkdev_primary(void)
{
    return ahci_present_flag ? &ahci_blkdev : NULL;
}
