/*
 * event.c – Event handling and dispatching for RISC OS Phoenix Wimp
 * Routes mouse, keyboard, menu, and redraw events to applications
 * Author: R Andrews Grok 4 – 06 Feb 2026
 */

#include "kernel.h"
#include "wimp.h"
#include "menu.h"
#include "filecore.h"

/* Registered event handlers (per event type) */
#define MAX_EVENT_TYPES  32
static void (*event_handlers[MAX_EVENT_TYPES])(wimp_event_t *event) = {0};

/* Register a handler for a specific event type */
void wimp_register_event_handler(int event_type, void (*handler)(wimp_event_t *event))
{
    if (event_type < 0 || event_type >= MAX_EVENT_TYPES) return;
    event_handlers[event_type] = handler;
}

/* Dispatch event to registered handler or default app handler */
void wimp_dispatch_event(wimp_event_t *event)
{
    if (event->type < MAX_EVENT_TYPES && event_handlers[event->type]) {
        event_handlers[event->type](event);
        return;
    }

    /* Default dispatching based on event type */
    switch (event->type) {
        case wimp_MOUSE_CLICK:
            app_handle_mouse(&event->mouse);
            break;

        case wimp_KEY_PRESSED:
            app_handle_key(&event->key);
            break;

        case wimp_REDRAW_WINDOW_REQUEST:
            gpu_redraw_window(event->redraw.window);
            break;

        case wimp_MENU_SELECTION:
            menu_handle_selection(&event->menu);
            break;

        case wimp_MENU_OPEN:
            menu_show(event->menu.menu, event->menu.x, event->menu.y, event->menu.window);
            break;

        case wimp_MENU_CLOSE:
            menu_hide();
            break;

        case wimp_OPEN_WINDOW_REQUEST:
            // Stub: update window position/size
            break;

        case wimp_CLOSE_WINDOW_REQUEST:
            // Stub: close window
            break;

        default:
            debug_print("Wimp: Unhandled event type %d\n", event->type);
            break;
    }
}

/* Application-level mouse handler stub */
void app_handle_mouse(wimp_mouse_t *mouse)
{
    // Apps override this or use Wimp_Poll
    debug_print("Mouse click: button=%d at (%d,%d)\n", 
                mouse->button, mouse->x, mouse->y);
}

/* Application-level key handler stub */
void app_handle_key(wimp_key_t *key)
{
    debug_print("Key pressed: code=%d modifiers=0x%x\n", 
                key->code, key->modifiers);
}

/* Module init (called from wimp.c) */
void event_init(void)
{
    memset(event_handlers, 0, sizeof(event_handlers));
    debug_print("Event dispatcher initialized\n");
}