/*
 * framebuffer.h - Framebuffer driver public interface
 */
#ifndef FRAMEBUFFER_H
#define FRAMEBUFFER_H

#include <stdint.h>

/* Packed 32-bit ARGB pixel */
typedef uint32_t pixel_t;

#define RGB(r,g,b)   ((pixel_t)(0xFF000000 | ((r)<<16) | ((g)<<8) | (b)))
#define RGBA(r,g,b,a) ((pixel_t)(((a)<<24) | ((r)<<16) | ((g)<<8) | (b)))

/* Standard palette */
#define COL_BLACK      RGB(0,   0,   0  )
#define COL_WHITE      RGB(255, 255, 255)
#define COL_RED        RGB(200, 30,  30 )
#define COL_GREEN      RGB(30,  200, 30 )
#define COL_BLUE       RGB(30,  30,  200)
#define COL_YELLOW     RGB(230, 200, 40 )
#define COL_CYAN       RGB(40,  200, 200)
#define COL_MAGENTA    RGB(200, 40,  200)
#define COL_GREY       RGB(128, 128, 128)
#define COL_DARK_GREY  RGB(48,  48,  48 )
#define COL_RISCOS_GREY RGB(187,187,187)  /* Classic RISC OS desktop grey */
#define COL_RISCOS_BG  RGB(224, 224, 224) /* RISC OS window background */

/* Framebuffer descriptor */
typedef struct {
    uint32_t  width;
    uint32_t  height;
    uint32_t  pitch;      /* bytes per row */
    uint32_t  bpp;        /* bits per pixel (always 32 here) */
    uint32_t *base;       /* pointer to pixel data */
    uint32_t  size;       /* total byte size */
    int       valid;
} framebuffer_t;

extern framebuffer_t fb;

/* Init & control */
int  fb_init(uint32_t width, uint32_t height);
void fb_clear(pixel_t colour);
void fb_set_pixel(int x, int y, pixel_t colour);
pixel_t fb_get_pixel(int x, int y);

/* Drawing primitives */
void fb_fill_rect(int x, int y, int w, int h, pixel_t colour);
void fb_draw_rect(int x, int y, int w, int h, pixel_t colour);
void fb_draw_line(int x0, int y0, int x1, int y1, pixel_t colour);
void fb_draw_circle(int cx, int cy, int r, pixel_t colour);

/* Text rendering */
void fb_draw_char(int x, int y, char c, pixel_t fg, pixel_t bg);
void fb_draw_string(int x, int y, const char *s, pixel_t fg, pixel_t bg);
void fb_draw_string_scaled(int x, int y, const char *s,
                           pixel_t fg, pixel_t bg, int scale);

/* Debug console overlay */
void con_init(void);
void con_putc(char c);
void con_puts(const char *s);
void con_printf(const char *fmt, ...);
void con_clear(void);
void con_set_colours(pixel_t fg, pixel_t bg);

#endif /* FRAMEBUFFER_H */
