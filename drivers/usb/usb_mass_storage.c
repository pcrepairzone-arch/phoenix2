/*
 * usb_mass_storage.c – USB Mass Storage (BOT) Driver for RISC OS Phoenix
 * Supports SCSI Transparent Command Set over Bulk-Only Transport (BOT)
 * Integrates with BlockDevice → FileCore
 * Author: R Andrews – 4 Feb 2026
 */

#include "kernel.h"
#include "usb.h"
#include "blockdriver.h"
#include "uasp.h"

extern void uart_puts(const char *s);
/* boot297: quiet mode for xHCI timeout log during TUR retries */
extern void xhci_set_quiet_timeouts(int q);

static void print_hex32(uint32_t v) {
    static const char h[] = "0123456789abcdef";
    char buf[11] = "0x";
    for (int i = 9; i >= 2; i--) { buf[i] = h[v & 0xF]; v >>= 4; }
    buf[10] = '\0';
    uart_puts(buf);
}

static void print_hex64(uint64_t v) {
    /* Print all 16 hex digits with no "0x" prefix — caller adds prefix.
     * Previously called print_hex32 for the high half which added its own
     * "0x", producing doubled "0x0x..." in the output (boot161 bug).     */
    static const char h[] = "0123456789abcdef";
    char buf[17];
    for (int i = 15; i >= 0; i--) { buf[i] = h[v & 0xF]; v >>= 4; }
    buf[16] = '\0';
    uart_puts(buf);
}

#define USB_MAX_LUN         8
#define USB_TIMEOUT         5000

/* ── usb_storage_t ────────────────────────────────────────────────────────── */
typedef struct usb_storage {
    usb_device_t    *dev;
    usb_interface_t *intf;
    usb_endpoint_t  *bulk_in;
    usb_endpoint_t  *bulk_out;
    uint8_t          intf_num;
    uint8_t          lun_count;
    uint64_t         capacity[USB_MAX_LUN];   /* sectors per LUN              */
    uint32_t         block_size[USB_MAX_LUN]; /* bytes per sector (from RC10) */
    blockdev_t      *bdev[USB_MAX_LUN];       /* registered blockdev_t        */
} usb_storage_t;

static usb_storage_t *usb_drives[16];
static int usb_drive_count = 0;

/* Private data stored in blockdev->private */
typedef struct {
    usb_storage_t *drive;
    int            lun;
} usb_bdev_priv_t;

/* USB Mass Storage constants */
#define USB_CLASS_MSC       0x08
#define USB_SUBCLASS_SCSI   0x06
#define USB_PROTOCOL_BULK   0x50    /* BOT  */
#define USB_PROTOCOL_UASP   0x62    /* UASP */

/* ── CBW / CSW structures ─────────────────────────────────────────────────── */
#pragma pack(1)
typedef struct {
    uint32_t signature;   /* 0x43425355 "USBC" */
    uint32_t tag;
    uint32_t data_len;
    uint8_t  flags;       /* 0x80 = IN, 0x00 = OUT */
    uint8_t  lun;
    uint8_t  cmd_len;
    uint8_t  cmd[16];
} cbw_t;

typedef struct {
    uint32_t signature;   /* 0x53425355 "USBS" */
    uint32_t tag;
    uint32_t residue;
    uint8_t  status;      /* 0 = good, 1 = failed, 2 = phase error */
} csw_t;
#pragma pack()

#define CBW_SIGNATURE   0x43425355u
#define CSW_SIGNATURE   0x53425355u

/* ── Generic BOT SCSI command ─────────────────────────────────────────────── *
 * Handles any SCSI CDB over BOT: CBW → [data phase] → CSW.
 * dir_in=1 for device→host data (e.g. READ CAPACITY, READ),
 * dir_in=0 for host→device (e.g. WRITE), buf=NULL/buf_len=0 for no data.
 * Returns 0 on success (CSW status=0), -1 on failure.
 *
 * boot184 fix: when the data-IN phase stalls (CC=6), BOT spec §6.6.2 requires
 * the host to: (1) clear the ENDPOINT_HALT on bulk-IN, then (2) read the CSW.
 * Previously we returned -1 immediately, leaving the pending CSW in the pipe.
 * Every subsequent CBW was then interpreted by the device as arriving while a
 * CSW was outstanding → phase error → device stalls again.  Now we always
 * attempt to clear the stall and drain the CSW before returning, leaving the
 * BOT state machine clean for the next command.                               */
