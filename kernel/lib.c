/*
 * lib.c - Basic library functions
 */
#include "kernel.h"
#include <stdarg.h>
#include <string.h>
/* boot265: visual heartbeat — draw to framebuffer without fb_mark_ready dependency */
#include "drivers/gpu/framebuffer.h"
/* boot303: Ethernet link + ARP */
#include "drivers/net/genet.h"

/* Forward declare UART functions */
extern void uart_putc(char c);
extern void uart_puts(const char *str);
extern void uart_hex(uint64_t value);
extern void uart_dec(int64_t value);

/* Forward declare console function (may not be ready yet) */
extern void con_putc(char c);

static int _fb_up = 0;   /* set to 1 after gpu_init() completes */

void fb_mark_ready(void)  { _fb_up = 1; }
void fb_mark_unready(void){ _fb_up = 0; }

static void _put(char c) {
    /* Always send to UART only.
     * boot169: removed the automatic debug_print→con_putc mirror.
     * Hundreds of xHCI/USB/FAT32 diagnostic lines were flooding the
     * screen and making it unreadable.  Screen output is now produced
     * only by explicit con_printf() calls at key boot milestones.    */
    if (c == '\n') uart_putc('\r');
    uart_putc(c);
    (void)_fb_up;   /* suppress unused-variable warning */
}

/* Real debug print with UART */
void debug_print(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);

    while (*fmt) {
        if (*fmt == '%') {
            fmt++;
            /* Parse optional flags and width (e.g. %04x, %02x) */
            char pad_char = ' ';
            int width = 0;
            if (*fmt == '0') { pad_char = '0'; fmt++; }
            while (*fmt >= '1' && *fmt <= '9') { width = width * 10 + (*fmt - '0'); fmt++; }

            switch (*fmt) {
                case 's': {
                    const char *s = va_arg(args, const char *);
                    const char *p = s ? s : "(null)";
                    while (*p) _put(*p++);
                    break;
                }
                case 'd': {
                    int val = va_arg(args, int);
                    if (val < 0) { _put('-'); val = -val; }
                    if (val == 0) { _put('0'); break; }
                    char buf[16]; int n = 0;
                    while (val > 0) { buf[n++] = '0' + val%10; val /= 10; }
                    for (int k = n-1; k >= 0; k--) _put(buf[k]);
                    break;
                }
                case 'u': {
                    unsigned val = va_arg(args, unsigned);
                    if (val == 0) { _put('0'); break; }
                    char buf[16]; int n = 0;
                    while (val > 0) { buf[n++] = '0' + val%10; val /= 10; }
                    for (int k = n-1; k >= 0; k--) _put(buf[k]);
                    break;
                }
                case 'x': {
                    unsigned int val = va_arg(args, unsigned int);
                    const char *h = "0123456789abcdef";
                    /* Build hex string */
                    char buf[8]; int n = 0;
                    if (val == 0) { buf[n++] = '0'; }
                    else { unsigned v = val; while (v) { buf[n++] = h[v & 0xF]; v >>= 4; } }
                    /* Pad to width */
                    for (int k = n; k < width; k++) _put(pad_char);
                    for (int k = n-1; k >= 0; k--) _put(buf[k]);
                    break;
                }
                case 'p': {
                    void *ptr = va_arg(args, void *);
                    uint64_t v = (uint64_t)ptr;
                    const char *h = "0123456789abcdef";
                    _put('0'); _put('x');
                    for (int sh = 60; sh >= 0; sh -= 4) _put(h[(v>>sh)&0xF]);
                    break;
                }
                case 'l': {
                    fmt++;
                    if (*fmt == 'l') fmt++;
                    if (*fmt == 'd') {
                        int64_t val = va_arg(args, int64_t);
                        if (val < 0) { _put('-'); val = -val; }
                        if (val == 0) { _put('0'); break; }
                        char buf[24]; int n = 0;
                        while (val > 0) { buf[n++] = '0' + (int)(val%10); val /= 10; }
                        for (int k = n-1; k >= 0; k--) _put(buf[k]);
                    } else if (*fmt == 'x') {
                        uint64_t val = va_arg(args, uint64_t);
                        const char *h = "0123456789abcdef";
                        _put('0'); _put('x');
                        for (int sh = 60; sh >= 0; sh -= 4) _put(h[(val>>sh)&0xF]);
                    }
                    break;
                }
                case 'z': {
                    fmt++;  /* skip 'u' */
                    uint64_t val = va_arg(args, uint64_t);
                    if (val == 0) { _put('0'); break; }
                    char buf[24]; int n = 0;
                    while (val > 0) { buf[n++] = '0' + (int)(val % 10); val /= 10; }
                    for (int k = n-1; k >= 0; k--) _put(buf[k]);
                    break;
                }
                default:
                    _put('%');
                    _put(*fmt);
                    break;
            }
        } else {
            _put(*fmt);
        }
        fmt++;
    }

    va_end(args);
}

/* Real memory allocation with bump allocator */
#define HEAP_SIZE (16 * 1024 * 1024)  // 16MB heap





/* String functions */
void *memset(void *s, int c, size_t n) {
    unsigned char *p = s;
    while (n--) *p++ = (unsigned char)c;
    return s;
}

void *memcpy(void *dest, const void *src, size_t n) {
    char *d = dest;
    const char *s = src;
    while (n--) *d++ = *s++;
    return dest;
}

