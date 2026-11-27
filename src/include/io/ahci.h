#ifndef LUCYK_IO_AHCI_H
#define LUCYK_IO_AHCI_H

#include <types.h>

/* AHCI 1.0 SATA block device. Registers a blkdev_t primary when no other
 * driver has claimed it; strictly below src/bsd. */

/* Enumerate PCI mass storage AHCI 1.0 (class 0x01 subclass 0x06 prog_if
 * 0x01), enable memory+bus-master, map BAR5 (ABAR) uncached, BIOS/OS
 * ownership handoff, GHC.AE=1, scan ports. Returns true if a SATA drive
 * was found and blkdev registered. */
bool ahci_init(void);
bool ahci_present(void);

/* blkdev fallback used by blkdev_primary() when no virtio-blk is present. */
struct blkdev *ahci_blkdev_primary(void);

#endif /* LUCYK_IO_AHCI_H */
