/*
 * bluetooth.h – Bluetooth Headers for RISC OS Phoenix
 * Defines structures for BTSdioTypeA driver (firmware, pairing, SPP)
 * Author: R Andrews Grok 4 – 04 Feb 2026
 */

#ifndef BLUETOOTH_H
#define BLUETOOTH_H

#include <stdint.h>
#include "vfs.h"

#define HCI_COMMAND_PKT    1
#define HCI_ACL_DATA_PKT   2
#define HCI_SCO_DATA_PKT   3
#define HCI_EVENT_PKT      4

#define RX_RING_SIZE     32768
#define LOG_RING_SIZE    65536
#define SERIAL_RING_SIZE 8192
#define TX_RING_SIZE     8192

typedef struct ring_buffer {
    uint8_t *data;
    size_t   size;
    size_t   head, tail;
} ring_buffer_t;

typedef struct open_handle {
    size_t   rx_tail, log_tail, serial_tail;
    ring_buffer_t tx_ring;
    int      is_log, is_serial;
} open_handle_t;

typedef struct bt_priv {
    sdio_func_t *func;          // SDIO function
    void        *irq_handle;
    uint16_t    acl_handle;
    uint16_t    l2cap_local_cid, l2cap_remote_cid;
    uint8_t     rfcomm_dlci, rfcomm_state;  /* 0=idle 1=l2cap 2=control 3=data */
    uint8_t     pairing_mode;               /* 0=none, 1=Just-Works, 2=Legacy PIN */
    char        pin_code[16];               /* User-provided PIN for legacy */
    uint8_t     remote_bd_addr[6];          /* Remote device address during pairing */
} bt_priv_t;

typedef struct sdio_device_id {
    uint16_t vendor;
    uint16_t device;
    uint16_t subvendor;
    uint16_t subdevice;
} sdio_device_id_t;

typedef struct sdio_driver {
    const char *name;
    const sdio_device_id_t *id_table;
    _kernel_oserror *(*probe)(sdio_func_t *func);
    void (*remove)(sdio_func_t *func);
} sdio_driver_t;

extern const sdio_device_id_t bt_id_table[];
extern sdio_driver_t bt_driver;

void ring_init(ring_buffer_t *r, size_t sz);
size_t ring_used(const ring_buffer_t *r);
void ring_write(ring_buffer_t *r, const uint8_t *d, size_t len);
size_t ring_copy_out(ring_buffer_t *r, uint8_t *dst, size_t len, size_t *tail);

_kernel_oserror *bt_device_entry(_kernel_swi_regs *r, void *pw);

#endif /* BLUETOOTH_H */