static int bot_scsi_cmd(usb_storage_t *drive, int lun,
                        const uint8_t *cdb, uint8_t cdb_len,
                        void *buf, uint32_t buf_len, int dir_in)
{
    cbw_t cbw = {0};
    csw_t csw = {0};

    cbw.signature = CBW_SIGNATURE;
    cbw.tag       = 0xC0DEC0DEu;
    cbw.data_len  = buf_len;
    cbw.flags     = dir_in ? 0x80u : 0x00u;
    cbw.lun       = (uint8_t)lun;
    cbw.cmd_len   = cdb_len;
    for (int _i = 0; _i < cdb_len && _i < 16; _i++)
        cbw.cmd[_i] = cdb[_i];

    /* Phase 1: Command */
    if (usb_bulk_transfer(drive->bulk_out, &cbw, 31, USB_TIMEOUT) < 0) {
        uart_puts("[MSC] BOT: CBW send failed\n");
        return -1;
    }

    /* Phase 2: Data (optional) */
    int data_stalled = 0;
    if (buf_len && buf) {
        usb_endpoint_t *ep = dir_in ? drive->bulk_in : drive->bulk_out;
        if (usb_bulk_transfer(ep, buf, buf_len, USB_TIMEOUT) < 0) {
            /* Data phase stalled.  Per BOT spec §6.6.2:
             * - Clear ENDPOINT_HALT on the stalled pipe
             * - Then proceed to read the CSW
             * NOT returning -1 here keeps the pipe clean for the next command. */
            data_stalled = 1;
            uart_puts("[MSC] BOT: data phase stalled — clearing halt\n");
            if (dir_in) {
                usb_control_transfer(drive->dev,
                    0x02u,   /* bmRequestType: std | ep | host→dev */
                    0x01u,   /* bRequest: CLEAR_FEATURE             */
                    0x0000u, /* wValue: ENDPOINT_HALT               */
                    drive->bulk_in->bEndpointAddress,
                    NULL, 0, 200);
            } else {
                usb_control_transfer(drive->dev,
                    0x02u, 0x01u, 0x0000u,
                    drive->bulk_out->bEndpointAddress,
                    NULL, 0, 200);
            }
            /* Fall through to Phase 3 to drain the CSW */
        }
    }

    /* Phase 3: Status */
    if (usb_bulk_transfer(drive->bulk_in, &csw, 13, USB_TIMEOUT) < 0) {
        uart_puts("[MSC] BOT: CSW recv failed\n");
        return -1;
    }
    if (csw.signature != CSW_SIGNATURE) {
        uart_puts("[MSC] BOT: bad CSW signature\n");
        return -1;
    }

    if (data_stalled) return -1;   /* command failed; pipe is now clean */
    return (csw.status == 0) ? 0 : -1;
}

/* ── Wall-clock helpers (CNTPCT_EL0 @ 54 MHz on BCM2711) ────────────────── */
static inline uint32_t msc_time_ms(void) {
    uint64_t v;
    asm volatile("isb; mrs %0, cntpct_el0" : "=r"(v) :: "memory");
    return (uint32_t)(v / 54000ULL);
}

static void msc_delay_ms(uint32_t ms) {
    uint32_t deadline = msc_time_ms() + ms;
    while (msc_time_ms() < deadline)
        asm volatile("nop");
}

/* ── SCSI TEST UNIT READY (opcode 0x00, no data phase) ───────────────────── *
 * Returns 0 if device reports GOOD status, -1 if CHECK CONDITION / timeout.
 */
static int bot_test_unit_ready(usb_storage_t *drive, int lun)
{
    uint8_t cdb[6] = {0x00, 0, 0, 0, 0, 0};   /* TEST UNIT READY */
    return bot_scsi_cmd(drive, lun, cdb, 6, NULL, 0, 1);
}

