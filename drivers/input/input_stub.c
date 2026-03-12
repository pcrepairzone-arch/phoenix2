/*
 * input_stub.c - Input driver integration for Phoenix RISC OS
 * Wires keyboard_poll/mouse_poll through to the USB HID driver.
 */

#include "kernel.h"
#include "keyboard.h"
#include "mouse.h"

static mouse_event_t mouse_state = {0};

/* Declared in usb_hid.c */
extern int hid_poll_all(void);

int keyboard_init(void)
{
    debug_print("INPUT: Keyboard init (USB HID)\n");
    return 0;
}

int keyboard_poll(keyboard_event_t *event)
{
    /*
     * HID key events are processed inside hid_process_keyboard() which
     * calls con_putc() directly for immediate echo to the framebuffer.
     * keyboard_poll() is the WIMP-facing API — call hid_poll_all() to
     * service the USB stack and return -1 (no discrete event queued yet;
     * a proper event queue can be wired in Phase 2 WIMP integration).
     */
    (void)event;
    hid_poll_all();
    return -1;
}

int mouse_init(void)
{
    debug_print("INPUT: Mouse init (USB HID)\n");
    mouse_state.x = 640;
    mouse_state.y = 360;
    return 0;
}

int mouse_poll(mouse_event_t *event)
{
    /*
     * Mouse events are processed inside hid_process_mouse().
     * hid_poll_all() will be called by keyboard_poll(); avoid double-poll
     * by returning cached state here.
     */
    if (event)
        *event = mouse_state;
    return -1;
}

void mouse_set_bounds(int16_t width, int16_t height)
{
    debug_print("INPUT: Mouse bounds %dx%d\n", width, height);
}

int is_f3_pressed(keyboard_event_t *event)
{
    return (event->key_code == KEY_F3);
}

int is_f12_pressed(keyboard_event_t *event)
{
    return (event->key_code == KEY_F12);
}

int is_select_pressed(mouse_event_t *event)
{
    return (event->buttons & BUTTON_SELECT) != 0;
}

int is_menu_pressed(mouse_event_t *event)
{
    return (event->buttons & BUTTON_MENU) != 0;
}

int is_adjust_pressed(mouse_event_t *event)
{
    return (event->buttons & BUTTON_ADJUST) != 0;
}
