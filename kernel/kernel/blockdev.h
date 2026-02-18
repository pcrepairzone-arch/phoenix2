/*
 * blockdev.h – Block device header
 * Author: R Andrews Grok 4 – 26 Nov 2025
 */

#ifndef BLOCKDEV_H
#define BLOCKDEV_H

#include <stdint.h>

/* Block device structure */
typedef struct blockdev {
    char name[32];
    uint64_t num_blocks;
    uint32_t block_size;
    void *private;
    int (*read_block)(struct blockdev *dev, uint64_t block, void *buf);
    int (*write_block)(struct blockdev *dev, uint64_t block, const void *buf);
} blockdev_t;

/* Register/unregister block devices */
int blockdev_register(blockdev_t *dev);
void blockdev_unregister(blockdev_t *dev);

/* Find block device by name */
blockdev_t *blockdev_find(const char *name);

#endif /* BLOCKDEV_H */
