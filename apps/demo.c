/*
 * demo.c - Interactive Demo for Phoenix RISC OS
 * Shows colored boxes and prepares for mouse/keyboard input
 */

#include "kernel.h"

extern framebuffer_info_t fb_info;

/* Draw a filled rectangle */
void draw_box(int x, int y, int width, int height, uint32_t color)
{
    if (!fb_info.base) return;
    
    uint32_t *fb = (uint32_t *)fb_info.base;
    
    for (int dy = 0; dy < height; dy++) {
        for (int dx = 0; dx < width; dx++) {
            int px = x + dx;
            int py = y + dy;
            
            if (px >= 0 && px < fb_info.width && 
                py >= 0 && py < fb_info.height) {
                fb[py * (fb_info.pitch / 4) + px] = color;
            }
        }
    }
}

/* Draw text at position (simple, using 8x8 font) */
void draw_text(int x, int y, const char *text, uint32_t color)
{
    extern void draw_string(int x, int y, const char *str, uint32_t color);
    draw_string(x, y, text, color);
}

/* Draw a box with border */
void draw_button(int x, int y, int width, int height, 
                 uint32_t bg_color, uint32_t border_color,
                 const char *label)
{
    /* Draw border */
    draw_box(x, y, width, 2, border_color);                    /* Top */
    draw_box(x, y + height - 2, width, 2, border_color);       /* Bottom */
    draw_box(x, y, 2, height, border_color);                   /* Left */
    draw_box(x + width - 2, y, 2, height, border_color);       /* Right */
    
    /* Draw background */
    draw_box(x + 2, y + 2, width - 4, height - 4, bg_color);
    
    /* Draw label (centered) */
    int label_len = 0;
    while (label[label_len]) label_len++;
    
    int text_x = x + (width - label_len * 8) / 2;
    int text_y = y + (height - 8) / 2;
    
    draw_text(text_x, text_y, label, 0xFFFFFFFF); /* White text */
}

/* Demo application */
void demo_app(void)
{
    debug_print("\n[DEMO] Starting interactive demo\n");
    
    /* Colors (RGBA format) */
    uint32_t COLOR_GREEN  = 0xFF00AA00;  /* Bright green */
    uint32_t COLOR_BLUE   = 0xFF0066FF;  /* Bright blue */
    uint32_t COLOR_RED    = 0xFFFF3333;  /* Bright red */
    uint32_t COLOR_YELLOW = 0xFFFFCC00;  /* Yellow */
    uint32_t COLOR_BORDER = 0xFF333333;  /* Dark grey */
    
    /* Button positions (centered on screen) */
    int center_x = fb_info.width / 2;
    int center_y = fb_info.height / 2;
    
    int button_width = 200;
    int button_height = 60;
    int button_spacing = 20;
    
    /* Draw three buttons */
    
    /* Button 1: Keyboard (top) */
    draw_button(center_x - button_width / 2, 
                center_y - button_height - button_spacing,
                button_width, button_height,
                COLOR_GREEN, COLOR_BORDER,
                "KEYBOARD");
    
    /* Button 2: Mouse (middle) */
    draw_button(center_x - button_width / 2,
                center_y,
                button_width, button_height,
                COLOR_BLUE, COLOR_BORDER,
                "MOUSE");
    
    /* Button 3: Exit (bottom) */
    draw_button(center_x - button_width / 2,
                center_y + button_height + button_spacing,
                button_width, button_height,
                COLOR_RED, COLOR_BORDER,
                "EXIT");
    
    /* Draw status text */
    draw_text(50, 150, "Phoenix RISC OS - Interactive Demo", COLOR_YELLOW);
    draw_text(50, 170, "USB Keyboard: In Progress", 0xFFFFFFFF);
    draw_text(50, 190, "USB Mouse: In Progress", 0xFFFFFFFF);
    draw_text(50, 210, "Click detection: Coming soon!", 0xFFFFFFFF);
    
    /* Draw animated cursor placeholder */
    int cursor_x = center_x;
    int cursor_y = center_y - 100;
    
    debug_print("[DEMO] Boxes drawn at screen center\n");
    debug_print("[DEMO] Green box = Keyboard (future)\n");
    debug_print("[DEMO] Blue box = Mouse (future)\n");
    debug_print("[DEMO] Red box = Exit (future)\n");
    
    /* Animation loop - pulse the cursor */
    int frame = 0;
    while (1) {
        /* Erase old cursor */
        draw_box(cursor_x - 5, cursor_y - 5, 11, 11, 0xFFBBBBBB);
        
        /* Calculate pulse size */
        int pulse = 5 + (frame % 10) / 2;
        
        /* Draw new cursor */
        draw_box(cursor_x - pulse, cursor_y - pulse, 
                 pulse * 2 + 1, pulse * 2 + 1, COLOR_YELLOW);
        
        /* Delay */
        for (volatile int i = 0; i < 1000000; i++);
        
        frame++;
        
        /* Every 100 frames, show we're alive */
        if (frame % 100 == 0) {
            debug_print("[DEMO] Frame %d - waiting for USB input\n", frame);
        }
    }
}
