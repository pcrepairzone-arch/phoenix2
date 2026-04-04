/*
 * usb_hid.c — USB HID (Keyboard + Mouse) driver for Phoenix RISC OS
 *
 * Boot-protocol mode, polled via interrupt endpoint.
 * Reports are decoded into RISC OS keyboard_event_t / mouse_event_t and
 * posted to the input layer via keyboard_event() / mouse_event().
 *
 * Architecture: polled.  hid_poll_all() must be called from the main
 * kernel loop (or a periodic timer task).  When xHCI interrupt delivery
 * is fully working the poll cadence can be replaced with a callback, but
 * the report-decoding and event-posting logic here does not change.
 *
 * R Andrews / prototype merged boot 70 — March 2026
 */

#include "kernel.h"
#include "usb.h"
#include "input/keyboard.h"
#include "input/mouse.h"
#include <string.h>

/* ── HID Boot Protocol report structures ─────────────────────────────────── */

typedef struct {
    uint8_t modifiers;    /* Bit mask: LCtrl LShift LAlt LGUI RCtrl RShift RAlt RGUI */
    uint8_t reserved;
    uint8_t keycodes[6];  /* Up to 6 simultaneous keys (HID page 0x07 scan codes) */
} hid_keyboard_report_t;

typedef struct {
    uint8_t buttons;  /* Bit 0=left Bit 1=right Bit 2=middle */
    int8_t  x;        /* Signed delta X */
    int8_t  y;        /* Signed delta Y */
    int8_t  wheel;    /* Signed wheel delta (optional, 4th byte) */
} hid_mouse_report_t;

/* ── HID device descriptor ────────────────────────────────────────────────── */

typedef struct {
    usb_device_t    *dev;
    usb_interface_t *intf;
    usb_endpoint_t  *int_in;   /* Interrupt IN endpoint */
    uint8_t          protocol; /* USB_PROTOCOL_KEYBOARD=1, USB_PROTOCOL_MOUSE=2 */
    uint8_t          last_report[8]; /* Debounce: compare against previous report */
} hid_device_t;

#define HID_MAX_DEVICES 4
static hid_device_t *hid_devices[HID_MAX_DEVICES];
static int           hid_device_count = 0;

/* ── Scancode tables (US layout, HID page 0x07) ─────────────────────────── */

/*
 * HID scan code → ASCII (lower-case / unshifted).
 * Index = HID scan code 0x00..0x65 (101 entries).
 */
static const char scancode_to_ascii[] = {
    0,   0,   0,   0,   'a', 'b', 'c', 'd', /* 0x00-0x07 */
    'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l', /* 0x08-0x0F */
    'm', 'n', 'o', 'p', 'q', 'r', 's', 't', /* 0x10-0x17 */
    'u', 'v', 'w', 'x', 'y', 'z', '1', '2', /* 0x18-0x1F */
    '3', '4', '5', '6', '7', '8', '9', '0', /* 0x20-0x27 */
    '\n',0x1B,'\b','\t',' ', '-', '=', '[', /* 0x28-0x2F */
    ']', '\\','#', ';', '\'','`', ',', '.', /* 0x30-0x37 */
    '/',  0,   0,   0,   0,   0,   0,   0,  /* 0x38-0x3F (F1-F4 via inkey) */
    0,   0,   0,   0,   0,   0,   0,   0,   /* 0x40-0x47 (F5-F8+more) */
    0,   0,   0,   0,   0,   0,   0,   0,   /* 0x48-0x4F (F9-F12+more) */
};

/*
 * HID scan code → shifted ASCII (shift held).
 */
static const char scancode_to_ascii_shifted[] = {
    0,   0,   0,   0,   'A', 'B', 'C', 'D', /* 0x00-0x07 */
    'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', /* 0x08-0x0F */
    'M', 'N', 'O', 'P', 'Q', 'R', 'S', 'T', /* 0x10-0x17 */
    'U', 'V', 'W', 'X', 'Y', 'Z', '!', '@', /* 0x18-0x1F */
    '#', '$', '%', '^', '&', '*', '(', ')', /* 0x20-0x27 */
    '\n',0x1B,'\b','\t',' ', '_', '+', '{', /* 0x28-0x2F */
    '}', '|', '~', ':', '"', '~', '<', '>', /* 0x30-0x37 */
    '?',  0,   0,   0,   0,   0,   0,   0,  /* 0x38-0x3F */
    0,   0,   0,   0,   0,   0,   0,   0,   /* 0x40-0x47 */
    0,   0,   0,   0,   0,   0,   0,   0,   /* 0x48-0x4F */
};

/*
 * HID scan code → RISC OS INKEY number.
 * Covers F1-F12, cursor keys, and a few specials.
 * 0 = not a special key (use ASCII path instead).
 */
