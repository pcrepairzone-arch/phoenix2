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

/* ── DHCP — delegated to net/dhcp.c (PhoenixDHCP module) ─────────────────
 * boot345: extracted from inline implementation.
 * All state, frame building, and retransmit logic live in net/dhcp.c.
 * wimp_task() just calls dhcp_start(), dhcp_tick(), and dhcp_rx().
 * g_our_ip is exported by dhcp.c and used directly below for ARP/ICMP.    */
#include "../net/dhcp.h"

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
        fb_draw_string(bx, by, "boot369  BCM2711 / Cortex-A72",
                       COL_GREY, COL_DARK_GREY);
    }

    /* boot369: pre-loop link wait removed.
     * PhoenixDHCP module_init() (called from module_init_all() before
     * wimp_task runs) now handles link-up wait, genet_apply_link(), and the
     * full DISCOVER→BOUND exchange.  By the time we reach this point the IP
     * is already bound and g_our_ip is valid.  No link polling needed here. */
    debug_print("[WIMP] task running — IP=%d.%d.%d.%d\n",
                g_our_ip[0], g_our_ip[1], g_our_ip[2], g_our_ip[3]);

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
    /* boot369: net_link_prev starts at 1 (UP) since PhoenixDHCP module_init
     * confirmed link before returning.  Heartbeat still monitors for cable
     * disconnect/reconnect and re-applies PHY speed on reconnect.          */
    int      net_link_prev = 1;
    static uint8_t g_rx_frame[GENET_MAX_FRAME]; /* RX frame buffer */
    /* boot345: our IP read from g_our_ip (exported by net/dhcp.c).
     * ARP and ICMP handlers use g_our_ip directly — no local copy needed.  */
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
                debug_print("[WIMP] alive t=%dms stalls=%u pidx=%u rx=%u mib=%u rfcs=%u\n",
                            (int)now_b, (unsigned)g_uart_stalls,
                            (unsigned)genet_rx_pidx_raw(),
                            (unsigned)genet_rx_count_raw(),
                            (unsigned)genet_tx_cons_raw(),
                            (unsigned)genet_rx_fcs_raw());
                /* boot303/304: log link state changes; send grat ARP on UP */
                int lnk = genet_link_up();
                if (lnk != net_link_prev) {
                    net_link_prev = lnk;
                    debug_print("[NET] link %s\n", lnk ? "UP" : "DOWN");
                    if (lnk) {
                        /* boot308/369: re-apply PHY speed on reconnect.
                         * If the cable was unplugged and re-plugged, UMAC
                         * speed must be re-matched to the newly-negotiated
                         * PHY rate.  DHCP is NOT re-triggered here — that
                         * is future work (PhoenixDHCP reconnect service).  */
                        genet_apply_link();
                    }
                }
            }
        }

        /* boot369: dhcp_tick() removed from wimp loop.
         * DHCP is fully handled by PhoenixDHCP.module_init before wimp_task
         * starts.  Once BOUND the state machine sits in DHCP_ST_BOUND and
         * dhcp_tick() would return immediately anyway — but cleaner to remove
         * it entirely so the module boundary is explicit.                   */

        /* ── GENET RX poll: 4 ms — drain entire ring on each tick ─────── */
        {
            uint32_t now_n = wimp_ms();
            if ((now_n - last_net) >= 4u) {
                last_net = now_n;
                /* boot347: drain ALL frames queued in the RDMA ring, not just
                 * one per 4 ms tick.  Previously a single genet_poll_rx() call
                 * left multiple frames (e.g. DHCP OFFERs buried behind IPv6
                 * multicast bursts) stuck in the ring for seconds.
                 *
                 * Loop condition:
                 *  flen > 0  → valid frame delivered, keep draining
                 *  flen == 0 → no frame OR error/oversized frame was skipped
                 *              (consumer index still advanced);
                 *              genet_rx_available() > 0 means ring not empty
                 *  flen == -1 → driver not init → stop
                 *
                 * We drain until (flen < 0) OR (flen == 0 AND ring empty).   */
                int flen;
                while ((flen = genet_poll_rx(g_rx_frame, GENET_MAX_FRAME)) >= 0
                       && (flen > 0 || genet_rx_available() > 0)) {
                if (flen > 13) {
                    uint16_t etype = (uint16_t)
                        ((g_rx_frame[12] << 8) | g_rx_frame[13]);

                    /* ARP reply handler — answer who-has for our IP */
                    if (etype == 0x0806 && flen >= 42) {
                        uint16_t op = (uint16_t)
                            ((g_rx_frame[20] << 8) | g_rx_frame[21]);
                        if (op == 1 &&
                            g_rx_frame[38] == g_our_ip[0] &&
                            g_rx_frame[39] == g_our_ip[1] &&
                            g_rx_frame[40] == g_our_ip[2] &&
                            g_rx_frame[41] == g_our_ip[3]) {
                            static uint8_t reply[60];
                            for (int _i=0;_i<60;_i++) reply[_i]=0;
                            /* boot368: unicast dst — send ARP reply directly to
                             * the requester's Ethernet source MAC (frame[6..11]).
                             * boot329 used broadcast (ff:ff:ff:ff:ff:ff) to work
                             * around potential unknown-unicast drops, but that
                             * caused DUP! replies on the pinger and flooded the
                             * switch, driving RTT spikes.  By the time we receive
                             * a who-has, the requester is already reachable (DHCP
                             * completed, grat-ARP sent) so unicast is correct. */
                            for (int _i=0;_i<6;_i++) reply[_i]   = g_rx_frame[6+_i];
                            for (int _i=0;_i<6;_i++) reply[6+_i] = g_genet_mac[_i];
                            reply[12]=0x08; reply[13]=0x06;
                            reply[14]=0x00; reply[15]=0x01;
                            reply[16]=0x08; reply[17]=0x00;
                            reply[18]=6;    reply[19]=4;
                            reply[20]=0x00; reply[21]=0x02;
                            for (int _i=0;_i<6;_i++) reply[22+_i] = g_genet_mac[_i];
                            for (int _i=0;_i<4;_i++) reply[28+_i] = g_our_ip[_i];
                            for (int _i=0;_i<6;_i++) reply[32+_i] = g_rx_frame[22+_i];
                            for (int _i=0;_i<4;_i++) reply[38+_i] = g_rx_frame[28+_i];
                            /* boot329: TX diagnostics — confirm TDMA consuming */
                            uint32_t tx_prod_pre=0, tx_cons_pre=0;
                            genet_tx_diag(&tx_prod_pre, &tx_cons_pre);
                            int arp_rc = genet_send(reply, 60);
                            uint32_t tx_prod_post=0, tx_cons_post=0;
                            genet_tx_diag(&tx_prod_post, &tx_cons_post);
                            /* boot352: split into ≤7-arg calls — AArch64 bare-metal
                             * debug_print puts args 8+ on the stack; va_arg can't
                             * read stack args → they print as 0. Split prevents
                             * SHA=dc:a6:32:00:00:00 / SPA=0.0.0.0 in the log.  */
                            debug_print(
                                "[ARP] reply: rc=%d tx_prod=%u->%u tx_cons=%u->%u\n",
                                arp_rc,
                                (unsigned)tx_prod_pre,  (unsigned)tx_prod_post,
                                (unsigned)tx_cons_pre,  (unsigned)tx_cons_post);
                            debug_print(
                                "[ARP]   SHA=%02x:%02x:%02x:%02x:%02x:%02x\n",
                                g_genet_mac[0], g_genet_mac[1], g_genet_mac[2],
                                g_genet_mac[3], g_genet_mac[4], g_genet_mac[5]);
                            debug_print("[ARP]   SPA=%d.%d.%d.%d\n",
                                g_our_ip[0], g_our_ip[1],
                                g_our_ip[2], g_our_ip[3]);
                            debug_print("[ARP]   TPA=%d.%d.%d.%d\n",
                                g_rx_frame[28], g_rx_frame[29],
                                g_rx_frame[30], g_rx_frame[31]);
                        }
                    }

                    /* boot369: dhcp_rx() removed from wimp RX dispatch.
                     * DHCP exchange is complete before wimp_task starts.
                     * Frames arriving here are not DHCP — they are ARP,
                     * ICMP ping, or background multicast traffic.          */

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
                            g_rx_frame[30] == g_our_ip[0] &&
                            g_rx_frame[31] == g_our_ip[1] &&
                            g_rx_frame[32] == g_our_ip[2] &&
                            g_rx_frame[33] == g_our_ip[3]) {

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
                                pkt[26+_i] = g_our_ip[_i];          /* src = us   */
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
                } /* if (flen > 13) */
                } /* while drain loop */
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
/* strncpy_safe is defined as a static inline in kernel/error.h */
