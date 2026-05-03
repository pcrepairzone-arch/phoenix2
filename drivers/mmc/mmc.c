/*
 * mmc.c - MMC/SD/SDHCI Driver for Phoenix RISC OS
 * Raspberry Pi BCM2711/BCM2712 SD Host Controller
 * Based on R Andrews MMC driver, adapted for Phoenix bare-metal
 * Follows RISC OS principles: simple, direct hardware access
 */

#include "kernel.h"
#include "mmc.h"
#include "blockdriver.h"   /* blockdev_register(), blockdev_ops_t */

/* Global MMC state - keep it simple */
static mmc_host_t mmc_host;

/* ── Wall-clock helper (CNTPCT_EL0, 54 MHz on BCM2711) ────────────────────
 * Matches get_time_ms() in usb_xhci.c — copied here to avoid a cross-driver
 * dependency.  Returns low 32 bits of elapsed ms; wraps every ~49 days.    */
static inline uint32_t mmc_time_ms(void) {
    uint64_t v;
    asm volatile("isb; mrs %0, cntpct_el0" : "=r"(v) :: "memory");
    return (uint32_t)(v / 54000ULL);
}

/* Direct MMIO access - no ioremap needed */
#define MMIO_READ(addr)   (*(volatile uint32_t*)(addr))
#define MMIO_WRITE(addr, val) (*(volatile uint32_t*)(addr) = (val))

/*
 * EMMC2 is at peripheral_base + 0x340000.
 * peripheral_base is set by detect_peripheral_base() before mmc_init() runs,
 * so this resolves correctly for both Pi 4 (0xFE000000) and Pi 5 (0x107C000000).
 * DO NOT use a hardcoded 0xFE340000 — it breaks on Pi 5.
 */
#define EMMC2_OFFSET    0x340000ULL

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

/* Response types (COMMAND register bits [4:0]) */
#define RESP_NONE   0x00  /* No response                        */
#define RESP_R1     0x1A  /* 48-bit, CRC + index check          */
#define RESP_R2     0x09  /* 136-bit CID/CSD, CRC check         */
#define RESP_R3     0x02  /* 48-bit OCR, no checks              */
#define RESP_R6     0x1A  /* Published RCA response             */
#define RESP_R7     0x1A  /* Card interface condition           */

/*
 * COMMAND register bit 5 — DATA_PRESENT_SELECT.
 * Set this for commands that have a data phase (CMD17/18/24/25).
 * Without it the SDHCI will not drive the data lines or post
 * BUFFER_READ_READY / BUFFER_WRITE_READY status bits.
 */
#define CMD_DATA_PRESENT    (1 << 5)

/*
 * TRANSFER_MODE register bits (16-bit, offset 0x0C lower half).
 * TRANSFER_MODE and COMMAND must ALWAYS be written together as a
 * single 32-bit write to SDHCI_TRANSFER_MODE (0x0C).
 * Writing to SDHCI_COMMAND (0x0E) as a 32-bit access is an unaligned
 * Device-memory write on AArch64 and will cause an alignment fault.
 */
#define TMODE_DMA_EN        (1 << 0)  /* 0 = PIO (we use PIO)          */
#define TMODE_BCEN          (1 << 1)  /* Block count enable            */
#define TMODE_READ          (1 << 4)  /* 1 = card→host (read)          */
#define TMODE_MULTI         (1 << 5)  /* Multi-block transfer          */

/* Helper: read SDHCI register */
static inline uint32_t sdhci_read(uint32_t offset)
{
    uint64_t addr = mmc_host.base + offset;
    return MMIO_READ(addr);
}

/* Helper: write SDHCI register (32-bit) */
static inline void sdhci_write(uint32_t offset, uint32_t value)
{
    uint64_t addr = mmc_host.base + offset;
    MMIO_WRITE(addr, value);
}

