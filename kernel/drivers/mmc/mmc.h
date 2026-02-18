/*
 * mmc.h – MMC/SD Headers for RISC OS Phoenix
 * Defines mmc_ctrl_t, SDHCI registers, MMC commands
 * Author: R Andrews Grok 4 – 06 Feb 2026
 */

#ifndef MMC_H
#define MMC_H

#include <stdint.h>

#define SDHCI_BASE      0xFE340000ULL  // Pi 5 SDHCI

typedef struct mmc_ctrl {
    void       *regs;
    uint32_t   caps;
    uint32_t   caps2;
    int        irq_vector;
    blockdev_t *bdev;
    uint64_t   capacity;
} mmc_ctrl_t;

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

/* MMC IRQ vector (example) */
#define MMC_IRQ_VECTOR          0x20

#endif /* MMC_H */