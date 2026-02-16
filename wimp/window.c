/*
 * window.c – Window management for RISC OS Phoenix Wimp
 * Handles window creation, state, finding windows/icons at coordinates
 * Integrates with GPU redraw and context menus
 * Author: R Andrews Grok 4 – 06 Feb 2026
 */

#include "kernel.h"
#include "wimp.h"
#include "gpu.h"
#include "vfs.h"
// #include <string.h> /* removed - use kernel.h */

#define MAX_WINDOWS     256

static window_t windows[MAX_WINDOWS];
static int num_windows = 0;
static window_t *focus_window = NULL;

/* Create a new window */
window_t *wimp_create_window(wimp_window_def *def)
{
    if (num_windows >= MAX_WINDOWS) {
        debug_print("Wimp: Maximum windows reached\n");
        return NULL;
    }

    window_t *win = &windows[num_windows++];
    memset(win, 0, sizeof(window_t));

    memcpy(&win->def, def, sizeof(wimp_window_def));

    // Allocate GPU texture for window backing store
    win->texture = gpu_create_texture(def->width, def->height);
    if (!win->texture) {
        debug_print("Wimp: Failed to create GPU texture for window\n");
        return NULL;
    }

    win->visible = 1;
    win->context_menu = NULL;  // Default no context menu

    debug_print("Wimp: Window created – handle %p, size %dx%d\n", 
                win, def->width, def->height);

    return win;
}

/* Find window at screen coordinates */
window_t *wimp_find_window_at(int x, int y)
{
    for (int i = num_windows - 1; i >= 0; i--) {  // Topmost first
        window_t *win = &windows[i];
        if (!win->visible) continue;

        if (x >= win->def.x0 && x <= win->def.x1 &&
            y >= win->def.y0 && y <= win->def.y1) {
            return win;
        }
    }
    return NULL;
}

/* Find icon at window-relative coordinates */
int wimp_find_icon_at(window_t *win, int x, int y)
{
    if (!win) return -1;

    for (int i = 0; i < win->def.icon_count; i++) {
        wimp_icon *icon = &win->def.icons[i];
        if (x >= icon->x0 && x <= icon->x1 &&
            y >= icon->y0 && y <= icon->y1) {
            return i;
        }
    }
    return -1;
}

/* Get currently focused window */
window_t *wimp_get_focus_window(void)
{
    return focus_window;
}

/* Set window focus */
void wimp_set_focus_window(window_t *win)
{
    focus_window = win;
    debug_print("Wimp: Focus set to window %p\n", win);
}

/* Redraw window request – enqueue event */
void wimp_redraw_request(window_t *win, bbox_t *clip)
{
    wimp_event_t event;
    event.type = wimp_REDRAW_WINDOW_REQUEST;
    event.redraw.window = win;
    if (clip) {
        event.redraw.clip = *clip;
    } else {
        event.redraw.clip.x0 = win->def.x0;
        event.redraw.clip.y0 = win->def.y0;
        event.redraw.clip.x1 = win->def.x1;
        event.redraw.clip.y1 = win->def.y1;
    }

    wimp_enqueue_event(&event);
}

/* Open window request */
void wimp_open_window_request(window_t *win)
{
    wimp_event_t event;
    event.type = wimp_OPEN_WINDOW_REQUEST;
    event.open.window = win;
    wimp_enqueue_event(&event);
}

/* Close window request */
void wimp_close_window_request(window_t *win)
{
    wimp_event_t event;
    event.type = wimp_CLOSE_WINDOW_REQUEST;
    event.close.window = win;
    wimp_enqueue_event(&event);
}

/* Module init stub (called from wimp.c) */
void window_init(void)
{
    num_windows = 0;
    focus_window = NULL;
    debug_print("Window subsystem initialized\n");
}