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
#include <string.h>

extern void uart_puts(const char *s);

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
 */
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
    memcpy(cbw.cmd, cdb, cdb_len);

    /* Phase 1: Command */
    if (usb_bulk_transfer(drive->bulk_out, &cbw, 31, USB_TIMEOUT) < 0) {
        uart_puts("[MSC] BOT: CBW send failed\n");
        return -1;
    }

    /* Phase 2: Data (optional) */
    if (buf_len && buf) {
        usb_endpoint_t *ep = dir_in ? drive->bulk_in : drive->bulk_out;
        if (usb_bulk_transfer(ep, buf, buf_len, USB_TIMEOUT) < 0) {
            uart_puts("[MSC] BOT: data phase failed\n");
            return -1;
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

    return (csw.status == 0) ? 0 : -1;
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

/* ── blockdev_ops_t callbacks (multi-block: lba + count) ─────────────────── */
static ssize_t usb_bdev_read(blockdev_t *bdev, uint64_t lba,
                              uint32_t count, void *buf)
{
    usb_bdev_priv_t *p = (usb_bdev_priv_t *)bdev->private;
    if (usb_bot_rw(p->drive, p->lun, lba, count, buf, 0) < 0)
        return -1;
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

    /* Issue READ CAPACITY(10) to LUN 0 to get sector count and block size   */
    if (bot_read_capacity(drive, 0) < 0) {
        uart_puts("[MSC] READ CAPACITY failed — device not ready?\n");
        /* Fall back to safe defaults so the drive still registers           */
        drive->capacity[0]   = 0;
        drive->block_size[0] = 512;
    }

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
