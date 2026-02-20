/*
 * input_stub.c - Input driver stubs for Phoenix RISC OS
 * These are stubs until USB HID is implemented
 * Will be replaced with real USB keyboard/mouse drivers
 */

#include "kernel.h"
#include "keyboard.h"
#include "mouse.h"

static mouse_event_t mouse_state = {0};

int keyboard_init(void)
{
    debug_print("INPUT: Keyboard stub initialized\n");
    debug_print("INPUT: Waiting for USB HID implementation\n");
    return 0;
}

int keyboard_poll(keyboard_event_t *event)
{
    /* Stub - returns no events */
    return -1;
}

int mouse_init(void)
{
    debug_print("INPUT: Mouse stub initialized\n");
    debug_print("INPUT: Three-button support: SELECT/MENU/ADJUST\n");
    
    mouse_state.x = 640;
    mouse_state.y = 360;
    return 0;
}

int mouse_poll(mouse_event_t *event)
{
    /* Stub - returns no events */
    return -1;
}

void mouse_set_bounds(int16_t width, int16_t height)
{
    debug_print("INPUT: Mouse bounds set to %dx%d\n", width, height);
}

/* Helper: Check if F3 (Save) is pressed - for future use */
int is_f3_pressed(keyboard_event_t *event)
{
    return (event->key_code == KEY_F3);
}

/* Helper: Check if F12 (CLI) is pressed */
int is_f12_pressed(keyboard_event_t *event)
{
    return (event->key_code == KEY_F12);
}

/* Helper: Check SELECT button */
int is_select_pressed(mouse_event_t *event)
{
    return (event->buttons & BUTTON_SELECT) != 0;
}

/* Helper: Check MENU button */
int is_menu_pressed(mouse_event_t *event)
{
    return (event->buttons & BUTTON_MENU) != 0;
}

/* Helper: Check ADJUST button */
int is_adjust_pressed(mouse_event_t *event)
{
    return (event->buttons & BUTTON_ADJUST) != 0;
}
