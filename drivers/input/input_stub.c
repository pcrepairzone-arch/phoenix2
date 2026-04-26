/*
 * input_stub.c — Input event layer for Phoenix RISC OS
 *
 * Provides keyboard_event() / mouse_event() for the USB HID driver to post
 * events into, and keyboard_poll() / mouse_poll() for the WIMP / apps to
 * consume them.  Both use simple lock-free single-consumer circular queues
 * (safe because we run single-core, no concurrent writers from IRQ context
 * while xHCI polling is done from the main loop).
 *
 * Mouse absolute position is tracked here; dx/dy from HID reports are
 * accumulated and clamped to the screen bounds set via mouse_set_bounds().
 */

#include "kernel.h"
#include "keyboard.h"
#include "mouse.h"

/* ── Keyboard event queue ─────────────────────────────────────────────────── */

#define KBD_QUEUE_SIZE  16   /* must be power-of-two */

static keyboard_event_t kbd_queue[KBD_QUEUE_SIZE];
static volatile int     kbd_head = 0;  /* next slot to write into */
static volatile int     kbd_tail = 0;  /* next slot to read from  */

int keyboard_init(void)
{
    kbd_head = kbd_tail = 0;
    debug_print("INPUT: Keyboard event queue ready (%d slots)\n", KBD_QUEUE_SIZE);
    return 0;
}

/* Called by USB HID driver to post a new key event */
void keyboard_event(const keyboard_event_t *ev)
{
    int next = (kbd_head + 1) & (KBD_QUEUE_SIZE - 1);
    if (next == kbd_tail) {
        /* Queue full — drop oldest event */
        kbd_tail = (kbd_tail + 1) & (KBD_QUEUE_SIZE - 1);
    }
    kbd_queue[kbd_head] = *ev;
    kbd_head = next;
}

/* Called by WIMP/apps to consume one event.  Returns 0 if queue empty. */
int keyboard_poll(keyboard_event_t *ev)
{
    if (kbd_tail == kbd_head)
        return 0;  /* empty */
    *ev = kbd_queue[kbd_tail];
    kbd_tail = (kbd_tail + 1) & (KBD_QUEUE_SIZE - 1);
    return 1;
}

/* ── Mouse event queue + absolute position ───────────────────────────────── */

#define MOUSE_QUEUE_SIZE  16

static mouse_event_t mouse_queue[MOUSE_QUEUE_SIZE];
static volatile int  mouse_head = 0;
static volatile int  mouse_tail = 0;

/* Absolute pointer position, clamped to [0, bounds] */
static int16_t mouse_x = 640;
static int16_t mouse_y = 360;
static int16_t mouse_max_x = 1919;
static int16_t mouse_max_y = 1079;

int mouse_init(void)
{
    mouse_head = mouse_tail = 0;
    mouse_x = 640;
    mouse_y = 360;
    debug_print("INPUT: Mouse event queue ready (%d slots), pos=(%d,%d)\n",
                MOUSE_QUEUE_SIZE, mouse_x, mouse_y);
    return 0;
}

void mouse_set_bounds(int16_t width, int16_t height)
{
    mouse_max_x = (int16_t)(width  > 0 ? width  - 1 : 1919);
    mouse_max_y = (int16_t)(height > 0 ? height - 1 : 1079);
    debug_print("INPUT: Mouse bounds %dx%d\n", (int)mouse_max_x+1, (int)mouse_max_y+1);
}

/* Called by USB HID driver to post a mouse movement/click event */
void mouse_event(const mouse_event_t *ev)
{
    /* Accumulate deltas into absolute position */
    int nx = (int)mouse_x + ev->dx;
    int ny = (int)mouse_y + ev->dy;
    if (nx < 0) nx = 0;
    if (ny < 0) ny = 0;
    if (nx > mouse_max_x) nx = mouse_max_x;
    if (ny > mouse_max_y) ny = mouse_max_y;
    mouse_x = (int16_t)nx;
    mouse_y = (int16_t)ny;

    int next = (mouse_head + 1) & (MOUSE_QUEUE_SIZE - 1);
    if (next == mouse_tail)
        mouse_tail = (mouse_tail + 1) & (MOUSE_QUEUE_SIZE - 1);  /* drop oldest */

    mouse_queue[mouse_head]         = *ev;
    mouse_queue[mouse_head].x       = mouse_x;
    mouse_queue[mouse_head].y       = mouse_y;
    mouse_head = next;
}

/* Called by WIMP/apps to consume one event.  Returns 0 if queue empty. */
int mouse_poll(mouse_event_t *ev)
{
    if (mouse_tail == mouse_head)
        return 0;
    *ev = mouse_queue[mouse_tail];
    mouse_tail = (mouse_tail + 1) & (MOUSE_QUEUE_SIZE - 1);
    return 1;
}

/* ── Absolute pointer position query (boot179) ───────────────────────────── */

void mouse_get_pos(int16_t *x, int16_t *y)
{
    if (x) *x = mouse_x;
    if (y) *y = mouse_y;
}

/* ── Helper queries (used by WIMP layer directly) ─────────────────────────── */

int is_f3_pressed(keyboard_event_t *ev)  { return ev->key_code == KEY_F3;  }
int is_f12_pressed(keyboard_event_t *ev) { return ev->key_code == KEY_F12; }
int is_select_pressed(mouse_event_t *ev) { return (ev->buttons & BUTTON_SELECT) != 0; }
int is_menu_pressed(mouse_event_t *ev)   { return (ev->buttons & BUTTON_MENU)   != 0; }
int is_adjust_pressed(mouse_event_t *ev) { return (ev->buttons & BUTTON_ADJUST) != 0; }
