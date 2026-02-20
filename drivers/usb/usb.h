/*
 * usb.h - USB definitions for Phoenix RISC OS
 * Adapted from R Andrews USB stack
 * Bare-metal version for Pi 4/5
 */

#ifndef DRIVERS_USB_H
#define DRIVERS_USB_H

#include <stdint.h>
#include <stddef.h>

/* USB Classes */
#define USB_CLASS_HID        0x03
#define USB_CLASS_MSC        0x08
#define USB_SUBCLASS_BOOT    0x01
#define USB_SUBCLASS_SCSI    0x06
#define USB_PROTOCOL_KEYBOARD 0x01
#define USB_PROTOCOL_MOUSE    0x02
#define USB_PROTOCOL_BULK    0x50
#define USB_PROTOCOL_UASP    0x62

/* USB Speeds */
#define USB_SPEED_LOW        0
#define USB_SPEED_FULL       1
#define USB_SPEED_HIGH       2
#define USB_SPEED_SUPER      3
#define USB_SPEED_SUPER_PLUS 4

#define USB_MAX_DEVICES      128
#define USB_MAX_INTERFACES   8
#define USB_MAX_ENDPOINTS    16

/* USB Endpoint */
typedef struct usb_endpoint {
    uint8_t  bEndpointAddress;   /* EP number + direction (bit 7) */
    uint8_t  bmAttributes;       /* Transfer type */
    uint16_t wMaxPacketSize;     /* Max packet size */
    uint8_t  bInterval;          /* Polling interval */
    uint8_t  max_burst;          /* USB 3.0+ burst size */
    uint16_t max_streams;        /* USB 3.0+ stream support */
} usb_endpoint_t;

/* USB Interface */
typedef struct usb_interface {
    uint8_t bInterfaceNumber;
    uint8_t bAlternateSetting;
    uint8_t bNumEndpoints;
    uint8_t bInterfaceClass;
    uint8_t bInterfaceSubClass;
    uint8_t bInterfaceProtocol;
    usb_endpoint_t endpoints[USB_MAX_ENDPOINTS];
    int endpoint_count;
} usb_interface_t;

/* USB Device */
typedef struct usb_device {
    uint16_t idVendor;
    uint16_t idProduct;
    uint16_t bcdUSB;             /* USB version */
    uint8_t  bDeviceClass;
    uint8_t  bDeviceSubClass;
    uint8_t  bDeviceProtocol;
    uint8_t  bMaxPacketSize0;
    uint8_t  address;            /* USB address (1-127) */
    uint8_t  speed;              /* USB_SPEED_* */
    usb_interface_t interfaces[USB_MAX_INTERFACES];
    int num_interfaces;
    void *hcd_private;           /* Host controller private data */
    void *class_private;         /* Class driver private data */
} usb_device_t;

/* USB Class Driver */
typedef struct usb_class_driver {
    const char *name;
    uint8_t class_code;
    int (*probe)(usb_device_t *dev, usb_interface_t *intf);
    void (*disconnect)(usb_device_t *dev);
} usb_class_driver_t;

/* USB Core Functions - TO BE IMPLEMENTED */
void usb_register_class_driver(usb_class_driver_t *driver);
int usb_control_transfer(usb_device_t *dev, uint8_t request_type,
                         uint8_t request, uint16_t value, uint16_t index,
                         void *data, uint16_t length, int timeout);
int usb_bulk_transfer(usb_endpoint_t *ep, void *data, size_t len, int timeout);
int usb_interrupt_transfer(usb_endpoint_t *ep, void *data, size_t len, int timeout);

/* For UASP multi-stream */
int usb_bulk_transfer_stream(usb_endpoint_t *ep, void *data, size_t len, 
                             int timeout, uint16_t stream_id);

/* USB Initialization */
int usb_init(void);

#endif /* DRIVERS_USB_H */