/*
 * sdhci_write16 — write a 16-bit SDHCI register.
 *
 * Required for SDHCI registers that are defined as 16-bit wide in the spec:
 *   SDHCI_BLOCK_COUNT   (0x06): upper half of the 32-bit BLOCK_SIZE register
 *   SDHCI_CLOCK_CONTROL (0x2C): shares a 32-bit word with TIMEOUT (0x2E)
 *   SDHCI_TRANSFER_MODE (0x0C): shares a 32-bit word with COMMAND (0x0E)
 *
 * Using a 32-bit write to these addresses stomps the adjacent 16-bit register.
 * The BCM2711 arasan SDHCI is strict about register widths.
 */
static inline void sdhci_write16(uint32_t offset, uint16_t value)
{
    *(volatile uint16_t *)(mmc_host.base + offset) = value;
    asm volatile("dsb sy" ::: "memory");
}

/*
 * sdhci_write8 — write an 8-bit SDHCI register.
 *
 * Required for:
 *   SDHCI_TIMEOUT_CONTROL (0x2E): shares 32-bit word with CLOCK_CONTROL (0x2C)
 *   SDHCI_SOFTWARE_RESET  (0x2F): shares 32-bit word with TIMEOUT_CONTROL
 *   SDHCI_POWER_CONTROL   (0x29): shares 32-bit word with HOST_CONTROL (0x28)
 *
 * A 32-bit write to 0x2F would also modify TIMEOUT_CONTROL at 0x2E —
 * potentially causing the card to timeout immediately after reset.
 */
static inline void sdhci_write8(uint32_t offset, uint8_t value)
{
    *(volatile uint8_t *)(mmc_host.base + offset) = value;
    asm volatile("dsb sy" ::: "memory");
}

/*
 * sdhci_read16 — read a 16-bit SDHCI register.
 */
