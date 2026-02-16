/*
 * wimp.c – 64-bit Window Manager (Wimp) for RISC OS Phoenix
 * Full desktop with windows, menus, icons, drag-select
 * Supports context-sensitive menus (middle mouse button)
 * Left double-click (Select) opens files/apps based on file type
 * GPU-accelerated redraw
 * Author:R Andrews Grok 4 – 06 Feb 2026
 */

#include "kernel.h"
#include "wimp.h"
#include "gpu.h"
#include "vfs.h"
#include "filecore.h"
// #include <string.h> /* removed - use kernel.h */

#define MAX_WINDOWS     256
#define MAX_EVENTS      1024

/* Mouse button constants (standard RISC OS) */
#define MOUSE_SELECT    1   // Left button
#define MOUSE_MENU      2   // Middle button → context menu
#define MOUSE_ADJUST    4   // Right button

typedef struct wimp_event_queue {
    wimp_event_t events[MAX_EVENTS];
    int head, tail;
    spinlock_t lock;
} wimp_event_queue_t;

static wimp_event_queue_t event_queue;

static window_t windows[MAX_WINDOWS];
static int num_windows = 0;

static void wimp_init(void) {
    memset(windows, 0, sizeof(windows));
    memset(&event_queue, 0, sizeof(event_queue));
    spinlock_init(&event_queue.lock);
    gpu_init();  // Initialize GPU acceleration
    debug_print("Wimp initialized – desktop ready\n");
}

/* Poll for events – cooperative at app level */
int Wimp_Poll(int mask, wimp_event_t *event) {
    unsigned long flags;
    spin_lock_irqsave(&event_queue.lock, &flags);

    if (event_queue.head == event_queue.tail) {
        spin_unlock_irqrestore(&event_queue.lock, flags);
        yield();  // Allow kernel preemption while idle
        return wimp_NULL_REASON_CODE;
    }

    *event = event_queue.events[event_queue.tail % MAX_EVENTS];
    event_queue.tail++;

    spin_unlock_irqrestore(&event_queue.lock, flags);
    return event->type;
}

/* Internal event enqueue */
void wimp_enqueue_event(wimp_event_t *event) {
    unsigned long flags;
    spin_lock_irqsave(&event_queue.lock, &flags);

    if ((event_queue.head - event_queue.tail) >= MAX_EVENTS) {
        debug_print("Wimp: Event queue overflow\n");
        spin_unlock_irqrestore(&event_queue.lock, flags);
        return;
    }

    event_queue.events[event_queue.head % MAX_EVENTS] = *event;
    event_queue.head++;

    spin_unlock_irqrestore(&event_queue.lock, flags);
    task_wakeup(wimp_task);  // Wake Wimp task if blocked
}

/* Create window */
window_t *wimp_create_window(wimp_window_def *def) {
    if (num_windows >= MAX_WINDOWS) return NULL;

    window_t *win = &windows[num_windows++];
    memcpy(&win->def, def, sizeof(wimp_window_def));

    // Allocate GPU texture for window backing store
    win->texture = gpu_create_texture(def->width, def->height);

    debug_print("Wimp: Window created – handle %p\n", win);
    return win;
}

/* Redraw window request – enqueue event */
void wimp_redraw_request(window_t *win, bbox_t *clip) {
    wimp_event_t event;
    event.type = wimp_REDRAW_WINDOW_REQUEST;
    event.redraw.window = win;
    if (clip) event.redraw.clip = *clip;

    wimp_enqueue_event(&event);
}

/* Mouse click handler – from input driver */
void input_mouse_click(int button, int x, int y) {
    wimp_event_t event;
    event.type = wimp_MOUSE_CLICK;
    event.mouse.button = button;
    event.mouse.x = x;
    event.mouse.y = y;

    // Find window and icon under mouse
    window_t *win = wimp_find_window_at(x, y);
    if (win) {
        event.mouse.window = win;
        event.mouse.icon = wimp_find_icon_at(win, x - win->def.x0, y - win->def.y0);
    }

    /* Context-sensitive menu on middle button (MENU) */
    if (button & MOUSE_MENU) {
        if (win) {
            menu_t *context_menu = get_context_menu(win, event.mouse.icon);
            if (context_menu) {
                menu_show(context_menu, x, y, win);
            } else if (win == filer_window) {
                menu_t *filer_menu = get_filer_menu(event.mouse.icon);
                menu_show(filer_menu, x, y, win);
            } else {
                menu_t *default_menu = get_default_menu(win);
                menu_show(default_menu, x, y, win);
            }
        }
        wimp_enqueue_event(&event);
        return;
    }

    /* Select (left button) double-click → open by file type */
    static int last_button = 0;
    static uint64_t last_time = 0;
    uint64_t now = get_time_ms();

    if ((button & MOUSE_SELECT) && last_button == MOUSE_SELECT && (now - last_time) < 300) {
        if (win == filer_window && event.mouse.icon) {
            inode_t *inode = get_icon_inode(event.mouse.icon);
            if (inode) {
                if (inode->i_mode & S_IFDIR) {
                    filer_open_directory(inode);
                } else {
                    char *app = get_app_for_file_type(inode->file_type);
                    if (app) {
                        execve(app, (char*[]){app, inode->path, NULL}, environ);
                    } else {
                        // Fallback to default editor
                        execve("/Apps/!Edit", (char*[]){"!Edit", inode->path, NULL}, environ);
                    }
                }
            }
        }
    }

    last_button = button;
    last_time = now;

    wimp_enqueue_event(&event);
}

/* Key press handler */
void input_key_press(int key, int modifiers) {
    wimp_event_t event;
    event.type = wimp_KEY_PRESSED;
    event.key.code = key;
    event.key.modifiers = modifiers;

    window_t *focus = wimp_get_focus_window();
    if (focus) event.key.window = focus;

    wimp_enqueue_event(&event);
}

/* Main Wimp task – runs as dedicated high-priority task */
void wimp_task(void) {
    wimp_init();

    while (1) {
        wimp_event_t event;
        int code = Wimp_Poll(0, &event);

        switch (code) {
            case wimp_REDRAW_WINDOW_REQUEST:
                gpu_redraw_window(event.redraw.window);  // GPU accelerated redraw
                break;

            case wimp_MOUSE_CLICK:
                app_handle_mouse(&event.mouse);
                break;

            case wimp_KEY_PRESSED:
                app_handle_key(&event.key);
                break;

            case wimp_MENU_SELECTION:
                menu_handle_selection(&event.menu);
                break;

            // ... other events (OPEN_WINDOW_REQUEST, CLOSE_WINDOW_REQUEST, etc.)
        }
    }
}

/* Module init – start Wimp task on core 0 for compatibility */
_kernel_oserror *module_init(const char *arg, int podule)
{
    task_create("wimp", wimp_task, 0, (1ULL << 0));  // Pin to core 0
    debug_print("Wimp module loaded – desktop active\n");
    return NULL;
}