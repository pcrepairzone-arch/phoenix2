/*
 * mmc.c - MMC/SD/SDHCI Driver for Phoenix RISC OS
 * Raspberry Pi BCM2711/BCM2712 SD Host Controller
 * Based on R Andrews MMC driver, adapted for Phoenix bare-metal
 * Follows RISC OS principles: simple, direct hardware access
 */

#include "kernel.h"
#include "mmc.h"

/* Global MMC state - keep it simple */
static mmc_host_t mmc_host;

/* Direct MMIO access - no ioremap needed */
#define MMIO_READ(addr)   (*(volatile uint32_t*)(addr))
#define MMIO_WRITE(addr, val) (*(volatile uint32_t*)(addr) = (val))

/* Pi 4: EMMC2 (SD card slot) */
#define SDHCI_BASE      0xFE340000ULL

/* SDHCI Register offsets */
#define SDHCI_DMA_ADDRESS       0x00
#define SDHCI_BLOCK_SIZE        0x04
#define SDHCI_BLOCK_COUNT       0x06
#define SDHCI_ARGUMENT          0x08
#define SDHCI_TRANSFER_MODE     0x0C
#define SDHCI_COMMAND           0x0E
#define SDHCI_RESPONSE0         0x10
#define SDHCI_RESPONSE1         0x14
#define SDHCI_RESPONSE2         0x18
#define SDHCI_RESPONSE3         0x1C
#define SDHCI_BUFFER            0x20
#define SDHCI_PRESENT_STATE     0x24
#define SDHCI_HOST_CONTROL      0x28
#define SDHCI_POWER_CONTROL     0x29
#define SDHCI_CLOCK_CONTROL     0x2C
#define SDHCI_TIMEOUT_CONTROL   0x2E
#define SDHCI_SOFTWARE_RESET    0x2F
#define SDHCI_INT_STATUS        0x30
#define SDHCI_INT_ENABLE        0x34
#define SDHCI_SIGNAL_ENABLE     0x38
#define SDHCI_CAPABILITIES      0x40
#define SDHCI_CAPABILITIES2     0x44

/* SDHCI Present State bits */
#define SDHCI_CMD_INHIBIT       (1 << 0)
#define SDHCI_DAT_INHIBIT       (1 << 1)
#define SDHCI_CARD_PRESENT      (1 << 16)

/* SD Commands */
#define CMD0_GO_IDLE_STATE      0
#define CMD2_ALL_SEND_CID       2
#define CMD3_SEND_RELATIVE_ADDR 3
#define CMD7_SELECT_CARD        7
#define CMD8_SEND_IF_COND       8
#define CMD9_SEND_CSD           9
#define CMD13_SEND_STATUS       13
#define CMD16_SET_BLOCKLEN      16
#define CMD17_READ_SINGLE_BLOCK 17
#define CMD18_READ_MULTIPLE_BLOCK 18
#define CMD24_WRITE_SINGLE_BLOCK 24
#define CMD25_WRITE_MULTIPLE_BLOCK 25
#define CMD55_APP_CMD           55

/* Application-specific commands (preceded by CMD55) */
#define ACMD6_SET_BUS_WIDTH     6
#define ACMD41_SD_APP_OP_COND   41
#define ACMD51_SEND_SCR         51

/* Response types */
#define RESP_NONE   0x00
#define RESP_R1     0x1A  /* Normal response */
#define RESP_R2     0x09  /* CID, CSD register */
#define RESP_R3     0x02  /* OCR register */
#define RESP_R6     0x1A  /* Published RCA response */
#define RESP_R7     0x1A  /* Card interface condition */

/* Helper: read SDHCI register */
static inline uint32_t sdhci_read(uint32_t offset)
{
    uint64_t addr = mmc_host.base + offset;
    return MMIO_READ(addr);
}

/* Helper: write SDHCI register */
static inline void sdhci_write(uint32_t offset, uint32_t value)
{
    uint64_t addr = mmc_host.base + offset;
    MMIO_WRITE(addr, value);
}

/* Helper: wait for command ready */
static int sdhci_wait_cmd_ready(void)
{
    int timeout = 10000;
    
    while ((sdhci_read(SDHCI_PRESENT_STATE) & SDHCI_CMD_INHIBIT) && timeout > 0) {
        timeout--;
        /* Small delay */
        for (volatile int i = 0; i < 100; i++);
    }
    
    return timeout > 0 ? 0 : -1;
}

