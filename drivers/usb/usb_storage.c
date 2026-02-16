/*
 * usb_storage.c – Full 64-bit USB Mass Storage Driver for RISC OS Phoenix
 * Supports USB 3.2 Gen 2x2 (20 Gbps) + full UASP (command queuing, streams, multi-stream)
 * Falls back to BOT if UASP not supported
 * Author: R Andrews Grok 4 – 04 Feb 2026
 */

#include "kernel.h"
#include "usb.h"
#include "blockdev.h"
// // #include <string.h> /* removed - use kernel.h */ /* removed - use kernel.h */

#define USB_MAX_LUN         8
#define USB_TIMEOUT         5000
#define UASP_MAX_STREAMS    256
#define UASP_TAG_BASE       0x1000
#define UASP_STREAMS_PER_PIPE 16  // Example – adjust based on hardware

/* USB Mass Storage Class */
#define USB_CLASS_MSC       0x08
#define USB_SUBCLASS_SCSI   0x06
#define USB_PROTOCOL_BULK   0x50    // BOT
#define USB_PROTOCOL_UASP   0x62    // UASP

/* CBW/Csw for BOT */
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

/* UASP Information Units (IU) */
#define UAS_IU_COMMAND      0x01
#define UAS_IU_SENSE        0x03
#define UAS_IU_RESPONSE     0x04
#define UAS_IU_TASK_MGMT    0x05
#define UAS_IU_READ_READY   0x06
#define UAS_IU_WRITE_READY  0x07

#pragma pack(1)
typedef struct {
    uint8_t  id;           // IU ID
    uint8_t  reserved[2];
    uint8_t  tag;          // Stream tag
    uint8_t  lun;
    uint8_t  cmd_len;
    uint8_t  task_prio;
    uint8_t  cmd[16];      // SCSI command
} uas_cmd_iu_t;

typedef struct {
    uint8_t  id;
    uint8_t  reserved[2];
    uint8_t  tag;
    uint8_t  status;
    uint8_t  reserved2[3];
    uint16_t sense_len;
    uint8_t  sense_data[18];
} uas_sense_iu_t;

typedef struct {
    uint8_t  id;
    uint8_t  reserved[2];
    uint8_t  tag;
    uint8_t  status;
    uint8_t  reserved2[3];
    uint32_t residue;
} uas_response_iu_t;
#pragma pack()

typedef struct usb_storage {
    usb_device_t    *dev;
    usb_interface_t *intf;
    usb_endpoint_t  *bulk_in;
    usb_endpoint_t  *bulk_out;
    usb_endpoint_t  *status_in;     // UASP status pipe
    usb_endpoint_t  *cmd_out;       // UASP command pipe
    uint8_t          intf_num;
    uint8_t          lun_count;
    uint64_t         capacity[USB_MAX_LUN];
    blockdev_t      *bdev[USB_MAX_LUN];
    int              uasp;           // 1 = UASP, 0 = BOT
    uint16_t         next_tag;
    uint16_t         max_streams;
    spinlock_t       lock;
} usb_storage_t;

static usb_storage_t *usb_drives[16];
static int usb_drive_count = 0;

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

    cbw.signature