/* ── SCSI READ CAPACITY(10) ───────────────────────────────────────────────── *
 * CDB 0x25 — returns 8 bytes big-endian: [Last LBA 4B][Block Size 4B]
 * Fills drive->capacity[lun] and drive->block_size[lun].
 * Returns 0 on success, -1 on failure.
 */
static int bot_read_capacity(usb_storage_t *drive, int lun)
{
    uint8_t cdb[10] = {0};
    cdb[0] = 0x25;              /* READ CAPACITY(10) */

    uint8_t data[8] = {0};
    if (bot_scsi_cmd(drive, lun, cdb, 10, data, 8, 1) < 0)
        return -1;

    uint32_t last_lba = ((uint32_t)data[0] << 24) |
                        ((uint32_t)data[1] << 16) |
                        ((uint32_t)data[2] <<  8) |
                         (uint32_t)data[3];

    uint32_t blk_len  = ((uint32_t)data[4] << 24) |
                        ((uint32_t)data[5] << 16) |
                        ((uint32_t)data[6] <<  8) |
                         (uint32_t)data[7];

    /* Guard against clearly wrong values */
    if (blk_len == 0 || blk_len > 65536) {
        uart_puts("[MSC] READ CAPACITY: bad block length, defaulting to 512\n");
        blk_len = 512;
    }

    drive->capacity[lun]   = (uint64_t)last_lba + 1;
    drive->block_size[lun] = blk_len;
    return 0;
}

/* ── SCSI READ CAPACITY(16) ─────────────────────────────────────────────────
 * SERVICE ACTION IN (0x9E) / READ CAPACITY (SA 0x10)
 * Returns 32 bytes: [Last LBA 8B big-endian][Block Size 4B big-endian]...
 * Required for drives > 2 TiB and by USB-SATA bridges (RTL9210 etc) that
 * stall RC(10) to signal they need the 16-byte variant.
 * Returns 0 on success, -1 on failure.
 */
static int bot_read_capacity16(usb_storage_t *drive, int lun)
{
    uint8_t cdb[16] = {0};
    cdb[0]  = 0x9Eu;   /* SERVICE ACTION IN(16) */
    cdb[1]  = 0x10u;   /* READ CAPACITY service action */
    cdb[10] = 0u;      /* Logical Block Address bytes 2-9 = 0 */
    cdb[11] = 0u;
    cdb[12] = 0u;
    cdb[13] = 32u;     /* Allocation length = 32 bytes */
    cdb[14] = 0u;      /* PMI = 0 */
    cdb[15] = 0u;

    uint8_t data[32] = {0};
    if (bot_scsi_cmd(drive, lun, cdb, 16, data, 32, 1) < 0)
        return -1;

    uint64_t last_lba =
        ((uint64_t)data[0] << 56) | ((uint64_t)data[1] << 48) |
        ((uint64_t)data[2] << 40) | ((uint64_t)data[3] << 32) |
        ((uint64_t)data[4] << 24) | ((uint64_t)data[5] << 16) |
        ((uint64_t)data[6] <<  8) |  (uint64_t)data[7];

    uint32_t blk_len =
        ((uint32_t)data[8]  << 24) | ((uint32_t)data[9]  << 16) |
        ((uint32_t)data[10] <<  8) |  (uint32_t)data[11];

    if (blk_len == 0 || blk_len > 65536) {
        uart_puts("[MSC] RC(16): bad block length, defaulting to 512\n");
        blk_len = 512;
    }

    uart_puts("[MSC] RC(16): last_lba=0x"); print_hex64(last_lba);
    uart_puts("  blk_len="); print_hex32(blk_len); uart_puts("\n");

    drive->capacity[lun]   = last_lba + 1ULL;
    drive->block_size[lun] = blk_len;
    return 0;
}

