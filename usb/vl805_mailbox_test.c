/*
 * vl805_mailbox_test.c — Quick & dirty mailbox diagnostic
 *
 * Tests mailbox tag 0x00030058 (NOTIFY_XHCI_RESET / VL805 firmware load)
 * and several related tags to understand what the VC firmware knows.
 *
 * Drop this into the kernel init sequence in place of (or just before)
 * vl805_init() to get a full picture of mailbox state.
 *
 * Uses the project's working mbox_call() from drivers/gpu/mailbox.c.
 *
 * Call vl805_mailbox_test() from pci_init_pi4() on the PERST# path,
 * before the existing vl805_init() call.
 */

#include "kernel.h"
#include "mailbox.h"   /* mbox_call() from drivers/gpu/mailbox.c */

extern void uart_puts(const char *s);
extern void uart_putc(char c);
extern uint64_t get_mailbox_base(void);

/* ── helpers ───────────────────────────────────────────────────── */

static void print_hex32(uint32_t v) {
    for (int i = 28; i >= 0; i -= 4) {
        int n = (v >> i) & 0xF;
        uart_putc(n < 10 ? '0' + n : 'a' + n - 10);
    }
}

static void print_dec(uint32_t v) {
    if (v == 0) { uart_putc('0'); return; }
    char buf[12]; int i = 0;
    while (v) { buf[i++] = '0' + (v % 10); v /= 10; }
    while (i--) uart_putc(buf[i]);
}

/* ── single-tag test using mbox_call() ─────────────────────────── */

static int mbox_test_tag(const char *name, uint32_t tag,
                         uint32_t val_buf_size, uint32_t req_val)
{
    /* 16-byte aligned buffer: [size][code][tag][vbuf_size][req_size][val...][0] */
    static volatile uint32_t __attribute__((aligned(16))) buf[16];

    buf[0] = 7 * 4;          /* total size */
    buf[1] = 0;              /* request */
    buf[2] = tag;
    buf[3] = val_buf_size;
    buf[4] = val_buf_size;   /* request length = value buffer size */
    buf[5] = req_val;        /* request value (e.g. BDF for 0x00030058) */
    buf[6] = 0;              /* end tag */

    uart_puts("[MBOX] tag=0x"); print_hex32(tag);
    uart_puts(" ("); uart_puts(name); uart_puts(") req=0x"); print_hex32(req_val);
    uart_puts(" ... ");

    int r = mbox_call(buf);

    if (r == 0) {
        uart_puts("OK  response=0x"); print_hex32(buf[5]);
        uart_puts("\n");
    } else {
        uart_puts("FAIL  code=0x"); print_hex32(buf[1]);
        uart_puts("  buf[5]=0x"); print_hex32(buf[5]);
        uart_puts("\n");
    }
    return r;
}

/* ── get firmware revision (sanity check mailbox works) ─────────── */

static void mbox_get_fw_rev(void) {
    static volatile uint32_t __attribute__((aligned(16))) buf[8];
    buf[0] = 7 * 4;
    buf[1] = 0;
    buf[2] = 0x00000001;  /* GET_FIRMWARE_REVISION */
    buf[3] = 4;
    buf[4] = 0;
    buf[5] = 0;
    buf[6] = 0;

    uart_puts("[MBOX] GET_FIRMWARE_REVISION ... ");
    int r = mbox_call(buf);
    if (r == 0) {
        uart_puts("OK  revision=0x"); print_hex32(buf[5]); uart_puts("\n");
    } else {
        uart_puts("FAIL  code=0x"); print_hex32(buf[1]); uart_puts("\n");
    }
}

/* ── try 0x00030058 with different BDF encodings ────────────────── */

static void mbox_test_notify_xhci(void) {
    uart_puts("[MBOX] --- Testing 0x00030058 (NOTIFY_XHCI_RESET) ---\n");

    /*
     * Tag 0x00030058 value buffer layout (from Linux vl805.c):
     *   u32: PCIe BDF encoded as  (bus << 20) | (dev << 15) | (fn << 12)
     *        for bus=1,dev=0,fn=0 → 0x00100000
     *
     * Some firmware versions also accept bus=0,dev=0,fn=0 → 0x00000000
     * Try both.
     */
    mbox_test_tag("NOTIFY_XHCI bus=1,dev=0,fn=0", 0x00030058, 4, 0x00100000U);
    mbox_test_tag("NOTIFY_XHCI bus=0,dev=0,fn=0", 0x00030058, 4, 0x00000000U);

    /*
     * Some firmware versions expect an 8-byte value buffer:
     *   u32[0]: BDF as above
     *   u32[1]: reserved / flags (0)
     */
    uart_puts("[MBOX] tag=0x00030058 8-byte vbuf, BDF=0x00100000 ... ");
    static volatile uint32_t __attribute__((aligned(16))) buf8[10];
    buf8[0] = 9 * 4;
    buf8[1] = 0;
    buf8[2] = 0x00030058U;
    buf8[3] = 8;     /* value buffer size = 8 */
    buf8[4] = 8;     /* request length = 8 */
    buf8[5] = 0x00100000U;
    buf8[6] = 0;
    buf8[7] = 0;     /* end tag */
    int r = mbox_call(buf8);
    if (r == 0) {
        uart_puts("OK  [5]=0x"); print_hex32(buf8[5]);
        uart_puts(" [6]=0x"); print_hex32(buf8[6]); uart_puts("\n");
    } else {
        uart_puts("FAIL  code=0x"); print_hex32(buf8[1]); uart_puts("\n");
    }
}

/* ── check VC knows about USB ────────────────────────────────────── */

static void mbox_test_usb_tags(void) {
    uart_puts("[MBOX] --- USB-related tags ---\n");

    /* 0x00030056 — GET_USB_STATE (Pi 4 specific, returns whether USB fw loaded) */
    mbox_test_tag("GET_USB_STATE",    0x00030056, 4, 0);

    /* 0x00030057 — sometimes listed as USB_RESET */
    mbox_test_tag("0x00030057",       0x00030057, 4, 0);

    /* 0x00030059 — sometimes listed as NOTIFY_XHCI_RESET variant */
    mbox_test_tag("0x00030059",       0x00030059, 4, 0x00100000U);
}

/* ── check mailbox base address ─────────────────────────────────── */

static void print_mbox_base(void) {
    uint64_t base = get_mailbox_base();
    uart_puts("[MBOX] Mailbox MMIO base = 0x");
    for (int i = 60; i >= 0; i -= 4) {
        int n = (base >> i) & 0xF;
        uart_putc(n < 10 ? '0' + n : 'a' + n - 10);
    }
    uart_puts("\n");
}

/* ── entry point ─────────────────────────────────────────────────── */

void vl805_mailbox_test(void) {
    uart_puts("\n[MBOX] ══════════════════════════════\n");
    uart_puts("[MBOX]  VL805 mailbox diagnostic\n");
    uart_puts("[MBOX] ══════════════════════════════\n");

    print_mbox_base();

    /* Step 1: verify basic mailbox works */
    mbox_get_fw_rev();

    /* Step 2: try the VL805 firmware load tag */
    mbox_test_notify_xhci();

    /* Step 3: try related USB tags */
    mbox_test_usb_tags();

    uart_puts("[MBOX] ══════════════════════════════\n\n");
}