static const uint8_t scancode_to_inkey[256] = {
    /* 0x00-0x03 */ 0, 0, 0, 0,
    /* 0x04 'a'  */ 0x62, 0x64, 0x52, 0x32, /* a b c d */
    /* 0x08 'e'  */ 0x22, 0x44, 0x54, 0x44, /* e f g h — INKEY approximate */
    /* remainder zero — fall through to ASCII path */
    [0x3A] = KEY_F1,  [0x3B] = KEY_F2,  [0x3C] = KEY_F3,  [0x3D] = KEY_F4,
    [0x3E] = KEY_F5,  [0x3F] = KEY_F6,  [0x40] = KEY_F7,  [0x41] = KEY_F8,
    [0x42] = KEY_F9,  [0x43] = KEY_F10, [0x44] = KEY_F11, [0x45] = KEY_F12,
    /* Cursor keys */
    [0x4F] = 0x8E,  /* Right */
    [0x50] = 0x8D,  /* Left  */
    [0x51] = 0x8F,  /* Down  */
    [0x52] = 0x8C,  /* Up    */
    /* Home / End / PgUp / PgDn */
    [0x4A] = 0x1E,  /* Home  */
    [0x4D] = 0x0E,  /* End   */
    [0x4B] = 0x1F,  /* PgUp  */
    [0x4E] = 0x0F,  /* PgDn  */
    /* Delete / Insert */
    [0x4C] = 0x7F,  /* Delete */
    [0x49] = 0x1D,  /* Insert */
};

/* ── USB modifier → RISC OS modifier mapping ──────────────────────────────
 * USB byte:  bit0=LCtrl bit1=LShift bit2=LAlt bit3=LGUI
 *            bit4=RCtrl bit5=RShift bit6=RAlt bit7=RGUI
 * RISC OS:   MOD_SHIFT=0x01  MOD_CTRL=0x02  MOD_ALT=0x04
 */
static uint8_t usb_mod_to_riscos(uint8_t usb_mod) {
    uint8_t m = 0;
    if (usb_mod & 0x22) m |= MOD_SHIFT; /* LShift | RShift */
    if (usb_mod & 0x11) m |= MOD_CTRL;  /* LCtrl  | RCtrl  */
    if (usb_mod & 0x44) m |= MOD_ALT;   /* LAlt   | RAlt   */
    return m;
}

/* ── Keyboard report processor ────────────────────────────────────────────── */

static void hid_process_keyboard(hid_device_t *hid, const uint8_t *data)
{
    hid_keyboard_report_t *report = (hid_keyboard_report_t *)data;
    uint8_t ro_mod = usb_mod_to_riscos(report->modifiers);
    int shift = (report->modifiers & 0x22) != 0;

    for (int i = 0; i < 6; i++) {
        uint8_t code = report->keycodes[i];
        if (code == 0 || code > 0xE7) continue;  /* no key / OOB */

        /* Debounce: skip if this code was present in the last report */
        int seen = 0;
        for (int j = 2; j < 8; j++) {
            if (hid->last_report[j] == code) { seen = 1; break; }
        }
        if (seen) continue;

        /* Build RISC OS keyboard event */
        keyboard_event_t ev = { 0 };
        ev.modifiers = ro_mod;

        /* F-keys and cursor keys: use INKEY path */
        if (scancode_to_inkey[code]) {
            ev.key_code = scancode_to_inkey[code];
            ev.key_char = 0;
        } else {
            /* ASCII path */
            char ch = (code < sizeof(scancode_to_ascii))
                      ? (shift ? scancode_to_ascii_shifted[code]
                               : scancode_to_ascii[code])
                      : 0;
            ev.key_char = (uint8_t)ch;
            ev.key_code = (uint8_t)ch;  /* RISC OS: key_code == char for ASCII */
        }

        if (ev.key_code || ev.key_char) {
            debug_print("[KBD] code=0x%02x inkey=0x%02x char='%c' mod=0x%02x\n",
                        code, ev.key_code,
                        (ev.key_char >= 0x20 && ev.key_char < 0x7F) ? ev.key_char : '.',
                        ev.modifiers);

            /* Post to RISC OS input layer */
            keyboard_event(&ev);

            /* Also echo printable characters directly to the console framebuffer */
            if (ev.key_char >= 0x20) {
                extern void con_putc(char c);
                con_putc((char)ev.key_char);
            }
        }
    }

    /* Save report for next-iteration debounce */
    for (int i = 0; i < 8; i++)
        hid->last_report[i] = data[i];
}

/* ── Mouse report processor ───────────────────────────────────────────────── */

