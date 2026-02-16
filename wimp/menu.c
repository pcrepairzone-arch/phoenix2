/*
 * menu.c – Pull-down menu system for RISC OS Phoenix Wimp
 * Supports hierarchical menus, selection, submenus
 * Context-sensitive menus triggered by middle mouse button
 * Integrates with GPU for accelerated rendering
 * Author: R Andrews Grok 4 – 06 Feb 2026
 */

#include "kernel.h"
#include "wimp.h"
#include "gpu.h"
// #include <string.h> /* removed - use kernel.h */

#define MAX_MENUS       64
#define MAX_MENU_ITEMS  256

typedef struct menu_item {
    char text[32];
    int flags;
    void (*callback)(int item_id);
    menu_t *submenu;
} menu_item_t;

typedef struct menu {
    menu_item_t items[MAX_MENU_ITEMS];
    int num_items;
    int width, height;
    int x, y;
    window_t *window;   // Parent window
    menu_t *parent;     // For submenus
} menu_t;

static menu_t menus[MAX_MENUS];
static int num_menus = 0;

static menu_t *active_menu = NULL;

/* Create new menu */
menu_t *menu_create(int num_items) {
    if (num_menus >= MAX_MENUS) return NULL;

    menu_t *menu = &menus[num_menus++];
    memset(menu, 0, sizeof(menu_t));
    menu->num_items = num_items;
    menu->width = 200;
    menu->height = num_items * 20;

    return menu;
}

/* Add item to menu */
void menu_add_item(menu_t *menu, int index, const char *text, int flags, 
                   void (*callback)(int), menu_t *submenu) {
    if (index >= menu->num_items) return;

    menu_item_t *item = &menu->items[index];
    strncpy(item->text, text, 31);
    item->flags = flags;
    item->callback = callback;
    item->submenu = submenu;
}

/* Show menu at position */
void menu_show(menu_t *menu, int x, int y, window_t *parent) {
    menu->x = x;
    menu->y = y;
    menu->window = parent;
    active_menu = menu;

    wimp_event_t event;
    event.type = wimp_MENU_OPEN;
    event.menu.menu = menu;
    event.menu.x = x;
    event.menu.y = y;
    event.menu.window = parent;
    wimp_enqueue_event(&event);

    wimp_redraw_request(parent, NULL);
}

/* Hide active menu */
void menu_hide(void) {
    if (!active_menu) return;

    wimp_event_t event;
    event.type = wimp_MENU_CLOSE;
    event.menu.menu = active_menu;
    wimp_enqueue_event(&event);

    active_menu = NULL;
    wimp_redraw_request(active_menu->window, NULL);
}

/* Handle mouse click in menu */
static void menu_handle_click(int x, int y) {
    if (!active_menu) return;

    if (x < active_menu->x || x > active_menu->x + active_menu->width ||
        y < active_menu->y || y > active_menu->y + active_menu->height) {
        menu_hide();
        return;
    }

    int item_id = (y - active_menu->y) / 20;
    if (item_id >= active_menu->num_items) return;

    menu_item_t *item = &active_menu->items[item_id];

    if (item->submenu) {
        menu_show(item->submenu, active_menu->x + active_menu->width, 
                  active_menu->y + item_id * 20, active_menu->window);
    } else if (item->callback) {
        item->callback(item_id);
        menu_hide();
    }
}

/* Render menu on GPU */
static void menu_render(menu_t *menu) {
    gpu_draw_rect(menu->x, menu->y, menu->width, menu->height, 0xFFFFFFFF);

    for (int i = 0; i < menu->num_items; i++) {
        menu_item_t *item = &menu->items[i];
        gpu_draw_text(menu->x + 10, menu->y + i * 20 + 5, item->text, 0x00000000);
        if (item->submenu) gpu_draw_arrow(menu->x + menu->width - 10, menu->y + i * 20 + 5);
    }
}

/* Handle menu selection event */
void menu_handle_selection(wimp_menu_event_t *menu_event) {
    if (!active_menu) return;
    int item_id = menu_event->item;
    if (item_id < 0 || item_id >= active_menu->num_items) return;

    menu_item_t *item = &active_menu->items[item_id];
    if (item->callback) item->callback(item_id);
    menu_hide();
}

/* Get context menu for window/icon (stub) */
menu_t *get_context_menu(window_t *win, int icon) {
    // Per-window or per-icon menu (stub)
    return NULL;
}

/* Get Filer context menu */
menu_t *get_filer_menu(icon_t *icon) {
    menu_t *menu = menu_create(5);
    menu_add_item(menu, 0, "Open", 0, filer_open_item, NULL);
    menu_add_item(menu, 1, "Copy", 0, filer_copy_item, NULL);
    menu_add_item(menu, 2, "Rename", 0, filer_rename_item, NULL);
    menu_add_item(menu, 3, "Delete", 0, filer_delete_item, NULL);
    menu_add_item(menu, 4, "Info", 0, filer_info_item, NULL);
    return menu;
}

/* Get default desktop menu */
menu_t *get_default_menu(window_t *win) {
    menu_t *menu = menu_create(3);
    menu_add_item(menu, 0, "New Directory", 0, NULL, NULL);
    menu_add_item(menu, 1, "Options", 0, NULL, NULL);
    menu_add_item(menu, 2, "Shutdown", 0, NULL, NULL);
    return menu;
}