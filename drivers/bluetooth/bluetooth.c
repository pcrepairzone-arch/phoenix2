/*
 * bluetooth.c – BTSdioTypeA Bluetooth Driver for RISC OS Phoenix
 * Full Classic Bluetooth stack with SPP, Just-Works/legacy PIN pairing, and co-existence
 * Author: R Andrews Grok 4 – 04 Feb 2026
 */

#include "kernel.h"
#include "swis.h"
#include "SDIODriver.h"
#include "DeviceFS.h"
#include "bluetooth.h"
#include <stdlib.h>
// // #include <string.h> /* removed - use kernel.h */ /* removed - use kernel.h */
#include <stdarg.h>

#define MODULE_TITLE     "BTSdioTypeA"
#define MODULE_VERSION   "1.40"
#define FIRMWARE_PATH    "Resources:Bluetooth.BCM4345C0.hcd"

typedef struct open_handle open_handle_t;
typedef struct bt_priv bt_priv_t;
typedef struct ring_buffer ring_buffer_t;

static bt_priv_t *g_priv = NULL;
static ring_buffer_t rx_ring, log_ring, serial_ring;
static uint16_t next_cid = 0x0040;
static int debug_enabled = 0;

static const sdio_device_id_t bt_id_table[] = {
    { 0x02d0, 0xa9a6, SDIO_ANY_ID, SDIO_ANY_ID },
    { 0x02d0, 0xa94d, SDIO_ANY_ID, SDIO_ANY_ID },
    { 0x04b4, 0xb028, SDIO_ANY_ID, SDIO_ANY_ID },
    { 0, 0, 0, 0 }
};

/* Ring buffer functions */
static void ring_init(ring_buffer_t *r, size_t sz) {
    r->data = malloc(sz); 
    if (!r->data) return;
    r->size = sz; 
    r->head = r->tail = 0;
}

static size_t ring_used(const ring_buffer_t *r) { 
    return r->head - r->tail; 
}

static void ring_write(ring_buffer_t *r, const uint8_t *d, size_t len) {
    if (!r->data) return;
    size_t pos = r->head % r->size, first = r->size - pos;
    if (len > first) {
        memcpy(r->data + pos, d, first);
        memcpy(r->data, d + first, len - first);
    } else memcpy(r->data + pos, d, len);
    r->head += len;
}

static size_t ring_copy_out(ring_buffer_t *r, uint8_t *dst, size_t len, size_t *tail) {
    if (!r->data || !dst) return 0;
    size_t avail = r->head - *tail, cp = len < avail ? len : avail;
    if (!cp) return 0;
    size_t pos = *tail % r->size, first = r->size - pos;
    if (cp > first) {
        memcpy(dst, r->data + pos, first);
        memcpy(dst + first, r->data, cp - first);
    } else memcpy(dst, r->data + pos, cp);
    *tail += cp; 
    return cp;
}

/* Debug print */
static void debug_print(const char *fmt, ...) {
    if (!debug_enabled) return;
    char buf[512]; va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
    const char *p = buf; while (*p) _kernel_oswrch(*p++);
}

/* Firmware download – full Intel-HEX */
static int hexbyte(const char *s) {
    int a = (s[0]>='A') ? s[0]-'A'+10 : s[0]-'0';
    int b = (s[1]>='A') ? s[1]-'A'+10 : s[1]-'0';
    return (a<<4)|b;
}

static _kernel_oserror *bt_download_firmware(bt_priv_t *priv) {
    _kernel_oserror *err; int h, size, read; char *buf;
    uint32_t extended = 0;

    err = _swix(OS_Find, _INR(0,1)|_OUT(1), 0x40, FIRMWARE_PATH, &h);
    if (err || !h) return err ? err : _kernel_error_lookup(0x10000, "No firmware");

    _swix(OS_Args, _INR(0,1), 0, h, &size);
    buf = malloc(size + 1);
    if (!buf) { _swix(OS_Find, _IN(0), 0, h); return NULL; }
    _swix(OS_GBPB, _INR(0,4), 10, h, buf, size, 0, &read);
    _swix(OS_Find, _IN(0), 0, h);
    if (read <= 0) { free(buf); return ERR_BAD_FILE; }
    buf[read] = 0;

    uint8_t minidrv[] = {0x01,0x2e,0xfc,0x00};
    uint8_t hdr[4] = {4,0,0,0x01};
    _swix(SDIODriver_WriteBytes, _INR(0,5), priv->func, 0, hdr, 4, SDIO_INCREMENT_ADDRESS);
    _swix(SDIODriver_WriteBytes, _INR(0,5), priv->func, 0, minidrv, 4, SDIO_INCREMENT_ADDRESS);

    char *p = buf;
    while (*p) {
        if (*p++ != ':') continue;
        uint8_t len = hexbyte(p); p+=2;
        uint16_t off = hexbyte(p)<<8 | hexbyte(p+2); p+=4;
        uint8_t type = hexbyte(p); p+=2;
        if (type == 4) { extended = hexbyte(p)<<24 | hexbyte(p+2)<<16; p+=4; continue; }
        if (type != 0) { while (*p && *p!='\n') p++; if (*p) p++; continue; }
        uint32_t addr = extended | off;
        int pos = 0;
        while (pos < len) {
            int chunk = len - pos; if (chunk > 248) chunk = 248;
            uint8_t plen = 5 + chunk;
            uint8_t cmd[9+248];
            cmd[0]=0x01; cmd[1]=0x17; cmd[2]=0xfc; cmd[3]=plen;
            cmd[4]=0; cmd