/* Helper: wait for data ready */
static int sdhci_wait_data_ready(void)
{
    int timeout = 10000;
    
    while ((sdhci_read(SDHCI_PRESENT_STATE) & SDHCI_DAT_INHIBIT) && timeout > 0) {
        timeout--;
        for (volatile int i = 0; i < 100; i++);
    }
    
    return timeout > 0 ? 0 : -1;
}

/* Send SD command */
static int mmc_send_cmd(uint32_t cmd, uint32_t arg, uint32_t *response)
{
    uint32_t cmd_reg;
    uint32_t resp_type;
    
    /* Wait for command line ready */
    if (sdhci_wait_cmd_ready() < 0) {
        debug_print("MMC: CMD%d timeout waiting for ready\n", cmd & 0x3F);
        return -1;
    }
    
    /* Determine response type */
    switch (cmd) {
        case CMD0_GO_IDLE_STATE:
            resp_type = RESP_NONE;
            break;
        case CMD2_ALL_SEND_CID:
        case CMD9_SEND_CSD:
            resp_type = RESP_R2;
            break;
        case CMD8_SEND_IF_COND:
            resp_type = RESP_R7;
            break;
        case ACMD41_SD_APP_OP_COND:
            resp_type = RESP_R3;
            break;
        default:
            resp_type = RESP_R1;
            break;
    }
    
    /* Write argument */
    sdhci_write(SDHCI_ARGUMENT, arg);
    
    /* Build command register */
    cmd_reg = ((cmd & 0x3F) << 8) | resp_type;
    
    /* Clear interrupt status */
    sdhci_write(SDHCI_INT_STATUS, 0xFFFFFFFF);
    
    /* Issue command */
    sdhci_write(SDHCI_COMMAND, cmd_reg);
    
    /* Wait for command complete */
    int timeout = 10000;
    uint32_t status;
    
    while (timeout > 0) {
        status = sdhci_read(SDHCI_INT_STATUS);
        
        if (status & 0x8000) {  /* Error */
            debug_print("MMC: CMD%d error, status=0x%x\n", cmd & 0x3F, status);
            sdhci_write(SDHCI_INT_STATUS, 0xFFFF);
            return -1;
        }
        
        if (status & 0x01) {  /* Command complete */
            break;
        }
        
        timeout--;
        for (volatile int i = 0; i < 100; i++);
    }
    
    if (timeout == 0) {
        debug_print("MMC: CMD%d timeout\n", cmd & 0x3F);
        return -1;
    }
    
    /* Clear command complete interrupt */
    sdhci_write(SDHCI_INT_STATUS, 0x01);
    
    /* Read response if needed */
    if (response && resp_type != RESP_NONE) {
        response[0] = sdhci_read(SDHCI_RESPONSE0);
        if (resp_type == RESP_R2) {
            response[1] = sdhci_read(SDHCI_RESPONSE1);
            response[2] = sdhci_read(SDHCI_RESPONSE2);
            response[3] = sdhci_read(SDHCI_RESPONSE3);
        }
    }
    
    return 0;
}

/* Send application command (CMD55 + ACMD) */
static int mmc_send_app_cmd(uint32_t acmd, uint32_t arg, uint32_t *response)
{
    /* Send CMD55 first */
    if (mmc_send_cmd(CMD55_APP_CMD, mmc_host.rca << 16, NULL) < 0) {
        return -1;
    }
    
    /* Then send the actual ACMD */
    return mmc_send_cmd(acmd, arg, response);
}

/* Reset SD controller */
static void sdhci_reset(void)
{
    /* Software reset for all */
    sdhci_write(SDHCI_SOFTWARE_RESET, 0x07);
    
    /* Wait for reset complete */
    int timeout = 10000;
    while ((sdhci_read(SDHCI_SOFTWARE_RESET) & 0x07) && timeout > 0) {
        timeout--;
        for (volatile int i = 0; i < 100; i++);
    }
    
    debug_print("MMC: Reset %s\n", timeout > 0 ? "OK" : "TIMEOUT");
}

