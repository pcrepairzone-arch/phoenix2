/*
 * nvme.h – NVMe driver header
 * Author: R Andrews Grok 4 – 26 Nov 2025
 */

#ifndef NVME_H
#define NVME_H

#include <stdint.h>

/* NVMe controller structure */
typedef struct nvme_ctrl {
    void *bar0;
    uint32_t nsid;
    uint64_t num_blocks;
    uint32_t block_size;
} nvme_ctrl_t;

/* Initialize NVMe controller */
int nvme_init_controller(void *pci_dev);

/* Read/write blocks */
int nvme_read_blocks(nvme_ctrl_t *ctrl, uint64_t lba, uint32_t count, void *buf);
int nvme_write_blocks(nvme_ctrl_t *ctrl, uint64_t lba, uint32_t count, const void *buf);

#endif /* NVME_H */
