/*
 * bluetooth.h - Bluetooth definitions for Phoenix
 * Adapted from BTSdioTypeA
 */

#ifndef BLUETOOTH_H
#define BLUETOOTH_H

#include <stdint.h>
#include <stddef.h>

/* HCI packet types */
#define HCI_COMMAND_PKT    1
#define HCI_ACL_DATA_PKT   2
#define HCI_SCO_DATA_PKT   3
#define HCI_EVENT_PKT      4

/* Ring buffer sizes */
#define RX_RING_SIZE       32768
#define LOG_RING_SIZE      65536
#define SERIAL_RING_SIZE   8192
#define TX_RING_SIZE       8192

/* Ring buffer structure */
typedef struct ring_buffer {
    uint8_t *data;
    size_t   size;
    size_t   head, tail;
} ring_buffer_t;

/* SDIO function stub (will be defined in sdio driver) */
typedef struct sdio_func sdio_func_t;

/* Bluetooth private data */
typedef struct bt_priv {
    sdio_func_t *func;          /* SDIO function */
    void        *irq_handle;    /* IRQ handler */
    uint16_t    acl_handle;     /* ACL connection handle */
    uint16_t    l2cap_local_cid;
    uint16_t    l2cap_remote_cid;
    uint8_t     rfcomm_dlci;
    uint8_t     rfcomm_state;   /* 0=idle 1=l2cap 2=control 3=data */
    uint8_t     pairing_mode;   /* 0=none, 1=Just-Works, 2=Legacy PIN */
    char        pin_code[16];
    uint8_t     remote_bd_addr[6];
} bt_priv_t;

/* Ring buffer operations */
void ring_init(ring_buffer_t *r, size_t sz);
size_t ring_used(const ring_buffer_t *r);
void ring_write(ring_buffer_t *r, const uint8_t *d, size_t len);
size_t ring_copy_out(ring_buffer_t *r, uint8_t *dst, size_t len, size_t *tail);

/* Bluetooth initialization */
int bluetooth_init(void);

/* Firmware download */
int bt_download_firmware(bt_priv_t *priv, const uint8_t *firmware, size_t fw_size);

#endif /* BLUETOOTH_H */
