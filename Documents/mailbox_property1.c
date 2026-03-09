/*
 * mailbox_property.c — VideoCore Mailbox Property Interface
 *
 * Unified wrapper around mbox_call() for all VC firmware operations.
 *
 * All functions build a property buffer, call mbox_call(), and return
 * the result.  The only hardware dependency is mbox_call() itself
 * (drivers/gpu/mailbox.c) which uses get_mailbox_base() from
 * periph_base.c.  Nothing here is Pi-model-specific.
 *
 * Buffer layout (per VC mailbox property protocol):
 *   [0]  u32  total buffer size in bytes (including this word)
 *   [1]  u32  request code (0) / response code (0x80000000 = ok)
 *   [2]  u32  tag ID
 *   [3]  u32  value buffer size in bytes
 *   [4]  u32  request size (bytes of input values in [5..])
 *   [5+] u32  value buffer (request values in, response values out)
 *   [n]  u32  0x00000000  end tag
 *
 * Note: mbox_call() sets buf[1]=0 internally before sending.
 */

#include "mailbox_property.h"

/* ── helpers ─────────────────────────────────────────────────────── */

/*
 * Simple single-tag call.
 * vbuf_size: size of value buffer in bytes.
 * req_size:  number of bytes of request data in val[].
 * val[]:     input values (request); output values written back by VC.
 * nval:      number of u32s in val[].
 */
static int mbox_simple(uint32_t tag,
                        uint32_t vbuf_size, uint32_t req_size,
                        volatile uint32_t *val, int nval)
{
    /* Max buffer: header(2) + tag_hdr(3) + values(nval) + end(1) */
    volatile uint32_t __attribute__((aligned(16))) buf[32];
    int n = 0;

    buf[n++] = 0;                   /* [0] size — filled below */
    buf[n++] = MBOX_REQUEST;        /* [1] request code */
    buf[n++] = tag;                 /* [2] tag */
    buf[n++] = vbuf_size;           /* [3] value buffer size */
    buf[n++] = req_size;            /* [4] request size */
    for (int i = 0; i < nval; i++)
        buf[n++] = val[i];          /* [5..] values */
    /* pad value buffer to vbuf_size if nval*4 < vbuf_size */
    int vbuf_words = (int)(vbuf_size + 3) / 4;
    while (n < 5 + vbuf_words)
        buf[n++] = 0;
    buf[n++] = MBOX_TAG_END;        /* end tag */
    buf[0]   = (uint32_t)(n * 4);  /* total size */

    if (mbox_call(buf) != 0)
        return -1;

    /* Copy response values back to caller */
    for (int i = 0; i < nval; i++)
        val[i] = buf[5 + i];

    return 0;
}

/* ── hardware info ─────────────────────────────────────────────── */

uint32_t mbox_get_firmware_revision(void)
{
    volatile uint32_t v[1] = { 0 };
    if (mbox_simple(MBOX_TAG_GET_FIRMWARE_REV, 4, 0, v, 1) < 0)
        return 0;
    return v[0];
}

uint32_t mbox_get_board_model(void)
{
    volatile uint32_t v[1] = { 0 };
    if (mbox_simple(MBOX_TAG_GET_BOARD_MODEL, 4, 0, v, 1) < 0)
        return 0;
    return v[0];
}

uint32_t mbox_get_board_revision(void)
{
    volatile uint32_t v[1] = { 0 };
    if (mbox_simple(MBOX_TAG_GET_BOARD_REVISION, 4, 0, v, 1) < 0)
        return 0;
    return v[0];
}

uint64_t mbox_get_board_serial(void)
{
    volatile uint32_t v[2] = { 0, 0 };
    if (mbox_simple(MBOX_TAG_GET_BOARD_SERIAL, 8, 0, v, 2) < 0)
        return 0;
    return ((uint64_t)v[1] << 32) | v[0];
}

