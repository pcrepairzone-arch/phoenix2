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
    
    /* Clear screen to dark grey */
    fb_clear(RGB(64, 64, 64));
    
    /* Corner test markers */
    fb_fill_rect(0,        0,        32, 32, RGB(255,0,0));
    fb_fill_rect(fb.width-32, 0,     32, 32, RGB(0,255,0));
    fb_fill_rect(0,        fb.height-32, 32, 32, RGB(0,0,255));
    fb_fill_rect(fb.width-32, fb.height-32, 32, 32, RGB(255,255,255));
    
    /* Title bar */
    fb_fill_rect(0, 0, fb.width, 32, RGB(0, 0, 180));
    fb_draw_string_scaled(8, 6, "Phoenix RISC OS - IT WORKS!", 
                         COL_WHITE, RGB(0,0,180), 2);
    
    /* Console */
    con_init();
    con_set_colours(RGB(0,255,0), COL_BLACK);
    con_printf("Phoenix RISC OS Kernel\n");
    con_printf("Video: %dx%d 32bpp\n", fb.width, fb.height);
    con_printf("Mailbox: 0x40000000 cache alias\n");
    con_set_colours(COL_WHITE, COL_BLACK);
    con_printf("========================================\n");
    
    fb_mark_ready();
    debug_print("[GPU] Video online\n");
}