int memcmp(const void *s1, const void *s2, size_t n) {
    const unsigned char *p1 = s1, *p2 = s2;
    while (n--) {
        if (*p1 != *p2) return *p1 - *p2;
        p1++; p2++;
    }
    return 0;
}

size_t strlen(const char *s) {
    const char *p = s;
    while (*p) p++;
    return p - s;
}

size_t strnlen(const char *s, size_t maxlen) {
    size_t len = 0;
    while (len < maxlen && s[len]) len++;
    return len;
}

char *strcpy(char *dest, const char *src) {
    char *d = dest;
    while ((*d++ = *src++));
    return dest;
}

char *strncpy(char *dest, const char *src, size_t n) {
    size_t i;
    for (i = 0; i < n && src[i]; i++) {
        dest[i] = src[i];
    }
    for (; i < n; i++) {
        dest[i] = '\0';
    }
    return dest;
}

int strcmp(const char *s1, const char *s2) {
    while (*s1 && *s1 == *s2) {
        s1++; s2++;
    }
    return *(unsigned char*)s1 - *(unsigned char*)s2;
}

int strncmp(const char *s1, const char *s2, size_t n) {
    while (n && *s1 && *s1 == *s2) {
        s1++; s2++; n--;
    }
    return n ? *(unsigned char*)s1 - *(unsigned char*)s2 : 0;
}

/* Stub functions */
void pci_scan_bus(void) {}
void vfs_init(void) {}
void net_init(void) {}

/* ── DHCP client (boot334) ────────────────────────────────────────────────
 * Minimal DISCOVER → OFFER → REQUEST → ACK state machine over UDP/IP.
 * No TCP needed.  Falls back to static 192.168.0.200 after 5 retries.   */

#define DHCP_DISCOVER     1
#define DHCP_REQUEST      3
#define DHCP_MSG_OFFER    2
#define DHCP_MSG_ACK      5
#define DHCP_MSG_NAK      6

#define DHCP_ST_IDLE      0   /* not started yet             */
#define DHCP_ST_DISCOVER  1   /* sent DISCOVER, awaiting OFFER */
#define DHCP_ST_REQUEST   2   /* sent REQUEST,  awaiting ACK  */
#define DHCP_ST_BOUND     3   /* ACK received, IP assigned    */

static int     g_dhcp_st       = DHCP_ST_IDLE;
static uint8_t g_dhcp_yiaddr[4];          /* IP offered to us            */
static uint8_t g_dhcp_srvid[4];           /* server identifier option    */
static int     g_dhcp_tries    = 0;       /* DISCOVER/REQUEST retries    */
/* XID = "PHOE" — identifies our transaction in server replies           */
static const uint8_t g_dhcp_xid[4]    = { 0x50, 0x48, 0x4F, 0x45 };
/* Fallback static IP if DHCP times out completely                       */
static const uint8_t g_dhcp_static[4] = { 192, 168, 0, 200 };

/* One's-complement IP checksum */
static uint16_t _ip_cksum(const uint8_t *p, int n)
{
    uint32_t s = 0;
    for (int i = 0; i < n; i += 2)
        s += (uint32_t)((p[i] << 8) | (i+1 < n ? p[i+1] : 0));
    while (s >> 16) s = (s & 0xffff) + (s >> 16);
    return (uint16_t)(~s & 0xffff);
}

/* Build a 342-byte Ethernet/IPv4/UDP/DHCP frame into buf.
 * type = DHCP_DISCOVER or DHCP_REQUEST.
 * DHCP frame layout:
 *   [  0.. 13] Ethernet header (14 B)
 *   [ 14.. 33] IPv4 header    (20 B)  total = 328 B
 *   [ 34.. 41] UDP header      (8 B)  length = 308 B
 *   [ 42..341] DHCP payload  (300 B)
 *     [42+  0] op  [42+  1] htype [42+  2] hlen [42+  3] hops
 *     [42+  4..7]  xid
 *     [42+  8..9]  secs   [42+10..11] flags
 *     [42+ 12..15] ciaddr [42+16..19] yiaddr
 *     [42+ 20..23] siaddr [42+24..27] giaddr
 *     [42+ 28..43] chaddr (16 B)
 *     [42+ 44..107] sname (64 B)
 *     [42+108..235] file  (128 B)
 *     [42+236..239] magic cookie = 63:82:53:63
 *     [42+240..]    options (TLV)                                       */
