/*
 * blockdriver.h – Block Device Driver Interface for RISC OS Phoenix
 * Defines the standard block device API used by NVMe, USB, SATA, MMC, etc.
 * Integrates with VFS and FileCore
 * Author: R Andrews – 06 Feb 2026
 */

#ifndef BLOCKDRIVER_H
#define BLOCKDRIVER_H

#include <stdint.h>

/* Block device structure */
typedef struct blockdev blockdev_t;

/* Block device operations */
typedef struct blockdev_ops {
    /* Read blocks from device */
    ssize_t (*read)(blockdev_t *dev, uint64_t lba, uint32_t count, void *buf);

    /* Write blocks to device */
    ssize_t (*write)(blockdev_t *dev, uint64_t lba, uint32_t count, const void *buf);

    /* TRIM / DISCARD (deallocate blocks) */
    int (*trim)(blockdev_t *dev, uint64_t lba, uint64_t count);

    /* Poll for I/O readiness (optional) */
    int (*poll)(blockdev_t *dev);

    /* Shutdown / cleanup */
    void (*close)(blockdev_t *dev);
} blockdev_ops_t;

/* ── Media class — set by each driver at registration time ───────────────────
 * Used by FileCore disc scoring to prefer faster / more reliable boot media.
 * Scoring order (highest to lowest): NVME > SSD > USB_FLASH > SD > UNKNOWN  */
typedef enum {
    MEDIA_UNKNOWN   = 0,   /* not classified (treated as USB_FLASH for scoring) */
    MEDIA_SD        = 1,   /* SD / eMMC / MMC card                              */
    MEDIA_USB_FLASH = 2,   /* USB thumb drive / flash stick                     */
    MEDIA_SSD       = 3,   /* USB-attached SATA SSD (ASMedia, JMicron etc.)     */
    MEDIA_NVME      = 4,   /* USB-NVMe bridge (RTL9210, JMS583 etc.)            */
} media_class_t;

/* Main block device descriptor */
struct blockdev {
    char            name[16];       /* e.g. "nvme", "usb", "sata", "mmc"        */
    uint64_t        size;           /* Total number of blocks                    */
    uint32_t        block_size;     /* Usually 512 or 4096                       */
    int             unit;           /* Unit number (for multi-device)            */
    media_class_t   media_class;    /* Media type — set by registering driver    */
    void           *private;        /* Driver private data                       */
    blockdev_ops_t *ops;            /* Operations table                          */
};

/* Register a new block device */
blockdev_t *blockdev_register(const char *name, uint64_t size, uint32_t block_size);

/* Get block device by name and unit */
blockdev_t *blockdev_get(const char *name, int unit);

/* Global list of registered block devices */
extern blockdev_t *blockdev_list[];
extern int blockdev_count;

#endif /* BLOCKDRIVER_H */