/* ── SCSI READ(10) / WRITE(10) over BOT ───────────────────────────────────── */
static int usb_bot_rw(usb_storage_t *drive, int lun, uint64_t lba,
                      uint32_t blocks, void *buffer, int write)
{
    uint32_t bsize = drive->block_size[lun];
    if (bsize == 0) bsize = 512;  /* safety fallback */

    uint8_t cdb[10] = {0};
    cdb[0] = write ? 0x2Au : 0x28u;   /* WRITE(10) : READ(10) */
    cdb[2] = (uint8_t)(lba >> 24);
    cdb[3] = (uint8_t)(lba >> 16);
    cdb[4] = (uint8_t)(lba >>  8);
    cdb[5] = (uint8_t) lba;
    cdb[7] = (uint8_t)(blocks >> 8);
    cdb[8] = (uint8_t) blocks;

    return bot_scsi_cmd(drive, lun, cdb, 10,
                        buffer, blocks * bsize, !write);
}

/* ── BOT error recovery ────────────────────────────────────────────────────── *
 * Called when a bulk transfer fails with CC=4 (USB Transaction Error).        *
 * Sequence per USB MSC BOT spec §5.3.4:                                       *
 *   1. BOT Mass Storage Reset (class-specific control request, via EP0)        *
 *   2. CLEAR_FEATURE ENDPOINT_HALT on bulk IN endpoint (standard, via EP0)    *
 *   3. CLEAR_FEATURE ENDPOINT_HALT on bulk OUT endpoint (standard, via EP0)   *
 *   4. xHCI endpoint reset + re-configure (xhci_ep_recover)                   *
 *   5. TEST UNIT READY to verify device responsiveness                         *
 * ──────────────────────────────────────────────────────────────────────────── */
extern int xhci_ep_recover(usb_device_t *dev);

static int bot_recover(usb_storage_t *drive)
{
    uart_puts("[MSC] BOT recovery: resetting device endpoints...\n");

    /* Step 1: BOT Mass Storage Reset
     * bmRequestType = 0x21 (out, class, interface)
     * bRequest      = 0xFF  */
    usb_control_transfer(drive->dev,
                         0x21u,          /* bmRequestType: class | interface | host→dev */
                         0xFFu,          /* bRequest: Bulk-Only Mass Storage Reset      */
                         0u,             /* wValue: 0                                   */
                         drive->intf_num,/* wIndex: interface number                    */
                         NULL, 0, 500);
    msc_delay_ms(50);

    /* Step 2: Clear HALT on bulk IN endpoint
     * bmRequestType = 0x02 (out, standard, endpoint), bRequest = 0x01 (CLEAR_FEATURE)
     * wValue = 0x0000 (ENDPOINT_HALT feature selector)                              */
    usb_control_transfer(drive->dev,
                         0x02u,          /* bmRequestType: standard | endpoint | host→dev */
                         0x01u,          /* bRequest: CLEAR_FEATURE                       */
                         0x0000u,        /* wValue: ENDPOINT_HALT                         */
                         drive->bulk_in->bEndpointAddress,
                         NULL, 0, 200);

    /* Step 3: Clear HALT on bulk OUT endpoint */
    usb_control_transfer(drive->dev,
                         0x02u,
                         0x01u,
                         0x0000u,
                         drive->bulk_out->bEndpointAddress,
                         NULL, 0, 200);

    msc_delay_ms(100);

    /* Step 4: Reset xHCI endpoint contexts and rebuild transfer rings */
    int rc = xhci_ep_recover(drive->dev);
    if (rc < 0) {
        uart_puts("[MSC] BOT recovery: xhci_ep_recover failed\n");
        return -1;
    }

    msc_delay_ms(200);

    /* Step 5: Verify device is responsive with TEST UNIT READY */
    int tur_ok = 0;
    for (int i = 0; i < 5; i++) {
        if (bot_test_unit_ready(drive, 0) == 0) { tur_ok = 1; break; }
        msc_delay_ms(100);
    }
    uart_puts("[MSC] BOT recovery: ");
    uart_puts(tur_ok ? "device ready\n" : "device NOT responding\n");
    return tur_ok ? 0 : -1;
}

