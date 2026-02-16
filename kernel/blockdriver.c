/*
 * blockdriver.c – Block Device Driver Glue for RISC OS Phoenix
 * Central registration and dispatch layer for all block devices
 * (NVMe, USB Mass Storage, SATA AHCI, MMC/SD, etc.)
 * Integrates with VFS and FileCore
 * Author: R andrews Grok 4 – 06 Feb 2026
 */

#include "kernel.h"
#include "blockdriver.h"
#include "vfs.h"
// // #include <string.h> /* removed - use kernel.h */ /* removed - use kernel.h */

#define MAX_BLOCKDEVS   16

/* Global list of registered block devices */
blockdev_t *blockdev_list[MAX_BLOCKDEVS];
int blockdev_count = 0;

static spinlock_t blockdev_lock = SPINLOCK_INIT;

/* Register a new block device */
blockdev_t *blockdev_register(const char *name, uint64_t size, uint32_t block_size)
{
    unsigned long flags;
    spin_lock_irqsave(&blockdev_lock, &flags);

    if (blockdev_count >= MAX_BLOCKDEVS) {
        spin_unlock_irqrestore(&blockdev_lock, flags);
        debug_print("BlockDriver: Maximum devices reached\n");
        return NULL;
    }

    blockdev_t *dev = kmalloc(sizeof(blockdev_t));
    if (!dev) {
        spin_unlock_irqrestore(&blockdev_lock, flags);
        return NULL;
    }

    strncpy(dev->name, name, 15);
    dev->name[15] = '\0';
    dev->size = size;
    dev->block_size = block_size;
    dev->unit = blockdev_count;
    dev->private = NULL;
    dev->ops = NULL;

    blockdev_list[blockdev_count++] = dev;

    spin_unlock_irqrestore(&blockdev_lock, flags);

    debug_print("BlockDriver: Registered %s (unit %d, %ld blocks)\n", 
                name, dev->unit, size);

    return dev;
}

/* Get block device by name and unit number */
blockdev_t *blockdev_get(const char *name, int unit)
{
    unsigned long flags;
    spin_lock_irqsave(&blockdev_lock, &flags);

    for (int i = 0; i < blockdev_count; i++) {
        blockdev_t *dev = blockdev_list[i];
        if (dev == NULL) continue;

        if ((unit == -1 && strcmp(dev->name, name) == 0) ||
            (unit >= 0 && dev->unit == unit)) {
            spin_unlock_irqrestore(&blockdev_lock, flags);
            return dev;
        }
    }

    spin_unlock_irqrestore(&blockdev_lock, flags);
    return NULL;
}

/* Read from block device (VFS wrapper) */
ssize_t blockdev_read(blockdev_t *dev, uint64_t lba, uint32_t count, void *buf)
{
    if (!dev || !dev->ops || !dev->ops->read) {
        debug_print("BlockDriver: No read operation for %s\n", dev ? dev->name : "NULL");
        return -1;
    }
    return dev->ops->read(dev, lba, count, buf);
}

/* Write to block device */
ssize_t blockdev_write(blockdev_t *dev, uint64_t lba, uint32_t count, const void *buf)
{
    if (!dev || !dev->ops || !dev->ops->write) {
        debug_print("BlockDriver: No write operation for %s\n", dev ? dev->name : "NULL");
        return -1;
    }
    return dev->ops->write(dev, lba, count, buf);
}

/* TRIM / DISCARD */
int blockdev_trim(blockdev_t *dev, uint64_t lba, uint64_t count)
{
    if (!dev || !dev->ops || !dev->ops->trim) {
        return -1;
    }
    return dev->ops->trim(dev, lba, count);
}

/* Poll for I/O readiness */
int blockdev_poll(blockdev_t *dev)
{
    if (!dev || !dev->ops || !dev->ops->poll) {
        return 0;
    }
    return dev->ops->poll(dev);
}

/* Close / shutdown block device */
void blockdev_close(blockdev_t *dev)
{
    if (dev && dev->ops && dev->ops->close) {
        dev->ops->close(dev);
    }
}

/* Module init */
_kernel_oserror *module_init(const char *arg, int podule)
{
    debug_print("BlockDriver: Block device subsystem initialized\n");
    return NULL;
}