/* Initialize SD card */
int mmc_init(void)
{
    uint32_t response[4];
    uint32_t ocr;
    
    debug_print("MMC: Initializing SD host controller\n");
    
    /* Set up host structure */
    mmc_host.base = SDHCI_BASE;
    mmc_host.rca = 0;
    mmc_host.capacity = 0;
    mmc_host.block_size = 512;
    
    /* Check card present */
    uint32_t present = sdhci_read(SDHCI_PRESENT_STATE);
    if (!(present & SDHCI_CARD_PRESENT)) {
        debug_print("MMC: No card detected\n");
        return -1;
    }
    
    /* Reset controller */
    sdhci_reset();
    
    /* Power on */
    sdhci_write(SDHCI_POWER_CONTROL, 0x0F);  /* 3.3V, power on */
    
    /* Enable internal clock */
    sdhci_write(SDHCI_CLOCK_CONTROL, 0x01);
    
    /* Wait for clock stable */
    int timeout = 10000;
    while (!(sdhci_read(SDHCI_CLOCK_CONTROL) & 0x02) && timeout > 0) {
        timeout--;
    }
    
    /* Enable SD clock */
    sdhci_write(SDHCI_CLOCK_CONTROL, sdhci_read(SDHCI_CLOCK_CONTROL) | 0x04);
    
    /* Set timeout */
    sdhci_write(SDHCI_TIMEOUT_CONTROL, 0x0E);
    
    debug_print("MMC: Clock enabled\n");
    
    /* CMD0: GO_IDLE_STATE */
    mmc_send_cmd(CMD0_GO_IDLE_STATE, 0, NULL);
    
    /* CMD8: SEND_IF_COND (SD 2.0+ voltage check) */
    if (mmc_send_cmd(CMD8_SEND_IF_COND, 0x1AA, response) == 0) {
        if ((response[0] & 0xFF) == 0xAA) {
            debug_print("MMC: SD 2.0+ card detected\n");
            mmc_host.version = 2;
        }
    } else {
        debug_print("MMC: SD 1.x card or MMC\n");
        mmc_host.version = 1;
    }
    
    /* ACMD41: SD_APP_OP_COND (initialize card) */
    timeout = 1000;
    ocr = (mmc_host.version == 2) ? 0x40FF8000 : 0x00FF8000;
    
    while (timeout > 0) {
        if (mmc_send_app_cmd(ACMD41_SD_APP_OP_COND, ocr, response) == 0) {
            if (response[0] & 0x80000000) {  /* Card ready */
                /* Check if SDHC/SDXC */
                if (response[0] & 0x40000000) {
                    mmc_host.high_capacity = 1;
                    debug_print("MMC: SDHC/SDXC card\n");
                } else {
                    mmc_host.high_capacity = 0;
                    debug_print("MMC: SDSC card\n");
                }
                break;
            }
        }
        timeout--;
        for (volatile int i = 0; i < 10000; i++);
    }
    
    if (timeout == 0) {
        debug_print("MMC: Card initialization timeout\n");
        return -1;
    }
    
    /* CMD2: ALL_SEND_CID */
    if (mmc_send_cmd(CMD2_ALL_SEND_CID, 0, response) < 0) {
        debug_print("MMC: Failed to get CID\n");
        return -1;
    }
    
    /* CMD3: SEND_RELATIVE_ADDR */
    if (mmc_send_cmd(CMD3_SEND_RELATIVE_ADDR, 0, response) < 0) {
        debug_print("MMC: Failed to get RCA\n");
        return -1;
    }
    
    mmc_host.rca = response[0] >> 16;
    debug_print("MMC: RCA = 0x%x\n", mmc_host.rca);
    
    /* CMD9: SEND_CSD (get card capacity) */
    if (mmc_send_cmd(CMD9_SEND_CSD, mmc_host.rca << 16, response) < 0) {
        debug_print("MMC: Failed to get CSD\n");
        return -1;
    }
    
    /* Parse CSD for capacity */
    if (mmc_host.high_capacity) {
        /* CSD v2.0 (SDHC/SDXC) */
        uint32_t c_size = ((response[1] & 0x3F) << 16) | (response[2] >> 16);
        mmc_host.capacity = (c_size + 1) * 512;  /* In 512-byte blocks */
    } else {
        /* CSD v1.0 (SDSC) */
        uint32_t c_size = ((response[1] & 0x3FF) << 2) | (response[2] >> 30);
        uint32_t c_size_mult = (response[2] >> 15) & 0x07;
        uint32_t read_bl_len = (response[1] >> 16) & 0x0F;
        mmc_host.capacity = (c_size + 1) * (1 << (c_size_mult + 2)) * (1 << read_bl_len) / 512;
    }
    
    debug_print("MMC: Capacity = %llu MB\n", (mmc_host.capacity * 512) / (1024 * 1024));
    
    /* CMD7: SELECT_CARD */
    if (mmc_send_cmd(CMD7_SELECT_CARD, mmc_host.rca << 16, NULL) < 0) {
        debug_print("MMC: Failed to select card\n");
        return -1;
    }
    
    /* Set block size to 512 bytes */
    if (mmc_send_cmd(CMD16_SET_BLOCKLEN, 512, NULL) < 0) {
        debug_print("MMC: Failed to set block size\n");
        return -1;
    }
    
    debug_print("MMC: Initialization complete\n");
    return 0;
}

