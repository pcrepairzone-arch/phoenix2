/*
 * cursor_module_impl.c — CursorModule implementation (kernel-linked)
 *
 * This module is built into the Phoenix kernel image and linked against
 * kernel symbols directly (no SWI path needed yet).  It:
 *
 *   1. Initialises the keyboard and mouse event queues.
 *   2. Sets mouse bounds from the framebuffer dimensions.
 *   3. Calls cursor_init() + cursor_show() to start the pointer sprite.
 *   4. Reports how many USB HID mice and keyboards were found.
 *
 * RISC OS button conventions (per drivers/input/mouse.h):
 *   SELECT  = left  button  (BUTTON_SELECT 0x04, USB bit 0)
 *   MENU    = middle button  (BUTTON_MENU   0x02, USB bit 2)
 *   ADJUST  = right  button  (BUTTON_ADJUST 0x01, USB bit 1)
 * These mappings are applied in usb_hid.c hid_process_mouse() and are
 * consumed here via mouse_poll() / mouse_event_t.buttons.
 *
 * Ownership: CursorModule owns cursor initialisation.  lib.c wimp_task()
 * no longer calls cursor_init() directly — it is called once here at
 * module init time (before wimp_task() starts).
 *
 * boot394: first kernel-linked AArch64 module with real kernel API calls.
 */

#include "kernel.h"
#include "../drivers/gpu/framebuffer.h"
#include "../drivers/input/mouse.h"
#include "../drivers/input/keyboard.h"

/* RISC OS error block — standard ABI return type for module entry points.
 * A NULL pointer means no error; non-NULL points to errnum + errmess.    */
typedef struct {
    unsigned int errnum;
    char         errmess[252];
} _kernel_oserror;

/* ── C-library hooks (stubs) ──────────────────────────────────────────────── */

/* _clib_initialisemodule / _clib_finalisemodule are called by the CMunge
 * -64bit veneer around our init/final entry points.  For an embedded
 * kernel module there is no RISC OS C run-time library to initialise;
 * return NULL (no error).  Declared weak so a future full riscos64-clib
 * can override them.                                                       */

__attribute__((weak))
_kernel_oserror *_clib_initialisemodule(void *pw)
{
    (void)pw;
    return 0;
}

__attribute__((weak))
_kernel_oserror *_clib_finalisemodule(void *pw)
{
    (void)pw;
    return 0;
}

/* ── Zero-initialised workspace size ──────────────────────────────────────── */

/* CMunge places __ZISize in the module flags block.  For a kernel-linked
 * module we manage our own storage; the ZI area size is 0.                */
int __ZISize = 0;

/* ── USB HID device count (weak — present once USB HID has probed) ───────── */

extern int usb_hid_mouse_count(void)    __attribute__((weak));
extern int usb_hid_keyboard_count(void) __attribute__((weak));

/* ── Module init ─────────────────────────────────────────────────────────── */

/*
 * cursor_module_impl_init — called by the CMunge -64bit veneer at module
 * initialisation.
 *
 * Arguments follow the RISC OS module init convention:
 *   tail        — pointer to command tail (0 at boot)
 *   podule_base — podule base (0 for non-podule modules)
 *   pw          — private word / workspace pointer
 *
 * We ignore all three: we're setting up kernel-global input state, not
 * per-instance state.
 */
_kernel_oserror *cursor_module_impl_init(const char *tail,
                                         int podule_base,
                                         void *pw)
{
    (void)tail;
    (void)podule_base;
    (void)pw;

    /* Initialise event queues */
    keyboard_init();
    mouse_init();

    /* Set mouse bounds from framebuffer — clamp to 1920×1080 if FB not ready */
    if (fb.valid) {
        mouse_set_bounds((int16_t)fb.width, (int16_t)fb.height);
    } else {
        mouse_set_bounds(1920, 1080);
    }

    /* Start the cursor sprite at the default position (640, 360) */
    cursor_init();
    cursor_show();

    /* Report connected HID devices */
    int mice = usb_hid_mouse_count    ? usb_hid_mouse_count()    : 0;
    int kbds = usb_hid_keyboard_count ? usb_hid_keyboard_count() : 0;

    debug_print("[CursorMod] init ok — mice=%d kbds=%d bounds=%ux%u\n",
                mice, kbds,
                (unsigned)(fb.valid ? fb.width  : 1920u),
                (unsigned)(fb.valid ? fb.height : 1080u));

    debug_print("[CursorMod] SELECT=left MENU=middle ADJUST=right\n");

    return 0;  /* NULL = no error */
}

/* ── Module final ────────────────────────────────────────────────────────── */

_kernel_oserror *cursor_module_impl_final(int fatal,
                                          int podule_base,
                                          void *pw)
{
    (void)fatal;
    (void)podule_base;
    (void)pw;

    cursor_hide();
    debug_print("[CursorMod] finalised\n");
    return 0;
}
