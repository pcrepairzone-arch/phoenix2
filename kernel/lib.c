/*
 * lib.c - Basic library functions
 */
#include "kernel.h"
#include <stdarg.h>
/* boot265: visual heartbeat — draw to framebuffer without fb_mark_ready dependency */
#include "drivers/gpu/framebuffer.h"

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
        fb_draw_string(bx, by, "boot301  BCM2711 / Cortex-A72",
                       COL_GREY, COL_DARK_GREY);
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
    uint32_t last_dwc2    = wimp_ms(); /* boot267: DWC2 HID poll timestamp */
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
                debug_print("[WIMP] alive t=%dms stalls=%u\n",
                            (int)now_b, (unsigned)g_uart_stalls);
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
