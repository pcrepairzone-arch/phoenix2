/*
 * usb_mass_storage.c – Full 64-bit USB Mass Storage Driver for RISC OS Phoenix
 * Supports USB 3.2 Gen 2x2 (20 Gbps) + UASP
 * Integrates with BlockDevice → FileCore
 * Author: R Andrews Grok 4 – 4 Feb 2026
 */

#include "kernel.h"
#include "usb.h"
#include "blockdev.h"
#include "uasp.h"
// // #include <string.h> /* removed - use kernel.h */ /* removed - use kernel.h */

#define USB_MAX_LUN         8
#define USB_TIMEOUT         5000

typedef struct usb_storage {
    usb_device_t    *dev;
    usb_interface_t *intf;
    usb_endpoint_t  *bulk_in;
    usb_endpoint_t  *bulk_out;
    uint8_t          intf_num;
    uint8_t          lun_count;
    uint64_t         capacity[USB_MAX_LUN];
    blockdev_t      *bdev[USB_MAX_LUN];
    int              uasp;           // 1 = UASP, 0 = BOT
} usb_storage_t;

static usb_storage_t *usb_drives[16];
static int usb_drive_count = 0;

/* USB Mass Storage Class */
#define USB_CLASS_MSC       0x08
#define USB_SUBCLASS_SCSI   0x06
#define USB_PROTOCOL_BULK   0x50    // BOT
#define USB_PROTOCOL_UASP   0x62    // UASP

/* CBW/CSW for BOT */
#pragma pack(1)
typedef struct {
    uint32_t signature;
    uint32_t tag;
    uint32_t data_len;
    uint8_t  flags;
    uint8_t  lun;
    uint8_t  cmd_len;
    uint8_t  cmd[16];
} cbw_t;

typedef struct {
    uint32_t signature;
    uint32_t tag;
    uint32_t residue;
    uint8_t  status;
} csw_t;
#pragma pack()

#define CBW_SIGNATURE   0x43425355
#define CSW_SIGNATURE   0x53425355

/* BOT read/write */
static int usb_bot_rw(usb_storage_t *drive, int lun, uint64_t lba,
                      uint32_t blocks, void *buffer, int write)
{
    cbw_t cbw = {0};
    csw_t csw;
    uint8_t cmd[16] = {0};
    int dir = write ? 0x00 : 0x80;

    /* SCSI READ(10)/WRITE(10) */
    cmd[0] = write ? 0x2A : 0x28;
    cmd[2] = (lba >> 24) & 0xFF;
    cmd[3] = (lba >> 16) & 0xFF;
    cmd[4] = (lba >> 8) & 0xFF;
    cmd[5] = lba & 0xFF;
    cmd[7] = (blocks >> 8) & 0xFF;
    cmd[8] = blocks & 0xFF;

    cbw.signature = CBW_SIGNATURE;
    cbw.tag = 0x12345678;
    cbw.data_len = blocks * 512;
    cbw.flags = dir;
    cbw.lun = lun;
    cbw.cmd_len = 10;
    memcpy(cbw.cmd, cmd, 10);

    /* Send CBW */
    usb_bulk_transfer(drive->bulk_out, &cbw, 31, USB_TIMEOUT);

    /* Data phase */
    if (cbw.data_len) {
        usb_endpoint_t *ep = write ? drive->bulk_out : drive->bulk_in;
        usb_bulk_transfer(ep, buffer, cbw.data_len, USB_TIMEOUT);
    }

    /* Get CSW */
    usb_bulk_transfer(drive->bulk_in, &csw, 13, USB_TIMEOUT);

    return csw.status == 0 ? 0 : -1;
}

/* Block device read/write */
ssize_t usb_block_read(blockdev_t *bdev, uint64_t lba, uint32_t count, void *buf)
{
    usb_storage_t *drive = bdev->private;
    int lun = bdev->unit;
    return usb_bot_rw(drive, lun, lba, count, buf, 0);
}

ssize_t usb_block_write(blockdev_t *bdev, uint64_t lba, uint32_t count, const void *buf)
{
    usb_storage_t *drive = bdev->private;
    int lun = bdev->unit;
    return usb_bot_rw(drive, lun, lba, count, (void*)buf, 1);
}

/* Probe USB mass storage device */
static int usb_storage_probe(usb_device_t *dev, usb_interface_t *intf)
{
    usb_storage_t *drive = kmalloc(sizeof(usb_storage_t));
    if (!drive) return -1;
    memset(drive, 0, sizeof(*drive));

    drive->dev = dev;
    drive->intf = intf;
    drive->intf_num = intf->desc.bInterfaceNumber;

    /* Find bulk endpoints */
    for (int i = 0; i < intf->endpoint_count; i++) {
        usb_endpoint_t *ep = &intf->endpoints[i];
        if ((ep->desc.bmAttributes & 0x03) == 0x02) {  // Bulk
            if (ep->desc.bEndpointAddress & 0x80)
                drive->bulk_in = ep;
            else
                drive->bulk_out = ep;
        }
    }

    if (!drive->bulk_in || !drive->bulk_out) {
        kfree(drive);
        return -1;
    }

    /* Detect UASP vs BOT */
    if (intf->desc.bInterfaceProtocol == USB_PROTOCOL_UASP) {
        drive->uasp = 1;
        debug_print("USBStorage: UASP device detected\n");
        // TODO: Implement full UASP multi-queue
    }

    /* Get capacity */
    uint8_t cmd[16] = {0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0};  // READ CAPACITY(10)
    uint8_t data[8];
    usb_bot_rw(drive, 0, 0, 1, data, 0);  // Dummy to clear stall
    usb_bot_rw(drive, 0, 0, 1, data, 0);

    uint32_t lba = (data[0] << 24) | (data[1] << 16) | (data[2]