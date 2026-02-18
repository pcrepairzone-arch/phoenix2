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
    /* LED: 4 quick blinks = entering gpu_init */
    led_signal_gpu_start();

    debug_print("GPU: start\n");

    int ok = 0;
    for (unsigned i = 0; i < sizeof(modes)/sizeof(modes[0]); i++) {
        debug_print("GPU: trying %ux%u\n", modes[i].w, modes[i].h);
        if (fb_init(modes[i].w, modes[i].h) == 0) { ok = 1; break; }
    }

    if (!ok) {
        debug_print("GPU: all modes failed\n");
        led_signal_hang();  /* Continuous slow blink */
        return;
    }

    /* LED: 5 quick blinks = framebuffer acquired */
    led_signal_gpu_ok();

    /* Title bar */
    fb_fill_rect(0, 0, fb.width, 32, RGB(0, 0, 160));
    fb_draw_string_scaled(8, 6, "Phoenix RISC OS", COL_WHITE, RGB(0,0,160), 2);

    /* Console */
    con_init();
    con_set_colours(RGB(0,255,0), COL_BLACK);
    con_printf("Phoenix RISC OS kernel\n");
    con_printf("Video: %dx%d 32bpp\n", fb.width, fb.height);
    con_set_colours(COL_WHITE, COL_BLACK);
    con_printf("-----------------------------------\n");

    fb_mark_ready();
    debug_print("GPU: online %dx%d\n", fb.width, fb.height);
}