static inline uint16_t sdhci_read16(uint32_t offset)
{
    return *(volatile uint16_t *)(mmc_host.base + offset);
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

/*
 * mmc_send_cmd — issue one SD/MMC command.
 *
 * @xfer_mode  TMODE_* bits for data commands; 0 for non-data commands.
 *             For read commands pass TMODE_READ (and TMODE_MULTI|TMODE_BCEN
 *             for multi-block).  For write commands pass TMODE_MULTI|TMODE_BCEN
 *             if multi-block; 0 for single-block write.
 *
 * SDHCI_TRANSFER_MODE (0x0C) and SDHCI_COMMAND (0x0E) must be written as
 * one 32-bit store to the aligned address 0x0C.  Writing to 0x0E directly
 * is an unaligned Device-memory access and causes an alignment fault on
 * AArch64.
 */
static int mmc_send_cmd(uint32_t cmd, uint32_t arg,
                        uint32_t *response, uint16_t xfer_mode)
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

    /* Build command register.
     * Set DATA_PRESENT_SELECT for commands that transfer data over DAT lines. */
    cmd_reg = ((cmd & 0x3F) << 8) | resp_type;
    if (xfer_mode != 0)
        cmd_reg |= CMD_DATA_PRESENT;

    /* Clear interrupt status */
    sdhci_write(SDHCI_INT_STATUS, 0xFFFFFFFF);

    /*
     * Issue command — combined 32-bit write to SDHCI_TRANSFER_MODE (0x0C).
     * Lower 16 bits = TRANSFER_MODE, upper 16 bits = COMMAND.
     * This is the only correctly-aligned way to reach the COMMAND register.
     */
    sdhci_write(SDHCI_TRANSFER_MODE,
                ((uint32_t)cmd_reg << 16) | (uint32_t)xfer_mode);
    
    /* Wait for command complete */
    int timeout = 10000;
    uint32_t status;
    
    while (timeout > 0) {
        status = sdhci_read(SDHCI_INT_STATUS);
        
        if (status & 0x8000) {  /* Error */
            debug_print("MMC: CMD%d error, status=0x%x\n", cmd & 0x3F, status);
            sdhci_write(SDHCI_INT_STATUS, 0xFFFFFFFF);
            /*
             * Reset CMD line (and DAT line if a data error occurred) so that
             * CMD_INHIBIT (and DAT_INHIBIT) clear before the next command.
             * Without this reset the controller refuses all further commands.
             */
            uint8_t srst = 0x02;                    /* SRST_CMD always */
            if (status & 0x0070) srst |= 0x04;      /* SRST_DAT on data errors */
            sdhci_write8(SDHCI_SOFTWARE_RESET, srst);
            {
                int rst_wait = 1000;
                while ((*(volatile uint8_t *)(mmc_host.base + SDHCI_SOFTWARE_RESET) & srst)
                       && rst_wait--)
                    for (volatile int i = 0; i < 100; i++);
            }
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
        /*
         * CMD_INHIBIT stays asserted after a command timeout.  Reset the CMD
         * line so the next call to sdhci_wait_cmd_ready() doesn't see a
         * permanently-stuck bit and return -1 immediately for every subsequent
         * command (which was the CMD55 spam seen in boot 63).
         */
        sdhci_write8(SDHCI_SOFTWARE_RESET, 0x02);   /* SRST_CMD */
        {
            int rst_wait = 1000;
            while ((*(volatile uint8_t *)(mmc_host.base + SDHCI_SOFTWARE_RESET) & 0x02)
                   && rst_wait--)
                for (volatile int i = 0; i < 100; i++);
        }
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

/* Send application command (CMD55 + ACMD) — both are non-data, xfer_mode=0 */
static int mmc_send_app_cmd(uint32_t acmd, uint32_t arg, uint32_t *response)
{
    if (mmc_send_cmd(CMD55_APP_CMD, mmc_host.rca << 16, NULL, 0) < 0)
        return -1;
    return mmc_send_cmd(acmd, arg, response, 0);
}

/* Reset SD controller */
static void sdhci_reset(void)
{
    /* FIX-B: SOFTWARE_RESET is 8-bit at 0x2F. Use byte access only.
     * 0x07 = SRST_DAT | SRST_CMD | SRST_ALL */
    sdhci_write8(SDHCI_SOFTWARE_RESET, 0x07);

    /* Wait for reset complete — all three bits must clear */
    int timeout = 10000;
    while ((*(volatile uint8_t *)(mmc_host.base + SDHCI_SOFTWARE_RESET) & 0x07)
           && timeout > 0) {
        timeout--;
        for (volatile int i = 0; i < 100; i++);
    }

    debug_print("MMC: Reset %s\n", timeout > 0 ? "OK" : "TIMEOUT");
}

/* ── Block device ops ────────────────────────────────────────────────────────
 * Thin wrappers so the blockdriver layer can call mmc_read/write_blocks.
 * mmc_read_blocks / mmc_write_blocks return 0 on success; blockdev_ops_t
 * expects bytes-transferred on success or -1 on error.
 */
static ssize_t mmc_bd_read(blockdev_t *dev, uint64_t lba,
                           uint32_t count, void *buf)
{
    (void)dev;
    return mmc_read_blocks((uint32_t)lba, count, buf) == 0
           ? (ssize_t)(count * 512) : -1;
}

static ssize_t mmc_bd_write(blockdev_t *dev, uint64_t lba,
                            uint32_t count, const void *buf)
{
    (void)dev;
    return mmc_write_blocks((uint32_t)lba, count, (void *)buf) == 0
           ? (ssize_t)(count * 512) : -1;
}

static blockdev_ops_t mmc_bd_ops = {
    .read  = mmc_bd_read,
    .write = mmc_bd_write,
    .trim  = NULL,
    .poll  = NULL,
    .close = NULL,
};

/* Initialize SD card */
int mmc_init(void)
{
    uint32_t response[4];
    uint32_t ocr;
    
    debug_print("MMC: Initializing SD host controller\n");
    
    /* Set up host structure — base address from board detection */
    mmc_host.base = peripheral_base + EMMC2_OFFSET;
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
    
    /* FIX-B: POWER_CONTROL is 8-bit at offset 0x29.
     * 32-bit write at 0x28 overlaps HOST_CONTROL — use byte write.
     * 0x0F = SDVSEL(3.3V)=0xE | SD_BUS_POWER=1 */
    sdhci_write8(SDHCI_POWER_CONTROL, 0x0F);
    
    /*
     * CLOCK_CONTROL (16-bit at 0x2C) — set divider for ≤400KHz init clock.
     *
     * Bits [15:8] = SDCLK_FREQ_SEL.  For SDHCI 2.0+ the divider is:
     *   SDCLK = base_clock / (2 × SDCLK_FREQ_SEL)
     * BCM2711 EMMC2 base clock from firmware is typically 100 MHz.
     * SDCLK_FREQ_SEL = 0x80 (128) → 100 MHz / 256 ≈ 390 KHz  ✓
     *
     * Previous value 0x0001 set SDCLK_FREQ_SEL=0 → base_clock / 2 ≈ 50 MHz,
     * far too fast for card initialisation — explains CMD0/CMD8 timeouts.
     *
     * Use 16-bit writes only; 32-bit writes would stomp TIMEOUT_CONTROL at 0x2E.
     */
    sdhci_write16(SDHCI_CLOCK_CONTROL, 0x8001);  /* div=128 (~390KHz) + int-clk-en */

    /* Wait for Internal Clock Stable (bit 1) */
    int timeout = 10000;
    while (!(sdhci_read16(SDHCI_CLOCK_CONTROL) & 0x0002) && timeout > 0) {
        timeout--;
    }

    /* Enable SD clock output (bit 2) */
    sdhci_write16(SDHCI_CLOCK_CONTROL,
                  sdhci_read16(SDHCI_CLOCK_CONTROL) | 0x0004);
    
    /* FIX-B: TIMEOUT_CONTROL is 8-bit at 0x2E. A 32-bit write at 0x2C
     * (the aligned address) would overwrite CLOCK_CONTROL.
     * 0x0E = data timeout = TMCLK × 2^(0x0E+13) ≈ 1 second */
    sdhci_write8(SDHCI_TIMEOUT_CONTROL, 0x0E);

    /*
     * SDHCI spec §3.5 (Normal Interrupt Status Enable Register, 0x34):
     *   "If a bit is set to 0, the corresponding Normal Interrupt Status
     *    Register bit is always 0."
     * After SRST_ALL the register resets to 0x00000000, so CMD_COMPLETE
     * (bit 0 of INT_STATUS) can NEVER be latched — every command poll
     * of INT_STATUS & 0x01 returns 0 → timeout.  This was the root cause
     * of ALL MMC command timeouts in boots 63 and 64.
     *
     * Lower 16 bits = Normal INT Status Enable:
     *   bit 0  CMD_COMPLETE    ← essential for mmc_send_cmd polling
     *   bit 1  XFER_COMPLETE   ← essential for data read/write
     *   bit 2  BLOCK_GAP
     *   bit 4  BWR (buffer write ready)
     *   bit 5  BRR (buffer read ready)
     *   bit 15 ERROR_INT summary  ← essential for error detection
     * Upper 16 bits = Error INT Status Enable:
     *   0x00FF = CMD timeout, CMD CRC, CMD end-bit, CMD index,
     *            DATA timeout, DATA CRC, DATA end-bit, current limit
     *
     * SIGNAL_ENABLE (0x38) stays 0: polling mode, no hardware IRQ output.
     */
    sdhci_write(SDHCI_INT_ENABLE,    0x00FF8037U);
    sdhci_write(SDHCI_SIGNAL_ENABLE, 0x00000000U);

    debug_print("MMC: Clock enabled\n");

    /*
     * SD spec §6.4.1.1 — host must supply ≥74 SD clock cycles to the card
     * before issuing CMD0.  At 390 KHz that is ≈190 µs; we busy-wait ~2 ms
     * to give comfortable headroom and let the card's internal power rail
     * settle completely.
     */
    for (volatile int i = 0; i < 1000000; i++);

    /* CMD0: GO_IDLE_STATE — no response, no data */
    mmc_send_cmd(CMD0_GO_IDLE_STATE, 0, NULL, 0);

    /* CMD8: SEND_IF_COND (SD 2.0+ voltage check) — no data */
    if (mmc_send_cmd(CMD8_SEND_IF_COND, 0x1AA, response, 0) == 0) {
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
    
    /* CMD2: ALL_SEND_CID — no data */
    if (mmc_send_cmd(CMD2_ALL_SEND_CID, 0, response, 0) < 0) {
        debug_print("MMC: Failed to get CID\n");
        return -1;
    }

    /* CMD3: SEND_RELATIVE_ADDR — no data */
    if (mmc_send_cmd(CMD3_SEND_RELATIVE_ADDR, 0, response, 0) < 0) {
        debug_print("MMC: Failed to get RCA\n");
        return -1;
    }

    mmc_host.rca = response[0] >> 16;
    debug_print("MMC: RCA = 0x%x\n", mmc_host.rca);

    /* CMD9: SEND_CSD (get card capacity) — R2 response, no DAT lines */
    if (mmc_send_cmd(CMD9_SEND_CSD, mmc_host.rca << 16, response, 0) < 0) {
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
    
    /* CMD7: SELECT_CARD — no data */
    if (mmc_send_cmd(CMD7_SELECT_CARD, mmc_host.rca << 16, NULL, 0) < 0) {
        debug_print("MMC: Failed to select card\n");
        return -1;
    }

    /* CMD16: SET_BLOCKLEN — no data */
    if (mmc_send_cmd(CMD16_SET_BLOCKLEN, 512, NULL, 0) < 0) {
        debug_print("MMC: Failed to set block size\n");
        return -1;
    }

    /* boot166: raise SD clock from init speed (~390 KHz) to 25 MHz for
     * data transfers.  At 390 KHz a 512-byte block takes ~10 ms to
     * transfer, but the PIO read-ready loop only spun ~0.07 ms — the
     * buffer-read-ready bit never had time to set before timeout.
     *
     * Sequence (SDHCI spec §3.2.3):
     *   1. Clear SD Clock Enable (bit 2) — stop clock while changing divider.
     *   2. Write new SDCLK_FREQ_SEL.
     *   3. Wait for Internal Clock Stable (bit 1).
     *   4. Set SD Clock Enable (bit 2) again.
     *
     * BCM2711 EMMC2 base clock = 200 MHz.
     * SDCLK_FREQ_SEL = 4 → 200 MHz / 8 = 25 MHz (Normal Speed, safe for all SD).
     * Writing 0x0401: bits[15:8]=0x04 (SEL), bit0=1 (Internal Clock Enable).    */
    sdhci_write16(SDHCI_CLOCK_CONTROL,
                  sdhci_read16(SDHCI_CLOCK_CONTROL) & ~0x0004u);  /* SD CLK off */
    sdhci_write16(SDHCI_CLOCK_CONTROL, 0x0401u);                  /* SEL=4, ICS EN */
    {
        int ics_wait = 10000;
        while (!(sdhci_read16(SDHCI_CLOCK_CONTROL) & 0x0002u) && ics_wait-- > 0);
    }
    sdhci_write16(SDHCI_CLOCK_CONTROL,
                  sdhci_read16(SDHCI_CLOCK_CONTROL) | 0x0004u);   /* SD CLK on */
    debug_print("MMC: Clock raised to 25 MHz for data transfer\n");

    /* Register with the block device layer — see mmc_bd_* below */
    blockdev_t *bd = blockdev_register("mmc", mmc_host.capacity, 512);
    if (bd) {
        bd->ops         = &mmc_bd_ops;
        bd->media_class = MEDIA_SD;
        debug_print("MMC: Registered as block device unit %d (%llu blocks)\n",
                    bd->unit, mmc_host.capacity);
    } else {
        debug_print("MMC: blockdev_register failed\n");
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
    
    /* Set block size and count in one 32-bit write to 0x04.
     * Lower 16 bits → BLOCK_SIZE: 512 bytes, SDMA boundary = 512KB (7<<12)
     * Upper 16 bits → BLOCK_COUNT: number of blocks to transfer.
     * Writing them atomically avoids an unaligned 32-bit write to 0x06. */
    sdhci_write(SDHCI_BLOCK_SIZE,
                ((uint32_t)num_blocks << 16) | (512 | (7 << 12)));

    /* Send read command with TRANSFER_MODE:
     *   TMODE_READ  — data direction card→host
     *   TMODE_MULTI + TMODE_BCEN — enable block counter for multi-block */
    uint32_t cmd = (num_blocks == 1) ? CMD17_READ_SINGLE_BLOCK : CMD18_READ_MULTIPLE_BLOCK;
    uint16_t xm  = (num_blocks == 1) ? TMODE_READ
                                      : (TMODE_READ | TMODE_MULTI | TMODE_BCEN);
    if (mmc_send_cmd(cmd, addr, NULL, xm) < 0) {
        return -1;
    }
    
    /* Read data via PIO */
    for (uint32_t block = 0; block < num_blocks; block++) {
        /* Wait for Buffer Read Ready (INT_STATUS bit 5 = 0x20).
         * boot166: use wall-clock ms timeout — the old iteration-count loop
         * (~0.07 ms) expired before the buffer was ready at any clock speed.
         * 200 ms is generous; at 25 MHz a 512-byte block arrives in ~0.16 ms. */
        uint32_t brr_deadline = mmc_time_ms() + 200u;
        while (!(sdhci_read(SDHCI_INT_STATUS) & 0x20u)) {
            if (mmc_time_ms() > brr_deadline) {
                debug_print("MMC: Read timeout (BRR not set)\n");
                return -1;
            }
            /* Also abort on error interrupt */
            if (sdhci_read(SDHCI_INT_STATUS) & 0x8000u) {
                debug_print("MMC: Read error (INT_STATUS error bit)\n");
                sdhci_write(SDHCI_INT_STATUS, 0xFFFFFFFFu);
                return -1;
            }
        }

        /* Read 512 bytes (128 × 32-bit words) */
        for (int i = 0; i < 128; i++) {
            *buf++ = sdhci_read(SDHCI_BUFFER);
        }

        /* Clear Buffer Read Ready */
        sdhci_write(SDHCI_INT_STATUS, 0x20u);
    }

    /* Wait for Transfer Complete (INT_STATUS bit 1 = 0x02) */
    uint32_t tc_deadline = mmc_time_ms() + 500u;
    while (!(sdhci_read(SDHCI_INT_STATUS) & 0x02u)) {
        if (mmc_time_ms() > tc_deadline) {
            debug_print("MMC: Transfer complete timeout\n");
            return -1;
        }
    }
    sdhci_write(SDHCI_INT_STATUS, 0x02u);  /* Clear transfer complete */
    
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
    
    /* Same combined 32-bit write as mmc_read_blocks — see comment there */
    sdhci_write(SDHCI_BLOCK_SIZE,
                ((uint32_t)num_blocks << 16) | (512 | (7 << 12)));

    /* Send write command with TRANSFER_MODE:
     *   direction bit clear — host→card (write)
     *   TMODE_MULTI + TMODE_BCEN for multi-block */
    uint32_t cmd = (num_blocks == 1) ? CMD24_WRITE_SINGLE_BLOCK : CMD25_WRITE_MULTIPLE_BLOCK;
    uint16_t xm  = (num_blocks == 1) ? 0 : (TMODE_MULTI | TMODE_BCEN);
    if (mmc_send_cmd(cmd, addr, NULL, xm) < 0) {
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
