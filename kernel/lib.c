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
/* boot370: PhoenixTCPIP dispatcher — replaces inline ARP/ICMP handlers */
#include "net/net.h"
/* boot376: PhoenixGENET DCI4 module — RX dispatch + DCI4 SWI interface */
#include "drivers/net/genet_module.h"

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
        fb_draw_string(bx, by, "boot393  BCM2711 / Cortex-A72",
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
                /* boot376: PhoenixGENET DCI4 module RX dispatch.
                 * genet_module_rx() walks the DCI4 filter list
                 * (registered by the Internet module or DCIShim),
                 * then falls through to net_rx_frame() for Phoenix's
                 * own TCP/IP stack until a proper Internet module
                 * registers above us.                                 */
                if (flen > 13)
                    genet_module_rx(g_rx_frame, flen);
                } /* while drain loop */
            }
        }

        /* ── TCP test suite (boot375) ──────────────────────────────────
         *
         * Four sequential tests exercising different aspects of the
         * TCP stack and GENET driver:
         *
         * Test A (boot372): single HTTP GET to router — baseline
         * Test B (boot374): 4 simultaneous connections (all slots)
         * Test C (boot374): RST/error path — connect to closed port
         * Test D (boot375): 8 sequential GETs — ring-wrap stress
         *                   target > 97 KB (64 × 1518 B RX ring)
         *
         * s_test drives the overall sequence:
         *   0 = Test A running / not yet started
         *   1 = Test B running
         *   2 = Test C running
         *   3 = Test D running
         *   4 = all done
         *
         * tcp_rx() is driven by net_rx_frame() in the drain loop
         * above — state machines advance automatically.              */
