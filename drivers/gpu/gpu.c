#include "kernel.h"
#include "mailbox.h"
#include "framebuffer.h"

extern void fb_mark_ready(void);
extern void led_signal_gpu_start(void);
extern void led_signal_gpu_ok(void);
extern void led_signal_hang(void);

static const struct { uint32_t w, h; } modes[] = {
    {1920,1080}, {1280,720}, {640,480}
};

void gpu_init(void)
{
    led_signal_gpu_start();  /* 4 quick blinks */
    
    debug_print("\n[GPU] Initializing video\n");
    
    int ok = 0;
    for (unsigned i = 0; i < sizeof(modes)/sizeof(modes[0]); i++) {
        debug_print("[GPU] Trying %ux%u\n", modes[i].w, modes[i].h);
        if (fb_init(modes[i].w, modes[i].h) == 0) {
            ok = 1;
            /* boot179: tell mouse layer the real screen bounds */
            extern void mouse_set_bounds(int16_t w, int16_t h) __attribute__((weak));
            if (mouse_set_bounds)
                mouse_set_bounds((int16_t)fb.width, (int16_t)fb.height);
            break;
        }
    }
    
    if (!ok) {
        debug_print("[GPU] All resolutions failed\n");
        led_signal_hang();
        return;
    }
    
    led_signal_gpu_ok();  /* 5 quick blinks = framebuffer acquired! */

    debug_print("[GPU] Framebuffer OK at %ux%u\n", fb.width, fb.height);

    /* ── Screen layout (boot169) ─────────────────────────────────────
     * RISC OS-style desktop chrome:
     *   Row 0–35:   deep-blue title bar with "Phoenix RISC OS" label
     *   Row 36–...: classic RISC OS grey background
     *   Console text overlaid in black on the grey area (boot status)
     * ──────────────────────────────────────────────────────────────── */

    /* Background: RISC OS classic grey */
    fb_clear(COL_RISCOS_GREY);

    /* Title bar — RISC OS dark blue */
    fb_fill_rect(0, 0, fb.width, 36, RGB(0, 0, 160));

    /* Title text — "Phoenix RISC OS" in white, 3× scale */
    fb_draw_string_scaled(10, 4, "Phoenix RISC OS",
                          COL_WHITE, RGB(0, 0, 160), 3);

    /* Version tag — right-aligned, 2× scale */
    fb_draw_string_scaled(fb.width - 120, 10, "v0.205",
                          RGB(180, 220, 255), RGB(0, 0, 160), 2);

    /* Divider line under title */
    fb_fill_rect(0, 36, fb.width, 2, RGB(0, 0, 100));

    /* Console — CON_MY=36+2=38 so text starts just below divider */
    con_init();
    con_set_colours(RGB(20, 20, 20), COL_RISCOS_GREY);   /* dark text on grey */

    /* First status line */
    con_printf("  Video:  %dx%d  32bpp\n", fb.width, fb.height);

    fb_mark_ready();
    debug_print("[GPU] Video online\n");
}