static int dhcp_make(uint8_t *buf, int type)
{
    for (int i = 0; i < 342; i++) buf[i] = 0;

    /* Ethernet: dst=broadcast, src=our MAC, type=IPv4 */
    for (int i = 0; i < 6; i++) { buf[i] = 0xff; buf[6+i] = g_genet_mac[i]; }
    buf[12] = 0x08; buf[13] = 0x00;

    /* IPv4: ver=4 IHL=5, total=328, DF, TTL=64, proto=UDP(17)
     * src=0.0.0.0, dst=255.255.255.255                                  */
    buf[14] = 0x45;
    buf[16] = 0x01; buf[17] = 0x48;   /* total length 328 */
    buf[20] = 0x40;                    /* DF bit           */
    buf[22] = 0x40;                    /* TTL = 64         */
    buf[23] = 0x11;                    /* UDP              */
    buf[30] = 255; buf[31] = 255; buf[32] = 255; buf[33] = 255;
    uint16_t ck = _ip_cksum(buf+14, 20);
    buf[24] = (uint8_t)(ck >> 8); buf[25] = (uint8_t)ck;

    /* UDP: src=68, dst=67, length=308, checksum=0 (disabled in IPv4) */
    buf[34] = 0x00; buf[35] = 0x44;   /* src port 68 */
    buf[36] = 0x00; buf[37] = 0x43;   /* dst port 67 */
    buf[38] = 0x01; buf[39] = 0x34;   /* length 308  */

    /* DHCP fixed header */
    buf[42] = 0x01;              /* op = BOOTREQUEST   */
    buf[43] = 0x01;              /* htype = Ethernet   */
    buf[44] = 0x06;              /* hlen  = 6          */
    buf[46] = g_dhcp_xid[0]; buf[47] = g_dhcp_xid[1];
    buf[48] = g_dhcp_xid[2]; buf[49] = g_dhcp_xid[3];
    buf[52] = 0x80;              /* flags: broadcast bit */
    /* chaddr at offset 42+28 = 70 */
    for (int i = 0; i < 6; i++) buf[70+i] = g_genet_mac[i];
    /* magic cookie at 42+236 = 278 */
    buf[278] = 0x63; buf[279] = 0x82; buf[280] = 0x53; buf[281] = 0x63;

    /* Options at 282 */
    int o = 282;
    buf[o++] = 53; buf[o++] = 1; buf[o++] = (uint8_t)type; /* msg type */
    if (type == DHCP_REQUEST) {
        buf[o++] = 50; buf[o++] = 4;              /* Requested IP  */
        buf[o++] = g_dhcp_yiaddr[0]; buf[o++] = g_dhcp_yiaddr[1];
        buf[o++] = g_dhcp_yiaddr[2]; buf[o++] = g_dhcp_yiaddr[3];
        buf[o++] = 54; buf[o++] = 4;              /* Server ID     */
        buf[o++] = g_dhcp_srvid[0]; buf[o++] = g_dhcp_srvid[1];
        buf[o++] = g_dhcp_srvid[2]; buf[o++] = g_dhcp_srvid[3];
    } else {
        /* Parameter Request List: subnet(1), router(3), domain(15), DNS(6) */
        buf[o++] = 55; buf[o++] = 4;
        buf[o++] = 1; buf[o++] = 3; buf[o++] = 15; buf[o++] = 6;
    }
    buf[o] = 0xff; /* End option */
    return 342;
}

/*
 * wimp_task — main desktop/input polling loop (boot178).
 *
 * Replaces the bare WFE stub with a real service loop:
 *   • hid_poll_all()    — reads HID boot-protocol reports (keyboard + mouse)
 *   • hub_poll_hotplug() — checks for USB devices plugged into the hub
 *   • xhci_check_hotplug() — processes root-hub PSCE events
 *
 * HID is polled every iteration (~yield cadence ≈ 10 ms).
 * Hotplug is rate-limited to every 500 ms to avoid flooding the bus with
 * GET_PORT_STATUS control transfers.
 */

/* ARM system counter: CNTPCT_EL0 / CNTFRQ_EL0, gives milliseconds */
static inline uint32_t wimp_ms(void) {
    uint64_t cnt, freq;
    __asm__ volatile("mrs %0, cntpct_el0" : "=r"(cnt));
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
    return (uint32_t)(cnt / (freq / 1000ULL));
}