static void hid_process_mouse(hid_device_t *hid, const uint8_t *data)
{
    (void)hid;
    hid_mouse_report_t *report = (hid_mouse_report_t *)data;

    mouse_event_t ev = { 0 };
    ev.dx = report->x;
    ev.dy = -report->y;  /* USB Y increases downward; RISC OS Y increases upward */
    ev.wheel = report->wheel;

    /* USB button → RISC OS: bit0=left(SELECT) bit1=right(ADJUST) bit2=middle(MENU) */
    if (report->buttons & 0x01) ev.buttons |= BUTTON_SELECT;  /* left   */
    if (report->buttons & 0x04) ev.buttons |= BUTTON_MENU;    /* middle */
    if (report->buttons & 0x02) ev.buttons |= BUTTON_ADJUST;  /* right  */

    if (ev.dx || ev.dy || ev.buttons || ev.wheel) {
        debug_print("[MOUSE] dx=%d dy=%d btn=0x%02x wheel=%d\n",
                    ev.dx, ev.dy, ev.buttons, ev.wheel);
        mouse_event(&ev);
    }
}

/* ── Poll a single HID device ─────────────────────────────────────────────── */

static void hid_poll(hid_device_t *hid)
{
    uint8_t report[8] = { 0 };
    int len;

    if (hid->int_in) {
        /* Interrupt endpoint poll (boot-protocol, 8-byte report) */
        len = usb_interrupt_transfer(hid->int_in, report, 8, 10);
    } else {
        /* Fallback: HID GET_REPORT class request over EP0
         * bmRequestType=0xA1 (IN|Class|Interface), bRequest=1 (GET_REPORT)
         * wValue=0x0100 (Input report type, report ID 0)                    */
        len = usb_control_transfer(hid->dev,
                                   0xA1, 0x01, 0x0100,
                                   hid->intf->bInterfaceNumber,
                                   report, 8, 100);
    }

    if (len > 0) {
        if (hid->protocol == USB_PROTOCOL_KEYBOARD)
            hid_process_keyboard(hid, report);
        else if (hid->protocol == USB_PROTOCOL_MOUSE)
            hid_process_mouse(hid, report);
    }
}

/* ── HID class driver probe ───────────────────────────────────────────────── */

static int hid_probe(usb_device_t *dev, usb_interface_t *intf)
{
    debug_print("[HID] Probing interface %d protocol=0x%02x subclass=0x%02x\n",
                intf->bInterfaceNumber,
                intf->bInterfaceProtocol,
                intf->bInterfaceSubClass);

    /* Boot protocol only — subclass 1 required */
    if (intf->bInterfaceSubClass != USB_SUBCLASS_BOOT) {
        debug_print("[HID] Not boot-protocol subclass, skipping\n");
        return -1;
    }

    hid_device_t *hid = kmalloc(sizeof(hid_device_t));
    if (!hid) { debug_print("[HID] kmalloc failed\n"); return -1; }
    memset(hid, 0, sizeof(hid_device_t));

    hid->dev      = dev;
    hid->intf     = intf;
    hid->protocol = intf->bInterfaceProtocol;

    /* Find interrupt IN endpoint */
    for (int i = 0; i < intf->endpoint_count; i++) {
        usb_endpoint_t *ep = &intf->endpoints[i];
        if (((ep->bmAttributes & 0x03) == 0x03) && (ep->bEndpointAddress & 0x80)) {
            hid->int_in = ep;
            break;
        }
    }

    if (!hid->int_in) {
        debug_print("[HID] No interrupt IN endpoint — falling back to GET_REPORT\n");
        /* Allow probe to continue; hid_poll uses GET_REPORT path */
    }

    /* SET_PROTOCOL 0 = boot protocol (class request, interface target) */
    usb_control_transfer(dev, 0x21, 0x0B, 0,
                         intf->bInterfaceNumber, NULL, 0, 1000);

    dev->class_private = hid;

    if (hid_device_count < HID_MAX_DEVICES)
        hid_devices[hid_device_count++] = hid;

    if (hid->protocol == USB_PROTOCOL_KEYBOARD)
        debug_print("[HID] USB Keyboard ready (boot protocol)\n");
    else if (hid->protocol == USB_PROTOCOL_MOUSE)
        debug_print("[HID] USB Mouse ready (boot protocol)\n");
    else
        debug_print("[HID] Unknown HID protocol 0x%02x\n", hid->protocol);

    return 0;
}

/* ── Class driver registration ────────────────────────────────────────────── */

static usb_class_driver_t hid_driver = {
    .name       = "HID",
    .class_code = USB_CLASS_HID,
    .probe      = hid_probe,
    .disconnect = NULL,
};

int hid_init(void)
{
    debug_print("HID: Initializing USB HID driver\n");
    usb_register_class_driver(&hid_driver);
    return 0;
}

/*
 * hid_poll_all() — call from the main kernel loop (or a 10ms timer task).
 * Issues one interrupt-endpoint transfer per device per call.
 * Returns the number of reports processed (>0 if activity seen).
 */
int hid_poll_all(void)
{
    int total = 0;
    for (int i = 0; i < hid_device_count; i++) {
        if (hid_devices[i])
            hid_poll(hid_devices[i]);
        total++;
    }
    return total;
}