/* ── blockdev_ops_t callbacks (multi-block: lba + count) ─────────────────── */
static ssize_t usb_bdev_read(blockdev_t *bdev, uint64_t lba,
                              uint32_t count, void *buf)
{
    usb_bdev_priv_t *p = (usb_bdev_priv_t *)bdev->private;
    if (usb_bot_rw(p->drive, p->lun, lba, count, buf, 0) < 0) {
        /* First read failed — attempt BOT recovery and retry once */
        uart_puts("[MSC] read failed @ LBA "); print_hex32((uint32_t)lba);
        uart_puts(" — attempting BOT recovery\n");
        if (bot_recover(p->drive) == 0) {
            if (usb_bot_rw(p->drive, p->lun, lba, count, buf, 0) < 0) {
                uart_puts("[MSC] read still failed after recovery\n");
                return -1;
            }
            uart_puts("[MSC] read succeeded after recovery\n");
            return (ssize_t)count;
        }
        return -1;
    }
    return (ssize_t)count;
}

static ssize_t usb_bdev_write(blockdev_t *bdev, uint64_t lba,
                               uint32_t count, const void *buf)
{
    usb_bdev_priv_t *p = (usb_bdev_priv_t *)bdev->private;
    if (usb_bot_rw(p->drive, p->lun, lba, count, (void *)buf, 1) < 0)
        return -1;
    return (ssize_t)count;
}

static blockdev_ops_t usb_bdev_ops = {
    .read  = usb_bdev_read,
    .write = usb_bdev_write,
    .trim  = NULL,
    .poll  = NULL,
    .close = NULL,
};