void wimp_task(void)
{
    /* boot256: guarantee uart_puts is live for WIMP/interrupt diagnostics */
    extern void uart_set_quiet(int q);
    uart_set_quiet(0);

    extern int  hid_poll_all(void)               __attribute__((weak));
    extern void hid_poll_mice(void)              __attribute__((weak));
    extern void hub_poll_hotplug(void)           __attribute__((weak));
    extern int  xhci_check_hotplug(void)         __attribute__((weak));
    extern void mouse_get_pos(int16_t *x,
                              int16_t *y)        __attribute__((weak));
    extern void cursor_init(void)                __attribute__((weak));
    extern void cursor_update(int x, int y)      __attribute__((weak));
    extern void con_puts(const char *s)          __attribute__((weak));

    /* boot179: Initialise mouse cursor and place it at the starting
     * mouse position (mouse_init() sets this to (640,360)).         */
    if (cursor_init) cursor_init();
    int16_t last_mx = 640, last_my = 360;
    if (mouse_get_pos && cursor_update) {
        mouse_get_pos(&last_mx, &last_my);
        cursor_update((int)last_mx, (int)last_my);
    }

    debug_print("[WIMP] task running — cursor at (%d,%d)\n",
                (int)last_mx, (int)last_my);

    /* boot301: startup splash — welcome panel drawn directly on the
     * framebuffer before the main loop starts.  Uses fb.width/height so
     * it centres correctly on any resolution the GPU has set up.        */
    if (fb.valid) {
        /* Panel geometry — centred, slightly above mid-screen */
        int pw = 560, ph = 180;
        int px = (int)((fb.width  - (uint32_t)pw) / 2u);
        int py = (int)((fb.height - (uint32_t)ph) / 2u) - 60;

        /* Dark background with double border */
        fb_fill_rect(px,   py,   pw,   ph,   COL_DARK_GREY);
        fb_draw_rect(px,   py,   pw,   ph,   COL_RISCOS_GREY);
        fb_draw_rect(px+2, py+2, pw-4, ph-4, COL_GREY);

        /* Title: "Phoenix OS" — scale 4 → each char is 32×32 px */
        int title_w = 10 * 8 * 4;          /* 10 chars × 8px × scale */
        int tx = px + (pw - title_w) / 2;
        int ty = py + 22;
        fb_draw_string_scaled(tx, ty, "Phoenix OS", COL_WHITE, COL_DARK_GREY, 4);

        /* Thin divider below title */
        fb_draw_line(px + 24, ty + 42, px + pw - 24, ty + 42, COL_RISCOS_GREY);

        /* Subtitle: "Bare-metal AArch64" — scale 2 → 16px tall */
        int sub_w = 18 * 8 * 2;
        int sx = px + (pw - sub_w) / 2;
        int sy = ty + 54;
        fb_draw_string_scaled(sx, sy, "Bare-metal AArch64", COL_CYAN, COL_DARK_GREY, 2);

        /* Small build tag — scale 1 (8px) */
        int tag_w = 29 * 8;
        int bx = px + (pw - tag_w) / 2;
        int by = sy + 30;
        fb_draw_string(bx, by, "boot340  BCM2711 / Cortex-A72",
                       COL_GREY, COL_DARK_GREY);
    }

    /* boot304/305: wait up to 8 s for GENETv5 PHY autoneg (boot305 measures
     * 3519 ms on BCM54213PE).  Just polls and logs; gratuitous ARP is sent
     * by the heartbeat link-state monitor on first tick (net_link_prev=-1). */
    {
        int link = 0;
        uint32_t t0 = wimp_ms();
        while ((wimp_ms() - t0) < 8000u) {
            if (genet_link_up()) { link = 1; break; }
            /* tiny pause — avoid hammering MDIO */
            for (volatile int d = 0; d < 50000; d++) {}
        }
        if (link) {
            debug_print("[WIMP] GENET link UP after %u ms\n",
                        (unsigned)(wimp_ms() - t0));
        } else {
            debug_print("[WIMP] GENET link still DOWN after 8 s\n");
            if (con_puts) con_puts("[NET] No link\n");
        }
    }

    /* keyboard_poll: drain the RISC OS keyboard event ring */
    extern int keyboard_poll(void *ev) __attribute__((weak));

    /* boot267: DWC2 OTG HID poll (mouse/keyboard via hub on USB-C port) */
    extern void dwc2_hid_poll(void) __attribute__((weak));

    /* Print a prompt so the user knows the console is live */
    if (con_puts) con_puts("\n[ESC to stop loop]\n> ");

    /* boot271: drain UART TX FIFO before entering the loop so we start
     * with a known-empty FIFO and uart_putc has 16 free slots.       */
    extern void uart_drain(void);
    extern volatile uint32_t g_uart_stalls;
    uart_drain();
    debug_print("[WIMP] drain done, stalls=%u\n", (unsigned)g_uart_stalls);

    uint32_t last_hotplug  = wimp_ms();
    uint32_t last_hid      = wimp_ms();
    uint32_t last_mouse    = wimp_ms(); /* boot285: 4 ms high-frequency mouse poll */
    uint32_t last_beat     = wimp_ms();
    uint32_t last_dwc2     = wimp_ms(); /* boot267: DWC2 HID poll timestamp */
    uint32_t last_net      = wimp_ms(); /* boot303: GENET RX poll */
    uint32_t last_dhcp     = 0;        /* boot334: DHCP retransmit timer */
    /* boot305: start at -1 (unknown) so the heartbeat always fires on its
     * first check, logs the link state, and sends the gratuitous ARP if
     * link is already up from the initial wait.                            */
    int      net_link_prev = -1;
    static uint8_t g_rx_frame[GENET_MAX_FRAME]; /* RX frame buffer */
    /* our_ip: starts 0.0.0.0; filled by DHCP ACK or static fallback */
    static uint8_t our_ip[4] = { 0, 0, 0, 0 };
    /* boot265: visual heartbeat — blinks a 16×16 square below the green
     * corner marker (top-right) at 1 Hz so the user can see WIMP is alive
     * without needing a serial terminal.  Drawn only when fb.valid is set. */
    uint32_t last_visual  = wimp_ms();
    int      visual_on    = 0;

    while (1) {
        /* ── Heartbeat: 500 ms — confirms WIMP loop is running ────────── */
        {
            uint32_t now_b = wimp_ms();
            if ((now_b - last_beat) >= 500u) {
                last_beat = now_b;
                extern uint32_t genet_rx_pidx_raw(void);
                extern uint32_t genet_rx_count_raw(void);
                extern uint32_t genet_tx_cons_raw(void);
                extern uint32_t genet_rx_fcs_raw(void);
                debug_print("[WIMP] alive t=%dms stalls=%u rx=%u pidx=%u mib=%u rfcs=%u\n",
                            (int)now_b, (unsigned)g_uart_stalls,
                            (unsigned)genet_rx_count_raw(),
                            (unsigned)genet_rx_pidx_raw(),
                            (unsigned)genet_tx_cons_raw(),
                            (unsigned)genet_rx_fcs_raw());
                /* boot303/304: log link state changes; send grat ARP on UP */
                int lnk = genet_link_up();
                if (lnk != net_link_prev) {
                    net_link_prev = lnk;
                    debug_print("[NET] link %s\n", lnk ? "UP" : "DOWN");
                    if (lnk) {
                        /* boot308: apply negotiated PHY speed to UMAC.
                         * PHY drives RGMII RX clock; if UMAC speed doesn't
                         * match, pidx never advances (RX silently broken). */
                        genet_apply_link();
                        /* boot334: start DHCP to acquire IP dynamically.
                         * Grat ARP deferred until we receive a DHCP ACK
                         * (or fall back to static after 5 retries).      */
                        static uint8_t disc[342];
                        dhcp_make(disc, DHCP_DISCOVER);
                        genet_send(disc, 342);
                        g_dhcp_st    = DHCP_ST_DISCOVER;
                        g_dhcp_tries = 1;
                        last_dhcp    = wimp_ms();
                        debug_print("[DHCP] DISCOVER sent (xid=PHOE)\n");
                    }
                }
            }
        }

        /* ── DHCP retransmit: 4 s — resend if no reply yet ────────────── */
        {
            uint32_t now_b = wimp_ms();
            if ((g_dhcp_st == DHCP_ST_DISCOVER ||
                 g_dhcp_st == DHCP_ST_REQUEST) &&
                (now_b - last_dhcp) >= 4000u) {
                last_dhcp = now_b;
                g_dhcp_tries++;
                if (g_dhcp_tries > 5) {
                    /* Give up — fall back to static IP */
                    for (int _i=0;_i<4;_i++) our_ip[_i]=g_dhcp_static[_i];
                    g_dhcp_st = DHCP_ST_BOUND;
                    debug_print("[DHCP] timeout — using static %d.%d.%d.%d\n",
                        our_ip[0],our_ip[1],our_ip[2],our_ip[3]);
                    /* Gratuitous ARP with fallback IP */
                    static uint8_t gfb[60];
                    for (int _i=0;_i<60;_i++) gfb[_i]=0;
                    for (int _i=0;_i<6;_i++) gfb[_i]=0xff;
                    for (int _i=0;_i<6;_i++) gfb[6+_i]=g_genet_mac[_i];
                    gfb[12]=0x08; gfb[13]=0x06;
                    gfb[14]=0x00; gfb[15]=0x01; gfb[16]=0x08; gfb[17]=0x00;
                    gfb[18]=6;    gfb[19]=4;
                    gfb[20]=0x00; gfb[21]=0x02;
                    for (int _i=0;_i<6;_i++) gfb[22+_i]=g_genet_mac[_i];
                    for (int _i=0;_i<4;_i++) gfb[28+_i]=our_ip[_i];
                    gfb[32]=0xff; gfb[33]=0xff; gfb[34]=0xff;
                    gfb[35]=0xff; gfb[36]=0xff; gfb[37]=0xff;
                    for (int _i=0;_i<4;_i++) gfb[38+_i]=our_ip[_i];
                    genet_send(gfb, 60);
                } else {
                    int t = (g_dhcp_st==DHCP_ST_DISCOVER)
                            ? DHCP_DISCOVER : DHCP_REQUEST;
                    static uint8_t dret[342];
                    dhcp_make(dret, t);
                    genet_send(dret, 342);
                    debug_print("[DHCP] retransmit try=%d\n", g_dhcp_tries);
                }
            }
        }

        /* ── GENET RX poll: 4 ms — drain entire RX ring each tick ─────── */
        /* boot340: process all pending frames in one tick instead of one
         * per tick.  With a single-frame policy, an ICMP echo request
         * behind 50 queued broadcast frames waits 50×4 ms = 200 ms before
         * being answered — in the worst case (ring fills between ticks)
         * it can wait seconds.  Draining the ring in a tight inner loop
         * delivers sub-4 ms echo latency regardless of background traffic
         * volume.  The ring holds at most 64 frames (RX_DESC_COUNT), so
         * the inner loop is bounded and can't stall the outer WIMP loop
         * for more than a few hundred µs.                                */
        {
            uint32_t now_n = wimp_ms();
            if ((now_n - last_net) >= 4u) {
                last_net = now_n;
                int flen;
                while ((flen = genet_poll_rx(g_rx_frame, GENET_MAX_FRAME)) > 13) {
                    uint16_t etype = (uint16_t)
                        ((g_rx_frame[12] << 8) | g_rx_frame[13]);

                    /* ARP reply handler — answer who-has for our IP */
                    if (etype == 0x0806 && flen >= 42) {
                        uint16_t op = (uint16_t)
                            ((g_rx_frame[20] << 8) | g_rx_frame[21]);
                        if (op == 1 &&
                            g_rx_frame[38] == our_ip[0] &&
                            g_rx_frame[39] == our_ip[1] &&
                            g_rx_frame[40] == our_ip[2] &&
                            g_rx_frame[41] == our_ip[3]) {
                            static uint8_t reply[60];
                            for (int _i=0;_i<60;_i++) reply[_i]=0;
                            /* boot340: unicast ARP reply — dst = requester's MAC.
                             * RFC 826 requires unicast ARP replies.  boot329 used
                             * broadcast ff:ff:ff:ff:ff:ff in case the switch had not
                             * yet learned the requester's port, but the requester
                             * just sent us an ARP request — the switch has already
                             * learned its port from that frame.  More importantly,
                             * many host stacks (macOS, BSD) only update their ARP
                             * cache from unicast replies; a broadcast reply is
                             * accepted as a frame but not used to fill the cache,
                             * causing repeated ARP retries and "Host is down" ping
                             * errors.  Switch to unicast to match RFC 826. */
                            for (int _i=0;_i<6;_i++) reply[_i]   = g_rx_frame[6+_i];
                            for (int _i=0;_i<6;_i++) reply[6+_i] = g_genet_mac[_i];
                            reply[12]=0x08; reply[13]=0x06;
                            reply[14]=0x00; reply[15]=0x01;
                            reply[16]=0x08; reply[17]=0x00;
                            reply[18]=6;    reply[19]=4;
                            reply[20]=0x00; reply[21]=0x02;
                            for (int _i=0;_i<6;_i++) reply[22+_i] = g_genet_mac[_i];
                            for (int _i=0;_i<4;_i++) reply[28+_i] = our_ip[_i];
                            for (int _i=0;_i<6;_i++) reply[32+_i] = g_rx_frame[22+_i];
                            for (int _i=0;_i<4;_i++) reply[38+_i] = g_rx_frame[28+_i];
                            /* boot329: TX diagnostics — confirm TDMA consuming */
                            uint32_t tx_prod_pre=0, tx_cons_pre=0;
                            genet_tx_diag(&tx_prod_pre, &tx_cons_pre);
                            int arp_rc = genet_send(reply, 60);
                            uint32_t tx_prod_post=0, tx_cons_post=0;
                            genet_tx_diag(&tx_prod_post, &tx_cons_post);
                            /* boot339: split long debug_print into three
                             * calls of ≤7 args each to avoid AArch64 stack
                             * spill that printed SHA/SPA/TPA as zeros.      */
                            debug_print(
                                "[ARP] reply rc=%d"
                                " tx_prod=%u->%u tx_cons=%u->%u\n",
                                arp_rc,
                                (unsigned)tx_prod_pre,  (unsigned)tx_prod_post,
                                (unsigned)tx_cons_pre,  (unsigned)tx_cons_post);
                            debug_print(
                                "[ARP]  SHA=%02x:%02x:%02x:%02x:%02x:%02x\n",
                                g_genet_mac[0], g_genet_mac[1],
                                g_genet_mac[2], g_genet_mac[3],
                                g_genet_mac[4], g_genet_mac[5]);
                            debug_print(
                                "[ARP]  SPA=%d.%d.%d.%d"
                                " TPA=%d.%d.%d.%d\n",
                                our_ip[0], our_ip[1],
                                our_ip[2], our_ip[3],
                                g_rx_frame[28], g_rx_frame[29],
                                g_rx_frame[30], g_rx_frame[31]);
                        }
                    }

                    /* boot334: DHCP reply (UDP port 68).
                     * IP frame: [23]=proto [14]=ver/IHL
                     * IHL lets us find the UDP header even with options.
                     * DHCP: op=BOOTREPLY(2), xid must match g_dhcp_xid,
                     * magic cookie at dhcp_off+236.                     */
                    if (g_rx_frame[23] == 0x11 && flen >= 300) {
                        int ihl2     = (g_rx_frame[14] & 0x0f) * 4;
                        int udp_off  = 14 + ihl2;
                        int doff     = udp_off + 8;   /* DHCP payload  */
                        int udp_dp   = (g_rx_frame[udp_off+2]<<8)|
                                        g_rx_frame[udp_off+3];
                        if (udp_dp == 68 &&
                            flen >= doff + 240 &&
                            g_rx_frame[doff+0] == 0x02 &&     /* BOOTREPLY */
                            g_rx_frame[doff+4] == g_dhcp_xid[0] &&
                            g_rx_frame[doff+5] == g_dhcp_xid[1] &&
                            g_rx_frame[doff+6] == g_dhcp_xid[2] &&
                            g_rx_frame[doff+7] == g_dhcp_xid[3] &&
                            g_rx_frame[doff+236] == 0x63 &&
                            g_rx_frame[doff+237] == 0x82 &&
                            g_rx_frame[doff+238] == 0x53 &&
                            g_rx_frame[doff+239] == 0x63) {
                            /* Parse options: tag 53=msg type, 54=server ID */
                            uint8_t dmsg = 0;
                            uint8_t dsrv[4] = {0,0,0,0};
                            int p = doff + 240;
                            while (p < flen && g_rx_frame[p] != 0xff) {
                                uint8_t tag = g_rx_frame[p++];
                                if (tag == 0) continue;
                                if (p >= flen) break;
                                uint8_t tl = g_rx_frame[p++];
                                if (tag == 53 && tl >= 1)
                                    dmsg = g_rx_frame[p];
                                if (tag == 54 && tl >= 4) {
                                    dsrv[0]=g_rx_frame[p];
                                    dsrv[1]=g_rx_frame[p+1];
                                    dsrv[2]=g_rx_frame[p+2];
                                    dsrv[3]=g_rx_frame[p+3];
                                }
                                p += tl;
                            }
                            if (dmsg == DHCP_MSG_OFFER &&
                                g_dhcp_st == DHCP_ST_DISCOVER) {
                                /* OFFER received — extract yiaddr, send REQUEST */
                                for (int _i=0;_i<4;_i++)
                                    g_dhcp_yiaddr[_i]=g_rx_frame[doff+16+_i];
                                for (int _i=0;_i<4;_i++)
                                    g_dhcp_srvid[_i]=dsrv[_i];
                                debug_print("[DHCP] OFFER %d.%d.%d.%d"
                                    " from %d.%d.%d.%d\n",
                                    g_dhcp_yiaddr[0],g_dhcp_yiaddr[1],
                                    g_dhcp_yiaddr[2],g_dhcp_yiaddr[3],
                                    g_dhcp_srvid[0], g_dhcp_srvid[1],
                                    g_dhcp_srvid[2], g_dhcp_srvid[3]);
                                static uint8_t dreq[342];
                                dhcp_make(dreq, DHCP_REQUEST);
                                genet_send(dreq, 342);
                                g_dhcp_st    = DHCP_ST_REQUEST;
                                g_dhcp_tries = 0;
                                last_dhcp    = wimp_ms();
                            } else if (dmsg == DHCP_MSG_ACK &&
                                       g_dhcp_st == DHCP_ST_REQUEST) {
                                /* ACK — bind the offered IP */
                                for (int _i=0;_i<4;_i++)
                                    our_ip[_i]=g_rx_frame[doff+16+_i];
                                g_dhcp_st = DHCP_ST_BOUND;
                                debug_print("[DHCP] BOUND — IP"
                                    " %d.%d.%d.%d\n",
                                    our_ip[0],our_ip[1],
                                    our_ip[2],our_ip[3]);
                                /* Gratuitous ARP with our new IP */
                                static uint8_t garp[60];
                                for (int _i=0;_i<60;_i++) garp[_i]=0;
                                for (int _i=0;_i<6;_i++) garp[_i]=0xff;
                                for (int _i=0;_i<6;_i++) garp[6+_i]=g_genet_mac[_i];
                                garp[12]=0x08; garp[13]=0x06;
                                garp[14]=0x00; garp[15]=0x01;
                                garp[16]=0x08; garp[17]=0x00;
                                garp[18]=6;    garp[19]=4;
                                garp[20]=0x00; garp[21]=0x02;
                                for (int _i=0;_i<6;_i++) garp[22+_i]=g_genet_mac[_i];
                                for (int _i=0;_i<4;_i++) garp[28+_i]=our_ip[_i];
                                garp[32]=0xff; garp[33]=0xff; garp[34]=0xff;
                                garp[35]=0xff; garp[36]=0xff; garp[37]=0xff;
                                for (int _i=0;_i<4;_i++) garp[38+_i]=our_ip[_i];
                                genet_send(garp, 60);
                            } else if (dmsg == DHCP_MSG_NAK) {
                                debug_print("[DHCP] NAK — restarting\n");
                                g_dhcp_st    = DHCP_ST_DISCOVER;
                                g_dhcp_tries = 0;
                                last_dhcp    = wimp_ms();
                            }
                        }
                    }

                    /* boot326: ICMP echo reply (ping).
                     * IPv4 frame layout:
                     *  [14]    version+IHL (0x45 = v4, 20-byte header)
                     *  [23]    protocol (0x01 = ICMP)
                     *  [24-25] IP header checksum
                     *  [26-29] src IP
                     *  [30-33] dst IP
                     *  [34]    ICMP type (8=echo request, 0=echo reply)
                     *  [35]    ICMP code
                     *  [36-37] ICMP checksum
                     *  [38+]   ICMP id, seq, data (echo back verbatim)  */
                    if (etype == 0x0800 && flen >= 42) {
                        int ihl = (g_rx_frame[14] & 0x0f) * 4;
                        int icmp_off = 14 + ihl;
                        /* ICMP echo request to our IP? */
                        if (g_rx_frame[23] == 0x01 &&          /* ICMP */
                            g_rx_frame[icmp_off] == 0x08 &&    /* echo request */
                            g_rx_frame[icmp_off+1] == 0x00 &&  /* code 0 */
                            g_rx_frame[30] == our_ip[0] &&
                            g_rx_frame[31] == our_ip[1] &&
                            g_rx_frame[32] == our_ip[2] &&
                            g_rx_frame[33] == our_ip[3]) {

                            static uint8_t pkt[1500];
                            int ip_total = (g_rx_frame[16]<<8)|g_rx_frame[17];
                            int pkt_len  = 14 + ip_total;
                            if (pkt_len > flen) pkt_len = flen;

                            /* Copy entire frame then patch fields */
                            for (int _i=0;_i<pkt_len;_i++) pkt[_i]=g_rx_frame[_i];

                            /* Ethernet: swap src/dst MAC */
                            for (int _i=0;_i<6;_i++) {
                                pkt[_i]   = g_rx_frame[6+_i]; /* dst = sender */
                                pkt[6+_i] = g_genet_mac[_i];  /* src = us    */
                            }
                            /* IP: swap src/dst, set TTL=64, clear+redo checksum */
                            for (int _i=0;_i<4;_i++) {
                                pkt[26+_i] = our_ip[_i];          /* src = us   */
                                pkt[30+_i] = g_rx_frame[26+_i];   /* dst = them */
                            }
                            pkt[22] = 64;    /* TTL */
                            pkt[24] = 0; pkt[25] = 0; /* zero checksum field */
                            /* IP header checksum (one's complement sum) */
                            uint32_t ck = 0;
                            for (int _i=14; _i<14+ihl; _i+=2)
                                ck += (uint32_t)((pkt[_i]<<8)|pkt[_i+1]);
                            while (ck>>16) ck=(ck&0xffff)+(ck>>16);
                            ck = ~ck & 0xffff;
                            pkt[24]=(uint8_t)(ck>>8); pkt[25]=(uint8_t)ck;

                            /* ICMP: type=0 (reply), clear+redo checksum */
                            pkt[icmp_off] = 0x00;
                            pkt[icmp_off+2] = 0; pkt[icmp_off+3] = 0;
                            int icmp_len = ip_total - ihl;
                            ck = 0;
                            for (int _i=0; _i<icmp_len; _i+=2)
                                ck += (uint32_t)((pkt[icmp_off+_i]<<8)|
                                      (_i+1<icmp_len?pkt[icmp_off+_i+1]:0));
                            while (ck>>16) ck=(ck&0xffff)+(ck>>16);
                            ck = ~ck & 0xffff;
                            pkt[icmp_off+2]=(uint8_t)(ck>>8);
                            pkt[icmp_off+3]=(uint8_t)ck;

                            genet_send(pkt, pkt_len);
                            debug_print("[ICMP] echo reply → "
                                "%d.%d.%d.%d seq=%d\n",
                                g_rx_frame[26],g_rx_frame[27],
                                g_rx_frame[28],g_rx_frame[29],
                                (g_rx_frame[icmp_off+6]<<8)|
                                 g_rx_frame[icmp_off+7]);
                        }
                    }
                }
            }
        }

        /* ── Mouse input (xHCI path): ~250 Hz (4 ms), non-blocking.
         * boot285: High-frequency mouse poll keeps the interrupt TRB
         * perpetually queued and catches each 7 ms mouse report within
         * one poll window.  Deltas stay small → smooth cursor tracking.
         * hid_poll_mice() calls usb_interrupt_transfer(timeout=0) which
         * does a single event-ring check and returns immediately (<5 µs)
         * when no data is ready — WIMP is never stalled.                */
        {
            uint32_t now_m = wimp_ms();
            if ((now_m - last_mouse) >= 4u) {
                last_mouse = now_m;
                if (hid_poll_mice) hid_poll_mice();
            }
        }

        /* ── Keyboard input (xHCI path): ~60 Hz (16 ms), blocking 8 ms.
         * boot285: hid_poll_all() now skips mice (handled above).
         * boot275: raised from 10 Hz so brief keypresses are caught.   */
        uint32_t now_hid = wimp_ms();
        if ((now_hid - last_hid) >= 16u) {
            last_hid = now_hid;
            if (hid_poll_all) hid_poll_all();
        }

        /* ── DWC2 OTG HID input: ~60 Hz (16 ms) ──────────────────────── */
        {
            uint32_t now_d = wimp_ms();
            if ((now_d - last_dwc2) >= 16u) {
                last_dwc2 = now_d;
                if (dwc2_hid_poll) dwc2_hid_poll();
            }
        }

        /* ── Keyboard events: drain queue, break on ESC ───────────────── */
        if (keyboard_poll) {
            uint8_t ev[4];
            while (keyboard_poll(ev)) {
                uint8_t kcode = ev[0];  /* HID key_code */
                uint8_t kchar = ev[1];  /* ASCII key_char */
                debug_print("[WIMP] key code=0x%02x char=0x%02x\n", kcode, kchar);
                if (kchar == 0x1B) {    /* ESC key */
                    if (con_puts) con_puts("\n[Loop stopped by ESC]\n");
                    debug_print("[WIMP] ESC pressed — loop halted\n");
                    goto loop_exit;
                }
            }
        }

        /* ── Mouse cursor: update position if moved ───────────────────── */
        if (mouse_get_pos && cursor_update) {
            int16_t mx, my;
            mouse_get_pos(&mx, &my);
            if (mx != last_mx || my != last_my) {
                last_mx = mx;
                last_my = my;
                cursor_update((int)mx, (int)my);
            }
        }

        /* ── Visual heartbeat: blink 32×32 square at screen centre ──── */
        {
            uint32_t now_v = wimp_ms();
            if ((now_v - last_visual) >= 500u) {
                last_visual = now_v;
                visual_on   = !visual_on;
                if (fb.valid && fb.base) {
                    int bx = (int)(fb.width  / 2u) - 16;
                    int by = (int)(fb.height / 2u) - 16;
                    pixel_t col = visual_on
                        ? RGB(30, 220, 30)
                        : RGB(48,  48, 48);
                    fb_fill_rect(bx, by, 32, 32, col);
                }
            }
        }

        /* ── USB hotplug: 500 ms ───────────────────────────────────────── */
        {
            uint32_t now = wimp_ms();
            if ((now - last_hotplug) >= 500u) {
                last_hotplug = now;
                if (hub_poll_hotplug)   hub_poll_hotplug();
                if (xhci_check_hotplug) xhci_check_hotplug();
            }
        }

        yield();
    }
loop_exit:
    /* Cursor stays at current position; system idles in WFE */
    while (1) { __asm__ volatile("wfe"); }
}

void paint_task(void)    { while(1) { yield(); } }
void netsurf_task(void)  { while(1) { yield(); } }

void mmu_free_pagetable(task_t *task) { (void)task; }
void mmu_free_usermemory(task_t *task) { (void)task; }

/* Dummy symbols - declared in kernel.c */
extern int nr_cpus;
extern task_t *current_task;
extern char exception_vectors[];

/* Atomic operations stub */
int __aarch64_ldadd4_acq_rel(int val, int *ptr) {
    int old = *ptr;
    *ptr += val;
    return old;
}

/* Get current CPU ID from MPIDR_EL1 */
int get_cpu_id(void) {
    uint64_t mpidr;
    __asm__ volatile ("mrs %0, mpidr_el1" : "=r"(mpidr));
    return (int)(mpidr & 0xFF);
}

/* strncpy_safe is defined as a static inline in kernel/error.h */
