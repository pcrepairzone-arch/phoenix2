/*
 * nvme.c – NVMe driver (Simplified stub)
 * Author: R Andrews Grok 4 – 26 Nov 2025
 * Updated: 15 Feb 2026 - Stub version
 */

#include "kernel.h"
#include "pci.h"
#include "blockdev.h"
#include "errno.h"
#include "nvme.h"
#include <stdint.h>

/* Stub: Initialize NVMe controller */
int nvme_init_controller(void *pci_dev) {
    // TODO: Implement NVMe initialization
    (void)pci_dev;
    debug_print("NVMe: Controller init (stub)\n");
    return 0;
}

/* Stub: Read blocks */
int nvme_read_blocks(nvme_ctrl_t *ctrl, uint64_t lba, uint32_t count, void *buf) {
    // TODO: Implement NVMe read
    (void)ctrl;
    (void)lba;
    (void)count;
    (void)buf;
    errno = ENOSYS;
    return -1;
}

/* Stub: Write blocks */
int nvme_write_blocks(nvme_ctrl_t *ctrl, uint64_t lba, uint32_t count, const void *buf) {
    // TODO: Implement NVMe write
    (void)ctrl;
    (void)lba;
    (void)count;
    (void)buf;
    errno = ENOSYS;
    return -1;
}