/* Read blocks from SD card */
int mmc_read_blocks(uint32_t start_block, uint32_t num_blocks, void *buffer)
{
    uint32_t *buf = (uint32_t *)buffer;
    uint32_t addr = mmc_host.high_capacity ? start_block : (start_block * 512);
    
    if (!mmc_host.capacity) {
        return -1;  /* Not initialized */
    }
    
    /* Wait for data line ready */
    if (sdhci_wait_data_ready() < 0) {
        return -1;
    }
    
    /* Set block size and count */
    sdhci_write(SDHCI_BLOCK_SIZE, 512 | (7 << 12));  /* 512 bytes, no DMA boundary */
    sdhci_write(SDHCI_BLOCK_COUNT, num_blocks);
    
    /* Send read command */
    uint32_t cmd = (num_blocks == 1) ? CMD17_READ_SINGLE_BLOCK : CMD18_READ_MULTIPLE_BLOCK;
    if (mmc_send_cmd(cmd, addr, NULL) < 0) {
        return -1;
    }
    
    /* Read data via PIO */
    for (uint32_t block = 0; block < num_blocks; block++) {
        /* Wait for buffer read ready */
        int timeout = 100000;
        while (!(sdhci_read(SDHCI_INT_STATUS) & 0x20) && timeout > 0) {
            timeout--;
        }
        
        if (timeout == 0) {
            debug_print("MMC: Read timeout\n");
            return -1;
        }
        
        /* Read 512 bytes (128 words) */
        for (int i = 0; i < 128; i++) {
            *buf++ = sdhci_read(SDHCI_BUFFER);
        }
        
        /* Clear buffer read ready */
        sdhci_write(SDHCI_INT_STATUS, 0x20);
    }
    
    /* Wait for transfer complete */
    int timeout = 100000;
    while (!(sdhci_read(SDHCI_INT_STATUS) & 0x02) && timeout > 0) {
        timeout--;
    }
    
    sdhci_write(SDHCI_INT_STATUS, 0x02);  /* Clear transfer complete */
    
    return 0;
}

/* Write blocks to SD card */
int mmc_write_blocks(uint32_t start_block, uint32_t num_blocks, const void *buffer)
{
    const uint32_t *buf = (const uint32_t *)buffer;
    uint32_t addr = mmc_host.high_capacity ? start_block : (start_block * 512);
    
    if (!mmc_host.capacity) {
        return -1;
    }
    
    if (sdhci_wait_data_ready() < 0) {
        return -1;
    }
    
    sdhci_write(SDHCI_BLOCK_SIZE, 512 | (7 << 12));
    sdhci_write(SDHCI_BLOCK_COUNT, num_blocks);
    
    uint32_t cmd = (num_blocks == 1) ? CMD24_WRITE_SINGLE_BLOCK : CMD25_WRITE_MULTIPLE_BLOCK;
    if (mmc_send_cmd(cmd, addr, NULL) < 0) {
        return -1;
    }
    
    /* Write data via PIO */
    for (uint32_t block = 0; block < num_blocks; block++) {
        /* Wait for buffer write ready */
        int timeout = 100000;
        while (!(sdhci_read(SDHCI_INT_STATUS) & 0x10) && timeout > 0) {
            timeout--;
        }
        
        if (timeout == 0) {
            return -1;
        }
        
        /* Write 512 bytes */
        for (int i = 0; i < 128; i++) {
            sdhci_write(SDHCI_BUFFER, *buf++);
        }
        
        sdhci_write(SDHCI_INT_STATUS, 0x10);
    }
    
    /* Wait for transfer complete */
    int timeout = 100000;
    while (!(sdhci_read(SDHCI_INT_STATUS) & 0x02) && timeout > 0) {
        timeout--;
    }
    
    sdhci_write(SDHCI_INT_STATUS, 0x02);
    
    return 0;
}

/* Get card info */
void mmc_get_info(mmc_card_info_t *info)
{
    info->capacity_mb = (mmc_host.capacity * 512) / (1024 * 1024);
    info->block_size = mmc_host.block_size;
    info->high_capacity = mmc_host.high_capacity;
    info->version = mmc_host.version;
}
