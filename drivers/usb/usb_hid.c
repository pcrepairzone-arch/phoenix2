/*
 * usb_hid.c - USB HID (Keyboard/Mouse) driver for Phoenix
 * Adapted from R Andrews USB stack
 * This is the MOST CRITICAL driver - makes system usable!
 */

#include "kernel.h"
#include "usb.h"
#include <string.h>

/* HID Report Descriptor item types */
#define HID_ITEM_MAIN_INPUT        0x80
#define HID_ITEM_MAIN_OUTPUT       0x90
#define HID_ITEM_MAIN_FEATURE      0xB0

/* HID Boot Protocol keyboard report */
typedef struct {
    uint8_t modifiers;    /* Ctrl, Shift, Alt, GUI */
    uint8_t reserved;
    uint8_t keycodes[6];  /* Up to 6 simultaneous keys */
} hid_keyboard_report_t;

/* HID Boot Protocol mouse report */
typedef struct {
    uint8_t buttons;      /* Button bits */
    int8_t  x;            /* X movement */
    int8_t  y;            /* Y movement */
    int8_t  wheel;        /* Wheel movement (optional) */
} hid_mouse_report_t;

/* HID device private data */
typedef struct {
    usb_device_t    *dev;
    usb_interface_t *intf;
    usb_endpoint_t  *int_in;     /* Interrupt IN endpoint */
    uint8_t         protocol;     /* Keyboard=1, Mouse=2 */
    uint8_t         last_report[8];
} hid_device_t;

/* Registered HID devices (up to 4: typically keyboard + mouse + 2 spare) */
#define HID_MAX_DEVICES 4
static hid_device_t *hid_devices[HID_MAX_DEVICES];
static int           hid_device_count = 0;

/* Scancode to ASCII table (US layout) */
static const char scancode_to_ascii[] = {
    0,   0,   0,   0,   'a', 'b', 'c', 'd', /* 0x00-0x07 */
    'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l', /* 0x08-0x0F */
    'm', 'n', 'o', 'p', 'q', 'r', 's', 't', /* 0x10-0x17 */
    'u', 'v', 'w', 'x', 'y', 'z', '1', '2', /* 0x18-0x1F */
    '3', '4', '5', '6', '7', '8', '9', '0', /* 0x20-0x27 */
    '\n', 0x1B, '\b', '\t', ' ', '-', '=', '[', /* 0x28-0x2F */
    ']', '\\', '#', ';', '\'', '`', ',', '.', /* 0x30-0x37 */
    '/',  0,   0,   0,   0,   0,   0,   0,    /* 0x38-0x3F */
};

/* Process keyboard report */
static void hid_process_keyboard(hid_device_t *hid, const uint8_t *data)
{
    hid_keyboard_report_t *report = (hid_keyboard_report_t *)data;
    
    /* Check for new keypresses */
    for (int i = 0; i < 6; i++) {
        uint8_t keycode = report->keycodes[i];
        
        if (keycode == 0) continue;  /* No key */
        if (keycode > 0x65) continue; /* Out of range */
        
        /* Check if this is a new keypress (not in last report) */
        int found = 0;
        for (int j = 0; j < 6; j++) {
            if (hid->last_report[j + 2] == keycode) {
                found = 1;
                break;
            }
        }
        
        if (!found) {
            /* New keypress! */
            char ch = scancode_to_ascii[keycode];
            
            /* Apply shift modifier */
            if (report->modifiers & 0x22) {  /* Left/Right Shift */
                if (ch >= 'a' && ch <= 'z') {
                    ch = ch - 'a' + 'A';
                }
                /* TODO: Handle shifted numbers and symbols */
            }
            
            if (ch) {
                /* TODO: Send to console input buffer */
                debug_print("Key: '%c' (0x%02x)\n", ch, ch);
                
                /* For now, just echo to console — con_putc() is in drivers/gpu/framebuffer.c */
                extern void con_putc(char c);
                con_putc(ch);
            }
        }
    }
    
    /* Save this report for next comparison */
    for (int i = 0; i < 8; i++) {
        hid->last_report[i] = data[i];
    }
}

/* Process mouse report */
static void hid_process_mouse(hid_device_t *hid, const uint8_t *data)
{
    hid_mouse_report_t *report = (hid_mouse_report_t *)data;
    
    /* TODO: Send mouse movement to WIMP/GUI layer */
    if (report->x || report->y) {
        debug_print("Mouse: dx=%d dy=%d buttons=0x%02x\n",
                   report->x, report->y, report->buttons);
    }
}

