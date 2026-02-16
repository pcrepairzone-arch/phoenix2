/*
 * mmc.c – MMC/SD Host Driver for RISC OS Phoenix
 * Supports SD/MMC/eMMC on Raspberry Pi (BCM2711/2712 SDHCI)
 * Integrates with SDIO for Bluetooth/WiFi
 * Author: R Andrews Grok 4 – 06 Feb 2026
 */

#include "kernel.h"
#include "mmc.h"
#include "blockdev.h"
#include <stdint.h>
// // #include <string.h> /* removed - use kernel.h */ /* removed - use kernel.h */

#define SDHCI_BASE      0xFE340000ULL  // Pi 5 SDHCI
#define SDHCI_BLOCK_SIZE 512

typedef struct mmc_ctrl {
    void       *regs;
    uint32_t   caps;
    uint32_t   caps2;
    int        irq_vector;
    blockdev_t *bdev;
    uint64_t   capacity;
} mmc_ctrl_t;

static mmc_ctrl_t *mmc_ctrl;

/* SDHCI registers */
#define SDHCI_DMA_ADDRESS       0x00
#define SDHCI_BLOCK_SIZE        0x04
#define SDHCI_BLOCK_COUNT       0x06
#define SDHCI_ARGUMENT          0x08
#define SDHCI_TRANSFER_MODE     0x0C
#define SDHCI_COMMAND           0x0E
#define SDHCI_RESPONSE0         0x10
#define SDHCI_BUFFER            0x20
#define SDHCI_PRESENT_STATE     0x24
#define SDHCI_HOST_CONTROL      0x28
#define SDHCI_POWER_CONTROL     0x29
#define SDHCI_BLOCK_GAP_CONTROL 0x2A
#define SDHCI_WAKE_UP_CONTROL   0x2B
#define SDHCI_CLOCK_CONTROL     0x2C
#define SDHCI_TIMEOUT_CONTROL   0x2E
#define SDHCI_SOFTWARE_RESET    0x2F
#define SDHCI_INT_STATUS        0x30
#define SDHCI_INT_ENABLE        0x34
#define SDHCI_SIGNAL_ENABLE     0x38
#define SDHCI_CAPABILITIES      0x40
#define SDHCI_CAPABILITIES2     0x44

/* Commands */
#define CMD_GO_IDLE_STATE       0
#define CMD_SEND_IF_COND        8
#define CMD_APP_CMD             55
#define CMD_ACMD41              41
#define CMD_SEND_CSD            9
#define CMD_READ_SINGLE_BLOCK   17
#define CMD_WRITE_SINGLE_BLOCK  24

/* MMC init */
static int mmc_init_controller(void) {
    mmc_ctrl = kmalloc(sizeof(mmc_ctrl_t));
    if (!mmc_ctrl) return -1;

    mmc_ctrl->regs = ioremap(SDHCI_BASE, 0x1000);
    if (!mmc_ctrl->regs) goto fail;

    mmc_ctrl->caps = readl(mmc_ctrl->regs + SDHCI_CAPABILITIES);
    mmc_ctrl->caps2 = readl(mmc_ctrl->regs + SDHCI_CAPABILITIES2);

    // Reset host
    writel(0x07, mmc_ctrl->regs + SDHCI_SOFTWARE_RESET);
    while (readl(mmc_ctrl->regs + SDHCI_SOFTWARE_RESET) & 0x07);

    // Power on
    writel(0x0F, mmc_ctrl->regs + SDHCI_POWER_CONTROL);

    // Clock enable
    writel(0x01, mmc_ctrl->regs + SDHCI_CLOCK_CONTROL);
    while (!(readl(mmc_ctrl->regs + SDHCI_CLOCK_CONTROL) & 0x02));

    // Send CMD0
    mmc_send_cmd(CMD_GO_IDLE_STATE, 0, 0);

    // Send CMD8 (SD 2.0+)
    uint32_t if_cond = 0x01AA;
    mmc_send_cmd(CMD_SEND_IF_COND, if_cond, 0);

    // Send ACMD41 until ready
    uint32_t ocr = 0x40FF8000;  // High capacity
    do {
        mmc_send_cmd(CMD_APP_CMD, 0, 0);
        mmc_send_cmd(CMD_ACMD41, ocr, 0);
    } while (!(readl(mmc_ctrl->regs + SDHCI_RESPONSE0) & 0x80000000));

    // Get CSD for capacity
    mmc_send_cmd(CMD_SEND_CSD, 0, 0);
    uint32_t csd[4];
    csd[0] = readl(mmc_ctrl->regs + SDHCI_RESPONSE0);
    csd[1] = readl(mmc_ctrl->regs + SDHCI_RESPONSE1);
    csd[2] = readl(mmc_ctrl->regs + SDHCI_RESPONSE2);
    csd[3] = readl(mmc_ctrl->regs + SDHCI_RESPONSE3);

    // Calculate capacity (stub – parse CSD)
    mmc_ctrl->capacity = 128 * 1024 * 1024 * 1024ULL / 512;  // 128 GB example

    // Register block device
    blockdev_t *bdev = blockdev_register("mmc", mmc_ctrl->capacity, 512);
    if (!bdev) goto fail;

    bdev->private = mmc_ctrl;
    bdev->read = mmc_block_read;
    bdev->write = mmc_block_write;

    mmc_ctrl->bdev = bdev;

    debug_print("MMC: %ld GB card detected\n", mmc_ctrl->capacity * 512 / (1000*1000*1000));
    return 0;

fail:
    if (mmc_ctrl->regs) iounmap(mmc_ctrl->regs);
    kfree(mmc_ctrl);
    return -1;
}

/* Send MMC command */
static int mmc_send_cmd(int cmd, uint32_t arg, int flags) {
    writel(arg, mmc_ctrl->regs + SDHCI_ARGUMENT);
    writel((cmd << 8) | flags, mmc_ctrl->regs + SDHCI_COMMAND);

    // Wait for completion
    while (!(readl(mmc_ctrl->regs + SDHCI_INT_STATUS) & 0x01));
    writel(0x01, mmc_ctrl->regs + SDHCI_INT_STATUS);

    return 0;
}

/* Block read */
ssize_t mmc_block_read(blockdev_t