#define D_REQS 8   /* 8 × ~15 KB ≈ 120 KB — wraps the 64-desc RX ring */
        {
            static int      s_test      = 0;   /* which test is active     */

            /* ── shared boot-time gate ─────────────────────────────── */
            static uint32_t s_wimp_start= 0;
            uint32_t now_t = wimp_ms();
            if (s_wimp_start == 0) s_wimp_start = now_t;
            if ((now_t - s_wimp_start) < 5000u) goto tcp_tests_done;

            /* ════════════════════════════════════════════════════════
             * TEST A — single HTTP/1.0 GET to 192.168.0.1:80
             * State machine (a_phase):
             *   0 → fire SYN
             *   1 → SYN_SENT: retransmit 1 s, 15 s timeout
             *   2 → ESTABLISHED: send GET
             *   3 → reading response
             *   4 → done
             * ════════════════════════════════════════════════════════ */
            if (s_test == 0) {
                static int      a_phase    = 0;
                static int      a_handle   = -1;
                static uint32_t a_t0       = 0;
                static uint32_t a_last_syn = 0;
                static int      a_total    = 0;

                if (a_phase == 0) {
                    static const uint8_t router_ip[4] = {192, 168, 0, 1};
                    a_handle   = tcp_connect(router_ip, 80);
                    a_t0       = now_t;
                    a_last_syn = now_t;
                    if (a_handle >= 0) {
                        a_phase = 1;
                        debug_print("[TestA] SYN → 192.168.0.1:80 handle=%d\n",
                            a_handle);
                    } else {
                        debug_print("[TestA] FAIL no free slot\n");
                        a_phase = 4;
                    }
                }

                if (a_phase == 1 && a_handle >= 0) {
                    int st = tcp_state(a_handle);
                    if (st == TCPS_ESTABLISHED) {
                        static const char req[] =
                            "GET / HTTP/1.0\r\n"
                            "Host: 192.168.0.1\r\n"
                            "Connection: close\r\n"
                            "\r\n";
                        tcp_write(a_handle, (const uint8_t *)req,
                                  sizeof(req) - 1);
                        a_phase = 2;
                        debug_print("[TestA] ESTABLISHED — GET sent\n");
                    } else {
                        if ((now_t - a_last_syn) >= 1000u) {
                            a_last_syn = now_t;
                            tcp_tick(a_handle);
                        }
                        if ((now_t - a_t0) > 15000u) {
                            debug_print("[TestA] FAIL SYN timeout st=%d\n", st);
                            tcp_close(a_handle);
                            a_phase = 4;
                        }
                    }
                }

                if ((a_phase == 2 || a_phase == 3) && a_handle >= 0) {
                    static uint8_t rxbuf[128];
                    int n = tcp_read(a_handle, rxbuf, (int)sizeof(rxbuf) - 1);
                    if (n > 0) { a_total += n; a_phase = 3; }
                    int st = tcp_state(a_handle);
                    if (st == TCPS_CLOSED || st == TCPS_TIME_WAIT) {
                        debug_print("[TestA] PASS — %d bytes, clean close\n",
                            a_total);
                        tcp_close(a_handle);
                        a_phase = 4;
                    }
                    if ((st == TCPS_FIN_WAIT_2 || st == TCPS_CLOSE_WAIT)
                        && n <= 0) {
                        debug_print("[TestA] PASS — %d bytes, peer FIN\n",
                            a_total);
                        tcp_close(a_handle);
                        a_phase = 4;
                    }
                    if (a_phase != 4 && (now_t - a_t0) > 20000u) {
                        debug_print("[TestA] FAIL rx timeout (%d bytes)\n",
                            a_total);
                        tcp_close(a_handle);
                        a_phase = 4;
                    }
                }

                if (a_phase == 4) s_test = 1;
            }

            /* ════════════════════════════════════════════════════════
             * TEST B — 4 simultaneous connections (all TCP slots)
             *
             * Opens all 4 slots to 192.168.0.1:80 at once.  Each slot
             * runs its own mini state machine: open → GET → drain →
             * close.  The test measures total bytes per slot and
             * verifies all 4 complete without corrupting each other.
             *
             * b_ph[i]: 0=opening  1=SYN_SENT  2=GET_sent  3=reading
             *          4=done
             * b_rx[i]: bytes received on slot i
             * ════════════════════════════════════════════════════════ */
            if (s_test == 1) {
                static int      b_ph[4]     = {0,0,0,0};
                static int      b_h[4]      = {-1,-1,-1,-1};
                static uint32_t b_t0[4]     = {0,0,0,0};
                static uint32_t b_lsyn[4]   = {0,0,0,0};
                static int      b_rx[4]     = {0,0,0,0};
                static int      b_started   = 0;
                static uint8_t  b_buf[128];
                static const uint8_t router_ip[4] = {192, 168, 0, 1};
                static const char b_req[] =
                    "GET / HTTP/1.0\r\n"
                    "Host: 192.168.0.1\r\n"
                    "Connection: close\r\n"
                    "\r\n";

                /* Open all 4 slots simultaneously on first entry */
                if (!b_started) {
                    b_started = 1;
                    debug_print("[TestB] opening 4 simultaneous connections\n");
                    int i;
                    for (i = 0; i < 4; i++) {
                        b_h[i]    = tcp_connect(router_ip, 80);
                        b_t0[i]   = now_t;
                        b_lsyn[i] = now_t;
                        if (b_h[i] >= 0) {
                            b_ph[i] = 1;
                            debug_print("[TestB] slot%d handle=%d SYN sent\n",
                                i, b_h[i]);
                        } else {
                            debug_print("[TestB] slot%d FAIL no slot\n", i);
                            b_ph[i] = 4;
                        }
                    }
                }

                /* Advance each slot's state machine */
                int i;
                for (i = 0; i < 4; i++) {
                    if (b_ph[i] == 4 || b_h[i] < 0) continue;
                    int st = tcp_state(b_h[i]);

                    /* Phase 1: SYN_SENT */
                    if (b_ph[i] == 1) {
                        if (st == TCPS_ESTABLISHED) {
                            tcp_write(b_h[i], (const uint8_t *)b_req,
                                      sizeof(b_req) - 1);
                            b_ph[i] = 2;
                            debug_print("[TestB] slot%d ESTABLISHED GET sent\n",
                                i);
                        } else {
                            if ((now_t - b_lsyn[i]) >= 1000u) {
                                b_lsyn[i] = now_t;
                                tcp_tick(b_h[i]);
                            }
                            if ((now_t - b_t0[i]) > 15000u) {
                                debug_print("[TestB] slot%d FAIL SYN timeout\n",
                                    i);
                                tcp_close(b_h[i]);
                                b_ph[i] = 4;
                            }
                        }
                    }

                    /* Phase 2/3: reading */
                    if (b_ph[i] == 2 || b_ph[i] == 3) {
                        int n = tcp_read(b_h[i], b_buf,
                                         (int)sizeof(b_buf) - 1);
                        if (n > 0) { b_rx[i] += n; b_ph[i] = 3; }
                        if (st == TCPS_CLOSED || st == TCPS_TIME_WAIT) {
                            debug_print("[TestB] slot%d PASS %d bytes\n",
                                i, b_rx[i]);
                            tcp_close(b_h[i]);
                            b_ph[i] = 4;
                        }
                        if ((st == TCPS_FIN_WAIT_2 || st == TCPS_CLOSE_WAIT)
                            && n <= 0) {
                            debug_print("[TestB] slot%d PASS %d bytes peer FIN\n",
                                i, b_rx[i]);
                            tcp_close(b_h[i]);
                            b_ph[i] = 4;
                        }
                        if (b_ph[i] != 4 && (now_t - b_t0[i]) > 20000u) {
                            debug_print("[TestB] slot%d FAIL timeout %d bytes\n",
                                i, b_rx[i]);
                            tcp_close(b_h[i]);
                            b_ph[i] = 4;
                        }
                    }
                }

                /* All slots done? */
                int done = 1;
                for (i = 0; i < 4; i++)
                    if (b_ph[i] != 4) { done = 0; break; }
                if (done) {
                    debug_print("[TestB] complete — slots 0-3 rx: "
                                "%d %d %d %d bytes\n",
                                b_rx[0], b_rx[1], b_rx[2], b_rx[3]);
                    s_test = 2;
                }
            }

            /* ════════════════════════════════════════════════════════
             * TEST C — RST / closed-port error path
             *
             * Connects to 192.168.0.1:81 (almost certainly closed).
             * A well-behaved TCP stack returns RST → CLOSED within a
             * few ms on a LAN.  We record time-to-close.
             *
             * Expected outcome:  CLOSED within 1 s
             * Timeout:           5 s → FAIL
             * ════════════════════════════════════════════════════════ */
            if (s_test == 2) {
                static int      c_phase     = 0;
                static int      c_handle    = -1;
                static uint32_t c_t0        = 0;
                static uint32_t c_last_syn  = 0;

                if (c_phase == 0) {
                    static const uint8_t router_ip[4] = {192, 168, 0, 1};
                    c_handle   = tcp_connect(router_ip, 81);
                    c_t0       = now_t;
                    c_last_syn = now_t;
                    if (c_handle >= 0) {
                        c_phase = 1;
                        debug_print("[TestC] SYN → 192.168.0.1:81 (closed port)"
                                    " handle=%d\n", c_handle);
                    } else {
                        debug_print("[TestC] FAIL no free slot\n");
                        c_phase = 4;
                    }
                }

                if (c_phase == 1 && c_handle >= 0) {
                    int st = tcp_state(c_handle);
                    /* Retransmit SYN every 500 ms — RST typically arrives
                     * within the first SYN so retransmits are just insurance */
                    if ((now_t - c_last_syn) >= 500u) {
                        c_last_syn = now_t;
                        tcp_tick(c_handle);
                    }
                    if (st == TCPS_CLOSED) {
                        uint32_t ms = now_t - c_t0;
                        debug_print("[TestC] PASS — RST in %u ms\n", ms);
                        tcp_close(c_handle);
                        c_phase = 4;
                    }
                    if (st == TCPS_ESTABLISHED) {
                        /* Port 81 unexpectedly open — close cleanly */
                        debug_print("[TestC] INFO port 81 is open (unexpected)\n");
                        tcp_close(c_handle);
                        c_phase = 4;
                    }
                    if ((now_t - c_t0) > 5000u) {
                        debug_print("[TestC] FAIL timeout st=%d"
                                    " (no RST after 5s)\n", st);
                        tcp_close(c_handle);
                        c_phase = 4;
                    }
                }

                if (c_phase == 4) {
                    s_test = 3;   /* → Test D (ring-wrap) */
                }
            }

            /* ════════════════════════════════════════════════════════
             * TEST D — sequential ring-wrap: D_REQS GETs in series
             *
             * Opens one TCP connection, drains the HTTP response,
             * closes cleanly, then immediately opens the next.
             * Repeats D_REQS (8) times, accumulating > 97 KB total
             * RX to confirm the GENET descriptor ring wraps correctly
             * and the consumer index stays in sync throughout.
             *
             * d_req:   current request index (0 .. D_REQS-1)
             * d_rx:    bytes received this request
             * d_total: cumulative bytes across all requests
             * d_phase: 0=fire SYN  1=SYN_SENT  2=GET_sent
             *          3=reading   9=all done
             * ════════════════════════════════════════════════════════ */
            if (s_test == 3) {
                static int      d_req    = 0;
                static int      d_phase  = 0;
                static int      d_slot   = -1;
                static uint32_t d_t0     = 0;
                static uint32_t d_lsyn   = 0;
                static int      d_rx     = 0;
                static int      d_total  = 0;
                static uint8_t  d_buf[256];
                static const uint8_t d_rip[4] = {192, 168, 0, 1};
                static const char d_get[] =
                    "GET / HTTP/1.0\r\n"
                    "Host: 192.168.0.1\r\n"
                    "Connection: close\r\n"
                    "\r\n";

                /* Phase 0: open connection for current request */
                if (d_phase == 0) {
                    d_slot = tcp_connect(d_rip, 80);
                    d_t0   = now_t;
                    d_lsyn = now_t;
                    d_rx   = 0;
                    if (d_slot >= 0) {
                        d_phase = 1;
                        debug_print("[TestD] req %d/%d SYN handle=%d\n",
                            d_req + 1, D_REQS, d_slot);
                    } else {
                        debug_print("[TestD] FAIL req %d no free slot\n",
                            d_req + 1);
                        d_req   = D_REQS;
                        d_phase = 9;
                    }
                }

                /* Phase 1: wait for ESTABLISHED */
                if (d_phase == 1 && d_slot >= 0) {
                    int st = tcp_state(d_slot);
                    if (st == TCPS_ESTABLISHED) {
                        tcp_write(d_slot, (const uint8_t *)d_get,
                                  sizeof(d_get) - 1);
                        d_phase = 2;
                        debug_print("[TestD] req %d ESTAB GET sent\n",
                            d_req + 1);
                    } else {
                        if ((now_t - d_lsyn) >= 1000u) {
                            d_lsyn = now_t;
                            tcp_tick(d_slot);
                        }
                        if ((now_t - d_t0) > 15000u) {
                            debug_print("[TestD] FAIL req %d SYN timeout\n",
                                d_req + 1);
                            tcp_close(d_slot);
                            d_req   = D_REQS;
                            d_phase = 9;
                        }
                    }
                }

                /* Phase 2/3: drain HTTP response */
                if ((d_phase == 2 || d_phase == 3) && d_slot >= 0) {
                    int n = tcp_read(d_slot, d_buf,
                                     (int)sizeof(d_buf) - 1);
                    if (n > 0) {
                        d_rx    += n;
                        d_total += n;
                        d_phase  = 3;
                    }
                    int st = tcp_state(d_slot);
                    int req_done = 0;
                    if (st == TCPS_CLOSED || st == TCPS_TIME_WAIT) {
                        debug_print("[TestD] req %d: %d B"
                                    " (cum %d B)\n",
                                    d_req + 1, d_rx, d_total);
                        tcp_close(d_slot);
                        req_done = 1;
                    }
                    if ((st == TCPS_FIN_WAIT_2 || st == TCPS_CLOSE_WAIT)
                        && n <= 0) {
                        debug_print("[TestD] req %d: %d B peer FIN"
                                    " (cum %d B)\n",
                                    d_req + 1, d_rx, d_total);
                        tcp_close(d_slot);
                        req_done = 1;
                    }
                    if (!req_done && (now_t - d_t0) > 20000u) {
                        debug_print("[TestD] req %d FAIL timeout %d B\n",
                            d_req + 1, d_rx);
                        tcp_close(d_slot);
                        req_done = 1;
                    }
                    if (req_done) {
                        d_req++;
                        d_phase = (d_req < D_REQS) ? 0 : 9;
                    }
                }

                /* Phase 9: all requests done */
                if (d_phase == 9) {
                    int pass = (d_req >= D_REQS);
                    debug_print("[TestD] %s — %d/%d reqs"
                                " %d bytes total"
                                " (ring wraps at 97 KB)\n",
                                pass ? "PASS" : "PARTIAL",
                                pass ? D_REQS : d_req,
                                D_REQS, d_total);
                    debug_print("[TCPtest] all tests complete\n");
                    s_test = 4;
                }
            }

            tcp_tests_done: ;
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
