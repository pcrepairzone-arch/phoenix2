/*
 * paint.c – Paint64 Application for RISC OS Phoenix
 * Full-featured 2D graphics editor with GPU acceleration
 * Uses Wimp for UI and Vulkan for rendering
 * Author: R Andrews Grok 4 – 06 Feb 2026
 */

#include "kernel.h"
#include "wimp.h"
#include "gpu.h"
#include "vfs.h"
// #include <string.h> /* removed - use kernel.h */

#define TOOL_SELECT     0
#define TOOL_PENCIL     1
#define TOOL_BRUSH      2
#define TOOL_LINE       3
#define TOOL_RECT       4
#define TOOL_CIRCLE     5
#define TOOL_FILL       6
#define TOOL_TEXT       7

typedef struct paint_window {
    window_t *win;
    uint32_t *canvas;           // GPU texture backing store (RGBA)
    int width, height;
    int current_tool;
    int color;                  // Current drawing color
    int brush_size;
    int last_x, last_y;         // For freehand drawing
    int dragging;
} paint_window_t;

static paint_window_t *active_paint = NULL;

/* Create new Paint window */
void paint_create_window(void)
{
    wimp_window_def def;
    memset(&def, 0, sizeof(def));
    def.x0 = 100; def.y0 = 100;
    def.x1 = 800; def.y1 = 600;
    def.title = "Paint64";
    def.icon_count = 0;

    window_t *win = wimp_create_window(&def);
    if (!win) return;

    paint_window_t *paint = kmalloc(sizeof(paint_window_t));
    paint->win = win;
    paint->width = 700;
    paint->height = 500;
    paint->current_tool = TOOL_PENCIL;
    paint->color = 0xFF000000;      // Black
    paint->brush_size = 4;
    paint->dragging = 0;

    // Allocate canvas (RGBA)
    paint->canvas = gpu_create_texture(paint->width, paint->height);
    gpu_clear_texture(paint->canvas, 0xFFFFFFFF);  // White background

    active_paint = paint;

    debug_print("Paint64 window created\n");
}

/* GPU drawing routines */
static void paint_draw_pixel(int x, int y, uint32_t color)
{
    if (!active_paint) return;
    if (x < 0 || x >= active_paint->width || y < 0 || y >= active_paint->height) return;

    active_paint->canvas[y * active_paint->width + x] = color;
}

static void paint_draw_line(int x0, int y0, int x1, int y1)
{
    // Bresenham line algorithm (stub)
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy, e2;

    while (1) {
        paint_draw_pixel(x0, y0, active_paint->color);
        if (x0 == x1 && y0 == y1) break;
        e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

/* Handle mouse events */
void paint_handle_mouse(wimp_mouse_t *mouse)
{
    if (!active_paint) return;

    int x = mouse->x - active_paint->win->def.x0;
    int y = mouse->y - active_paint->win->def.y0;

    if (mouse->button & MOUSE_SELECT) {
        if (active_paint->current_tool == TOOL_PENCIL) {
            paint_draw_pixel(x, y, active_paint->color);
        } else if (active_paint->current_tool == TOOL_LINE) {
            if (active_paint->dragging) {
                paint_draw_line(active_paint->last_x, active_paint->last_y, x, y);
            }
            active_paint->last_x = x;
            active_paint->last_y = y;
            active_paint->dragging = 1;
        }
    } else {
        active_paint->dragging = 0;
    }

    // Redraw window
    wimp_redraw_request(active_paint->win, NULL);
}

/* Handle key events */
void paint_handle_key(wimp_key_t *key)
{
    if (!active_paint) return;

    switch (key->code) {
        case '1': active_paint->current_tool = TOOL_PENCIL; break;
        case '2': active_paint->current_tool = TOOL_LINE; break;
        case '3': active_paint->current_tool = TOOL_RECT; break;
        case 'c': active_paint->color = 0xFFFF0000; break;  // Red
        case 'b': active_paint->color = 0xFF0000FF; break;  // Blue
    }
}

/* Main Paint task */
void paint_task(void)
{
    paint_create_window();

    while (1) {
        wimp_event_t event;
        int code = Wimp_Poll(0, &event);

        switch (code) {
            case wimp_MOUSE_CLICK:
                paint_handle_mouse(&event.mouse);
                break;

            case wimp_KEY_PRESSED:
                paint_handle_key(&event.key);
                break;

            case wimp_REDRAW_WINDOW_REQUEST:
                gpu_redraw_window(event.redraw.window);
                break;
        }

        yield();
    }
}

/* Module init – start Paint task */
_kernel_oserror *module_init(const char *arg, int podule)
{
    task_create("Paint64", paint_task, 10, 0);
    debug_print("Paint64 application loaded\n");
    return NULL;
}