/* USB HID interrupt handler (polled mode) */
void hid_poll(hid_device_t *hid)
{
    uint8_t report[8];
    int len;

    if (hid->int_in) {
        /*
         * Try interrupt endpoint transfer first.
         * xhci_interrupt_transfer() implements this as GET_REPORT over EP0
         * (boot protocol fallback), which works for any HID boot-protocol
         * device without needing a separate interrupt endpoint ring.
         */
        len = usb_interrupt_transfer(hid->int_in, report, 8, 10);
    } else {
        /*
         * No interrupt endpoint — use GET_REPORT directly.
         * bmRequestType=0xA1 (IN, Class, Interface), bRequest=0x01 (GET_REPORT)
         * wValue=0x0100 (Input report, ID 0), wIndex=interface number.
         */
        len = usb_control_transfer(hid->dev,
                                   0xA1, 0x01, 0x0100,
                                   hid->intf->bInterfaceNumber,
                                   report, 8, 100);
    }
    
    if (len > 0) {
        if (hid->protocol == USB_PROTOCOL_KEYBOARD) {
            hid_process_keyboard(hid, report);
        } else if (hid->protocol == USB_PROTOCOL_MOUSE) {
            hid_process_mouse(hid, report);
        }
    }
}

/* HID class driver probe */
static int hid_probe(usb_device_t *dev, usb_interface_t *intf)
{
    debug_print("HID: Probing interface %d (protocol 0x%02x)\n",
               intf->bInterfaceNumber, intf->bInterfaceProtocol);
    
    /* Only support boot protocol for now */
    if (intf->bInterfaceSubClass != USB_SUBCLASS_BOOT) {
        debug_print("HID: Not boot protocol, skipping\n");
        return -1;
    }
    
    /* Allocate HID device */
    hid_device_t *hid = kmalloc(sizeof(hid_device_t));
    if (!hid) {
        debug_print("HID: kmalloc failed\n");
        return -1;
    }
    memset(hid, 0, sizeof(hid_device_t));
    
    hid->dev = dev;
    hid->intf = intf;
    hid->protocol = intf->bInterfaceProtocol;
    
    /* Find interrupt IN endpoint */
    for (int i = 0; i < intf->endpoint_count; i++) {
        usb_endpoint_t *ep = &intf->endpoints[i];
        if ((ep->bmAttributes & 0x03) == 0x03) {  /* Interrupt */
            if (ep->bEndpointAddress & 0x80) {    /* IN */
                hid->int_in = ep;
                break;
            }
        }
    }
    
    if (!hid->int_in) {
        debug_print("HID: No interrupt IN endpoint\n");
        /* kfree(hid); */
        return -1;
    }
    
    /* Set boot protocol mode */
    usb_control_transfer(dev, 0x21, 0x0B, 0, intf->bInterfaceNumber,
                        NULL, 0, 1000);
    
    dev->class_private = hid;

    /* Register in device table for poll loop */
    if (hid_device_count < HID_MAX_DEVICES)
        hid_devices[hid_device_count++] = hid;

    if (hid->protocol == USB_PROTOCOL_KEYBOARD) {
        debug_print("HID: USB Keyboard detected!\n");
    } else if (hid->protocol == USB_PROTOCOL_MOUSE) {
        debug_print("HID: USB Mouse detected!\n");
    }
    
    return 0;
}

/* HID class driver */
static usb_class_driver_t hid_driver = {
    .name = "HID",
    .class_code = USB_CLASS_HID,
    .probe = hid_probe,
    .disconnect = NULL,
};

/* Initialize HID driver */
int hid_init(void)
{
    debug_print("HID: Initializing USB HID driver\n");
    usb_register_class_driver(&hid_driver);
    return 0;
}

/*
 * hid_poll_all() — call from main kernel loop to service all HID devices.
 * Each call polls every registered keyboard/mouse for a new report.
 * Returns total number of events processed across all devices.
 */
int hid_poll_all(void)
{
    int total = 0;
    for (int i = 0; i < hid_device_count; i++) {
        if (hid_devices[i])
            hid_poll(hid_devices[i]);
    }
    return total;
}
