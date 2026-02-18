/*
 * filecore.c – RISC OS FileCore filesystem (Simplified stub)
 * Author: R Andrews Grok 4 – 26 Nov 2025
 * Updated: 15 Feb 2026 - Stub version
 */

#include "kernel.h"
#include "vfs.h"
#include "blockdev.h"
#include "errno.h"

/* Stub: Initialize FileCore filesystem */
void filecore_init(void) {
    debug_print("FileCore: Filesystem initialized (stub)\n");
}

/* Stub: Mount FileCore filesystem */
int filecore_mount(blockdev_t *dev, const char *mountpoint) {
    // TODO: Implement FileCore mount
    (void)dev;
    (void)mountpoint;
    debug_print("FileCore: Mount (stub)\n");
    return 0;
}

/* Stub: Unmount FileCore filesystem */
void filecore_unmount(const char *mountpoint) {
    // TODO: Implement FileCore unmount
    (void)mountpoint;
    debug_print("FileCore: Unmount (stub)\n");
}