void mbox_get_arm_memory(uint32_t *base, uint32_t *size)
{
    volatile uint32_t v[2] = { 0, 0 };
    if (mbox_simple(MBOX_TAG_GET_ARM_MEMORY, 8, 0, v, 2) < 0) {
        if (base) *base = 0;
        if (size) *size = 0;
        return;
    }
    if (base) *base = v[0];
    if (size) *size = v[1];
}

void mbox_get_vc_memory(uint32_t *base, uint32_t *size)
{
    volatile uint32_t v[2] = { 0, 0 };
    if (mbox_simple(MBOX_TAG_GET_VC_MEMORY, 8, 0, v, 2) < 0) {
        if (base) *base = 0;
        if (size) *size = 0;
        return;
    }
    if (base) *base = v[0];
    if (size) *size = v[1];
}

/* ── power management ──────────────────────────────────────────── */

int mbox_get_power_state(uint32_t device_id)
{
    volatile uint32_t v[2] = { device_id, 0 };
    if (mbox_simple(MBOX_TAG_GET_POWER_STATE, 8, 4, v, 2) < 0)
        return -1;
    return (int)(v[1] & 0x1);
}

int mbox_set_power_state(uint32_t device_id, int on, int wait)
{
    uint32_t flags = (on ? 1U : 0U) | (wait ? 2U : 0U);
    volatile uint32_t v[2] = { device_id, flags };
    if (mbox_simple(MBOX_TAG_SET_POWER_STATE, 8, 8, v, 2) < 0)
        return -1;
    return 0;
}

/* ── clocks ────────────────────────────────────────────────────── */

uint32_t mbox_get_clock_rate(uint32_t clock_id)
{
    volatile uint32_t v[2] = { clock_id, 0 };
    if (mbox_simple(MBOX_TAG_GET_CLOCK_RATE, 8, 4, v, 2) < 0)
        return 0;
    return v[1];
}

uint32_t mbox_get_max_clock_rate(uint32_t clock_id)
{
    volatile uint32_t v[2] = { clock_id, 0 };
    if (mbox_simple(MBOX_TAG_GET_MAX_CLOCK_RATE, 8, 4, v, 2) < 0)
        return 0;
    return v[1];
}

int mbox_set_clock_rate(uint32_t clock_id, uint32_t rate_hz)
{
    volatile uint32_t v[3] = { clock_id, rate_hz, 0 };
    return mbox_simple(MBOX_TAG_SET_CLOCK_RATE, 12, 8, v, 3);
}

/* ── USB / PCIe ────────────────────────────────────────────────── */

int mbox_power_on_usb(void)
{
    /*
     * Power on the USB HCD (device ID 3).
     * May be required on some Pi 4 variants before NOTIFY_XHCI_RESET.
     * Wait=1 so the call blocks until the VC confirms power-on.
     */
    return mbox_set_power_state(MBOX_PWR_USB_HCD, 1, 1);
}

int mbox_notify_xhci_reset(uint32_t pcie_bdf)
{
    /*
     * Tag 0x00030058: ask VC to load VL805 firmware via PCIe.
     *
     * Value buffer: one u32 = PCIe BDF.
     * Request size must be 4 (not 0) — the VC reads the BDF to know
     * which device to target.  Sending req_size=0 causes code=0x00000000.
     *
     * This call fails silently on boards with SPI EEPROM (c03112 etc.)
     * — the VL805 self-loads from EEPROM, the VC returns an error code
     * but USB still works.  Only boards without EEPROM (d03114, Pi 400,
     * CM4) strictly need this to succeed.
     */
    volatile uint32_t v[1] = { pcie_bdf };
    return mbox_simple(MBOX_TAG_NOTIFY_XHCI_RESET, 4, 4, v, 1);
}

/* ── LED / diagnostics ─────────────────────────────────────────── */

int mbox_set_led_state(uint32_t gpio_pin, int on)
{
    volatile uint32_t v[2] = { gpio_pin, (uint32_t)(on ? 1 : 0) };
    return mbox_simple(MBOX_TAG_SET_LED_STATE, 8, 8, v, 2);
}
