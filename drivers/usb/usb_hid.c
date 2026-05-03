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

/* boot261: ARM system counter → milliseconds (same formula as usb_xhci.c) */
static inline uint32_t hid_time_ms(void) {
    uint64_t cnt, freq;
    __asm__ volatile("mrs %0, cntpct_el0" : "=r"(cnt));
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
    return (uint32_t)(cnt / (freq / 1000ULL));
}

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
    uint8_t          get_report_warned;   /* boot179: printed first GET_REPORT result */
    uint8_t          report_id_offset;   /* boot281: 1 if device prepends a report ID byte */
    uint8_t          report_id_detected; /* boot281: 1 once format has been auto-detected */
    uint32_t         last_get_report_ms; /* boot261: rate-limit GET_REPORT to 60 Hz */
    uint32_t         mouse_last_ok_ms;   /* boot286: timestamp of last successful mouse data */
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
            debug_print("[KBD] code=0x%02x inkey=0x%02x char=0x%02x mod=0x%02x\n",
                        code, ev.key_code, ev.key_char, ev.modifiers);

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
    /* boot284: auto-detect report ID prefix — run once on first received report.
     *
     * Since boot283 removed SET_PROTOCOL for mice, all mice operate in their
     * native HID report protocol.  In native mode, most mice send reports as:
     *   [buttons][X][Y][wheel...]   (no report ID prefix, offset=0)
     * A minority prepend a 1-byte report ID before the button byte (offset=1).
     *
     * Detection rule:
     *   data[0] bits 3-7 set → CANNOT be a button byte (buttons use only bits
     *   0-2) → it is a report ID → offset = 1.
     *   Anything else (data[0] == 0x00, or 0x01-0x07) → treat as button byte
     *   → offset = 0.  This is the safe default now that SET_PROTOCOL is gone:
     *   native-mode mice without report IDs are far more common, and a false
     *   positive offset=1 causes all axes to be read from the wrong bytes.    */
    if (!hid->report_id_detected) {
        hid->report_id_detected = 1;
        hid->report_id_offset   = (data[0] & 0xF8) ? 1 : 0;
        debug_print("[HID] mouse fmt: offset=%d "
                    "(raw[0]=0x%02x raw[1]=0x%02x raw[2]=0x%02x)\n",
                    hid->report_id_offset, data[0], data[1], data[2]);
    }

    data += hid->report_id_offset;
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
    int len = -1;

    /* boot278 polling strategy — interrupt IN for BOTH devices.
     *
     * KEYBOARD — interrupt IN, 8ms timeout.
     *   VL805 issues IN tokens when TRB is queued (confirmed boot277).
     *   Keyboard responds within ~1ms when a key is held; times out when idle.
     *   Timeout reduced from 12ms→8ms to cut WIMP loop blocking time.
     *
     * MOUSE — interrupt IN, 2ms timeout.
     *   boot277 confirmed: GET_REPORT does NOT clear this mouse's delta
     *   accumulator.  Once the user stops moving, GET_REPORT returns the last
     *   non-zero delta on every poll → cursor drifts endlessly.
     *   Interrupt IN drains the accumulator correctly: device clears its delta
     *   registers after each IN token response, so a stationary mouse returns
     *   0 bytes (timeout) and the cursor stops. ✓
     *   Short 2ms timeout keeps WIMP responsive while moving.
     *
     * GET_REPORT fallback — only if no interrupt IN endpoint exists.
     */
    if (hid->int_in) {
        int t = (hid->protocol == USB_PROTOCOL_KEYBOARD) ? 8 : 2;
        len = usb_interrupt_transfer(hid->int_in, report, 8, t);

        if (len > 0) {
            if (hid->protocol == USB_PROTOCOL_KEYBOARD &&
                (report[0]|report[2]|report[3]|report[4]|
                 report[5]|report[6]|report[7])) {
                debug_print("[KBD] int_in len=%d "
                            "data=%02x %02x %02x %02x %02x %02x %02x %02x\n",
                            len,
                            report[0], report[1], report[2], report[3],
                            report[4], report[5], report[6], report[7]);
            }
            /* Mouse raw bytes are logged inside hid_process_mouse via [MOUSE] */
        }
    }

    if (len <= 0 && !hid->int_in) {
        /* GET_REPORT fallback — no interrupt endpoint; rate-limited 60 Hz */
        uint32_t now = hid_time_ms();
        if ((now - hid->last_get_report_ms) >= 16u) {
            hid->last_get_report_ms = now;
            len = usb_control_transfer(hid->dev,
                                       0xA1, 0x01, 0x0100,
                                       hid->intf->bInterfaceNumber,
                                       report, 8, 50);
            if (!hid->get_report_warned) {
                hid->get_report_warned = 1;
                debug_print("[HID] GET_REPORT proto=0x%02x len=%d "
                            "data=%02x %02x %02x %02x %02x %02x %02x %02x\n",
                            hid->protocol, len,
                            report[0], report[1], report[2], report[3],
                            report[4], report[5], report[6], report[7]);
            }
        } else {
            len = 0;
        }
    }

    if (len > 0) {
        if (hid->protocol == USB_PROTOCOL_KEYBOARD)
            hid_process_keyboard(hid, report);
        else if (hid->protocol == USB_PROTOCOL_MOUSE) {
            /* boot300: guard against sub-minimum transfers (e.g. the spurious
             * single byte 0x08 delivered by xHCI int_steal / cc=13 Stopped
             * endpoint recovery).  A valid mouse report is buttons+X+Y = 3 bytes
             * minimum (4 with wheel).  If we got less, reset report_id_detected
             * so the auto-detect logic re-runs cleanly on the next real report. */
            if (len < 3) {
                hid->report_id_detected = 0;
            } else {
                hid_process_mouse(hid, report);
            }
        }
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

    /* boot283: SET_PROTOCOL behaviour is device-type specific.
     *
     * KEYBOARD — send SET_PROTOCOL(0) = boot protocol (8-byte report, standard
     *   layout).  Keyboards generally implement boot protocol reliably.
     *
     * MOUSE — do NOT send SET_PROTOCOL(0).  Some mice (e.g. Sunplus VID=1BCF
     *   PID=0007, mps=7) technically acknowledge boot protocol but their
     *   firmware only reports the X-axis in that mode.  Y-axis, buttons, and
     *   wheel only appear in the native HID report protocol.  Leaving the mouse
     *   in report protocol mode gives us all the data we need.
     *
     *   The native format for this family typically is:
     *     [report_id=0x01][buttons][X][Y][wheel][pad...]
     *   which is detected by the existing report_id auto-detection in
     *   hid_process_mouse().  If the native format turns out to be different,
     *   the 8-byte diagnostic log ([HID] mouse8:) will reveal the actual bytes.
     */
    if (intf->bInterfaceProtocol == USB_PROTOCOL_KEYBOARD) {
        usb_control_transfer(dev, 0x21, 0x0B, 0,
                             intf->bInterfaceNumber, NULL, 0, 1000);
        debug_print("[HID] SET_PROTOCOL(boot) sent to keyboard\n");
    } else {
        debug_print("[HID] Leaving mouse in native report protocol (no SET_PROTOCOL)\n");
    }

    /* SET_IDLE 0 — tell keyboard to report current state on every IN token,
     * not just when the state changes (HID spec §7.2.4).
     * wValue = 0x0000: duration=0 (report every poll), report_id=0 (all).
     * Without this, boot keyboards NAK every IN token when nothing changed,
     * which combined with MFINDEX=0 means we might never see a first report.
     * Only applies to keyboards. */
    if (intf->bInterfaceProtocol == USB_PROTOCOL_KEYBOARD) {
        int idle_rc = usb_control_transfer(dev, 0x21, 0x0A, 0x0000,
                                           intf->bInterfaceNumber, NULL, 0, 1000);
        debug_print("[HID] SET_IDLE rc=%d\n", idle_rc);
    }

    dev->class_private = hid;

    if (hid_device_count < HID_MAX_DEVICES)
        hid_devices[hid_device_count++] = hid;

    if (hid->protocol == USB_PROTOCOL_KEYBOARD)
        debug_print("[HID] USB Keyboard ready (boot protocol)\n");
    else if (hid->protocol == USB_PROTOCOL_MOUSE)
        debug_print("[HID] USB Mouse ready (native report protocol)\n");
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
 * hid_poll_all() — call from the main kernel loop at ~16 ms (keyboards only).
 * boot285: mice are now polled by hid_poll_mice() at 4 ms non-blocking so
 * that the interrupt TRB is always freshly armed; only keyboards are handled
 * here with the existing 8 ms blocking wait.
 * Returns the number of devices polled.
 */
int hid_poll_all(void)
{
    int total = 0;
    for (int i = 0; i < hid_device_count; i++) {
        hid_device_t *hid = hid_devices[i];
        if (!hid) continue;
        if (hid->protocol == USB_PROTOCOL_MOUSE) continue; /* polled by hid_poll_mice */
        hid_poll(hid);
        total++;
    }
    return total;
}

/*
 * hid_poll_mice() — non-blocking mouse-only poll, call at ~4 ms.
 *
 * boot285: High-frequency mouse poll strategy.
 *
 * Calling usb_interrupt_transfer with timeout=0 on a mouse that has no
 * pending event is essentially free: the xHCI layer does a single event-ring
 * read and returns immediately (< 5 µs).  Calling at 4 ms means the worst-
 * case accumulation between consecutive reads is one 7 ms mouse report
 * interval, yielding deltas of ~15–30 counts instead of the 127-clamped
 * values we saw at 16 ms.  The TRB is re-armed on the very next 4 ms poll
 * after it completes, so the endpoint is perpetually queued.
 */
void hid_poll_mice(void)
{
    uint32_t now = hid_time_ms();

    for (int i = 0; i < hid_device_count; i++) {
        hid_device_t *hid = hid_devices[i];
        if (!hid || hid->protocol != USB_PROTOCOL_MOUSE) continue;
        if (!hid->int_in) continue;

        uint8_t report[8] = { 0 };
        int len;

        /* boot286: Watchdog — if no mouse data for 500 ms, the TRB may be
         * stale (e.g. wireless receiver lost contact, or Transfer Event was
         * missed).  Fall back to a short 2 ms blocking call which also
         * triggers the DMA-buffer fallback check inside xhci_interrupt_transfer.
         * This ensures the endpoint re-arms promptly when the device comes
         * back into range.                                                   */
        if (hid->mouse_last_ok_ms != 0 && (now - hid->mouse_last_ok_ms) > 500u) {
            hid->mouse_last_ok_ms = now;   /* reset timer; watchdog re-arms silently */
            len = usb_interrupt_transfer(hid->int_in, report, 8, 2); /* 2 ms blocking */
        } else {
            len = usb_interrupt_transfer(hid->int_in, report, 8, 0); /* non-blocking */
        }

        if (len > 0) {
            hid->mouse_last_ok_ms = now;
            hid_process_mouse(hid, report);
        }
    }
}

/*
 * hid_device_disconnect — boot287: remove all HID poll entries for a given
 * xHCI slot_id.
 *
 * Called from xhci_disconnect_slot() (usb_xhci.c) after the slot has been
 * stopped and disabled by the xHCI layer.  At this point the device DMA
 * buffers are invalid and the interrupt endpoint is no longer armed, so we
 * must remove the hid_device_t entries from the poll lists before the next
 * hid_poll_all() / hid_poll_mice() call.
 *
 * The hid_devices[] array is compacted after removal so that no NULL holes
 * are left mid-array; hid_device_count is adjusted accordingly.
 *
 * slot_id: xHCI slot (1-based) whose devices should be removed.
 *          Matched via hid->dev->hcd_private (set to (void*)(uintptr_t)sid
 *          by usb_xhci.c at enumeration time).
 */
void hid_device_disconnect(uint8_t slot_id)
{
    int removed = 0;

    for (int i = 0; i < hid_device_count; i++) {
        hid_device_t *hid = hid_devices[i];
        if (!hid) continue;

        uint8_t hid_slot = (uint8_t)(uintptr_t)hid->dev->hcd_private;
        if (hid_slot != slot_id) continue;

        debug_print("[HID] Removing slot=%u protocol=0x%02x from poll list\n",
                    (unsigned)slot_id, hid->protocol);

        kfree(hid);
        hid_devices[i] = NULL;
        removed++;
    }

    if (removed) {
        /* Compact: shift non-NULL entries down to close any gaps */
        int out = 0;
        for (int i = 0; i < hid_device_count; i++) {
            if (hid_devices[i]) hid_devices[out++] = hid_devices[i];
        }
        for (int i = out; i < hid_device_count; i++)
            hid_devices[i] = NULL;
        hid_device_count = out;

        debug_print("[HID] %d device(s) removed for slot=%u; %d remaining\n",
                    removed, (unsigned)slot_id, hid_device_count);
    }
    /* no HID entries for this slot = normal for MSC / hub disconnects */
}

/*
 * usb_hid_keyboard_count() — return number of enumerated HID keyboards.
 * Called by CursorMod and any future KeyV module.
 */
int usb_hid_keyboard_count(void)
{
    int count = 0;
    for (int i = 0; i < hid_device_count; i++) {
        if (hid_devices[i] && hid_devices[i]->protocol == USB_PROTOCOL_KEYBOARD)
            count++;
    }
    return count;
}

/*
 * usb_hid_mouse_count() — return number of enumerated HID mice/pointers.
 */
int usb_hid_mouse_count(void)
{
    int count = 0;
    for (int i = 0; i < hid_device_count; i++) {
        if (hid_devices[i] && hid_devices[i]->protocol == USB_PROTOCOL_MOUSE)
            count++;
    }
    return count;
}