/* ── Probe ────────────────────────────────────────────────────────────────── */
static int usb_storage_probe(usb_device_t *dev, usb_interface_t *intf)
{
    /* Decline UAS — not yet implemented.
     * The ASMedia ASM1153E presents both UAS (proto=0x62) and BOT (proto=0x50)
     * alternate interfaces.  Return -1 here so the framework tries the next
     * interface, which is BOT and will bind successfully.                   */
    if (intf->bInterfaceProtocol == USB_PROTOCOL_UASP) {
        uart_puts("[MSC] UAS interface declined (BOT will bind)\n");
        return -1;
    }

    usb_storage_t *drive = kmalloc(sizeof(usb_storage_t));
    if (!drive) return -1;
    memset(drive, 0, sizeof(*drive));

    drive->dev      = dev;
    drive->intf     = intf;
    drive->intf_num = intf->bInterfaceNumber;

    /* Find bulk IN and OUT endpoints */
    for (int i = 0; i < intf->endpoint_count; i++) {
        usb_endpoint_t *ep = &intf->endpoints[i];
        if ((ep->bmAttributes & 0x03) == 0x02) {   /* Bulk */
            if (ep->bEndpointAddress & 0x80)
                drive->bulk_in  = ep;
            else
                drive->bulk_out = ep;
        }
    }

    if (!drive->bulk_in || !drive->bulk_out) {
        uart_puts("[MSC] probe: no bulk endpoints found\n");
        kfree(drive);
        return -1;
    }

    /* boot168: Give slow devices (NVMe adapters, USB-SATA bridges) time to
     * initialise before the first SCSI command.  A bare 500 ms here prevents
     * the RTL9210 and similar bridges from stalling the bulk IN pipe when we
     * hit them cold with READ CAPACITY.                                      */
    msc_delay_ms(500);

    /* boot168: TEST UNIT READY retry loop.
     * Standard SCSI init order: TUR (until GOOD) → READ CAPACITY.
     * TUR has no data phase — failures come back as CSW status=1 (CHECK
     * CONDITION = not ready yet), which is a clean exchange that leaves the
     * BOT pipe uncorrupted.  We retry up to 5 s before giving up.
     * This prevents the mid-phase RC failure that left the RTL9210 pipe
     * stalled in boot167.                                                    */
    uart_puts("[MSC] TEST UNIT READY...\n");
    int tur_ok = 0;
    uint32_t tur_deadline = msc_time_ms() + 5000u;
    /* boot297: suppress [xHCI] timeout lines during TUR retries — slow devices
     * (Toshiba Mac stick, cold-start bridges) generate many expected timeouts
     * before becoming ready.  Quiet mode is cleared on exit whether or not
     * TUR succeeds, so all subsequent commands log normally.                 */
    xhci_set_quiet_timeouts(1);
    while (msc_time_ms() < tur_deadline) {
        if (bot_test_unit_ready(drive, 0) == 0) {
            tur_ok = 1;
            break;
        }
        msc_delay_ms(100);
    }
    /* boot298: keep quiet through INQUIRY and READ CAPACITY too —
     * a stuck device (failed SET_CONFIGURATION, BOT pipe locked up)
     * floods the log during all three phases.  Quiet mode is cleared
     * immediately after the RC sequence so the capacity print and all
     * subsequent operations log normally.                             */
    if (tur_ok)
        uart_puts("[MSC] Device ready\n");
    else
        uart_puts("[MSC] TUR timeout — trying READ CAPACITY anyway\n");

    /* INQUIRY — issue before READ CAPACITY.
     * Many USB-SATA bridges (RTL9210 etc.) require an INQUIRY to initialise
     * their internal SCSI emulation layer; without it they accept TUR but stall
     * any command that has a data phase (including READ CAPACITY).
     * vendor[] / product[] are also used below for media_class scoring.       */
    char msc_vendor[9]  = {0};
    char msc_product[17] = {0};
    {
        uint8_t inq_cdb[6] = {0x12, 0, 0, 0, 36, 0};
        uint8_t inq_buf[36] = {0};
        int inq_rc = bot_scsi_cmd(drive, 0, inq_cdb, 6, inq_buf, 36, 1);
        if (inq_rc == 0) {
            for (int _i = 0; _i < 8;  _i++) {
                char _c = (char)(inq_buf[8  + _i] & 0x7Fu);
                msc_vendor[_i]  = (_c >= 0x20) ? _c : ' ';
            }
            for (int _i = 0; _i < 16; _i++) {
                char _c = (char)(inq_buf[16 + _i] & 0x7Fu);
                msc_product[_i] = (_c >= 0x20) ? _c : ' ';
            }
            uart_puts("[MSC] INQUIRY OK  vendor='"); uart_puts(msc_vendor);
            uart_puts("'  product='");               uart_puts(msc_product);
            uart_puts("'\n");
        } else {
            uart_puts("[MSC] INQUIRY failed (non-fatal)\n");
        }
    }

    /* READ CAPACITY: try RC(10) first, fall back to RC(16) if stalled.
     *
     * USB-SATA bridges (RTL9210, etc.) often stall the bulk-IN pipe on
     * RC(10) to signal "use the 16-byte variant instead".  After the stall
     * the IN endpoint is halted — we must call bot_recover() to clear it
     * before attempting any further commands.
     *
     * Sequence: RC(10) → on failure → BOT recover → RC(16)
     *           → on failure → BOT recover → RC(16) retry → give up        */
    /* boot291: RC(10) first.  If it stalls, the device's SCSI state machine
     * is in phase-error (pending CSW, halted bulk-IN).  Attempting RC(16)
     * while in that state causes the CBW to be interpreted as a phase error
     * and the device (notably RTL9210 in HS-BOT mode) locks up entirely,
     * refusing even EP0 for ~8 s.  Correct sequence:
     *   RC(10) → on stall → bot_recover (BOMSR resets state machine)
     *          → RC(16)   → on stall → bot_recover → RC(10) last-resort */
    uart_puts("[MSC] READ CAPACITY(10)...\n");
    if (bot_read_capacity(drive, 0) < 0) {
        uart_puts("[MSC] RC(10) failed — BOT recover, then RC(16)\n");
        bot_recover(drive);
        msc_delay_ms(200);
        if (bot_read_capacity16(drive, 0) < 0) {
            uart_puts("[MSC] RC(16) failed — second BOT recover, last RC(10)\n");
            bot_recover(drive);
            msc_delay_ms(200);
            if (bot_read_capacity(drive, 0) < 0) {
                uart_puts("[MSC] READ CAPACITY failed — registering with 0 blocks\n");
                drive->capacity[0]   = 0;
                drive->block_size[0] = 512;
            }
        }
    }

    xhci_set_quiet_timeouts(0);   /* boot298: quiet zone ends — all commands log normally from here */
    uart_puts("[MSC] LUN 0: capacity=0x");
    print_hex64(drive->capacity[0]);
    uart_puts(" sectors, block_size=");
    print_hex32(drive->block_size[0]);
    uart_puts(" bytes\n");

    /* Build name: usb0, usb1, … (max 15 chars per blockdriver.h)           */
    char devname[5] = "usb0";
    devname[3] = (char)('0' + (usb_drive_count & 0x0F));

    /* Register with blockdriver.c — it allocates and owns the blockdev_t   */
    blockdev_t *bd = blockdev_register(devname,
                                       drive->capacity[0],
                                       drive->block_size[0]);
    if (!bd) {
        uart_puts("[MSC] blockdev_register failed\n");
        kfree(drive);
        return -1;
    }

    /* Wire up ops and private data */
    usb_bdev_priv_t *priv = kmalloc(sizeof(usb_bdev_priv_t));
    if (!priv) { kfree(drive); return -1; }
    priv->drive = drive;
    priv->lun   = 0;
    bd->private = priv;
    bd->ops     = &usb_bdev_ops;

    /* ── Media class classification ─────────────────────────────────────────
     * VID/PID first (definitive); INQUIRY product string as fallback.
     * Used by FileCore disc scoring: NVMe > SSD > USB-Flash > SD.            */
    {
        media_class_t mc   = MEDIA_USB_FLASH;
        uint16_t      vid  = dev->idVendor;
        uint16_t      pid  = dev->idProduct;

        /* Known USB-NVMe bridges */
        if      (vid == 0x0bdau && pid == 0x9210u) mc = MEDIA_NVME; /* RTL9210  */
        else if (vid == 0x152du && pid == 0x0583u) mc = MEDIA_NVME; /* JMS583   */
        else if (vid == 0x2109u && pid == 0x0715u) mc = MEDIA_NVME; /* VL716    */
        /* Known USB-SATA SSD enclosures */
        else if (vid == 0x174cu && pid == 0x55aau) mc = MEDIA_SSD;  /* ASM1351  */
        else if (vid == 0x152du && pid == 0x0578u) mc = MEDIA_SSD;  /* JMS580   */
        else if (vid == 0x0080u && pid == 0x0578u) mc = MEDIA_SSD;  /* Sabrent  */
        else {
            /* INQUIRY product string heuristic — simple substring search */
            static const char * const nvme_keys[] = { "NVMe","NVME","NVM",NULL };
            static const char * const ssd_keys[]  = { "SSD","M.2",NULL };
            int matched = 0;
            for (int _k = 0; nvme_keys[_k] && !matched; _k++) {
                const char *h = msc_product, *n = nvme_keys[_k];
                for (int _i = 0; h[_i] && !matched; _i++) {
                    int _j = 0;
                    while (n[_j] && h[_i+_j] == n[_j]) _j++;
                    if (!n[_j]) { mc = MEDIA_NVME; matched = 1; }
                }
            }
            for (int _k = 0; ssd_keys[_k] && !matched; _k++) {
                const char *h = msc_product, *n = ssd_keys[_k];
                for (int _i = 0; h[_i] && !matched; _i++) {
                    int _j = 0;
                    while (n[_j] && h[_i+_j] == n[_j]) _j++;
                    if (!n[_j]) { mc = MEDIA_SSD;  matched = 1; }
                }
            }
        }
        bd->media_class = mc;
        uart_puts("[MSC] media_class=");
        uart_puts(mc == MEDIA_NVME ? "NVMe" :
                  mc == MEDIA_SSD  ? "SSD"  : "USB-Flash");
        uart_puts("\n");
    }

    drive->bdev[0] = bd;

    if (usb_drive_count < 16)
        usb_drives[usb_drive_count++] = drive;

    dev->class_private = drive;

    uart_puts("[MSC] USB mass storage registered as '");
    uart_puts(bd->name);
    uart_puts("'\n");
    return 0;
}

/* ── Class driver registration ───────────────────────────────────────────── */
static usb_class_driver_t msc_driver = {
    .name       = "USB-MSC",
    .class_code = USB_CLASS_MSC,
    .probe      = usb_storage_probe,
    .disconnect = NULL,
};

int usb_mass_storage_init(void) {
    uart_puts("[MSC] Registering USB mass storage class driver\n");
    usb_register_class_driver(&msc_driver);
    return 0;
}
