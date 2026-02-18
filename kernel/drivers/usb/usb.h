/*
 * usb.h – USB Headers for RISC OS Phoenix
 * Defines USB structures, class drivers, transfer functions
 * Supports USB 3.2 + UASP streams
 * Author: R Andrews Grok 4 – 04 Feb 2026
 */

#ifndef USB_H
#define USB_H

#include <stdint.h>

#define USB_CLASS_MSC       0x08
#define USB_SUBCLASS_SCSI   0x06
#define USB_PROTOCOL_BULK   0x50
#define USB_PROTOCOL_UASP   0x62

#define USB_MAX_DEVICES     128
#define USB_MAX_INTERFACES  8
#define USB_MAX_ENDPOINTS   16

typedef struct usb_device usb_device_t;
typedef struct usb_interface usb_interface_t;
typedef struct usb_endpoint usb_endpoint_t;
typedef struct usb_class_driver usb_class_driver_t;

struct usb_endpoint {
    uint8_t bEndpointAddress;   // Endpoint number + direction
    uint8_t bmAttributes;       // Transfer type (bulk, isoc, etc.)
    uint16_t wMaxPacketSize;    // Max packet size
    uint8_t bInterval;          // Polling interval
    // ... other fields (interval, max_burst for USB3)
};

struct usb_interface {
    uint8_t bInterfaceNumber;   // Interface number
    uint8_t bAlternateSetting;
    uint8_t bNumEndpoints;      // Number of endpoints
    uint8_t bInterfaceClass;
    uint8_t bInterfaceSubClass;
    uint8_t bInterfaceProtocol;
    usb_endpoint_t endpoints[USB_MAX_ENDPOINTS];
};

struct usb_device {
    uint8_t idVendor;
    uint8_t idProduct;
    uint8_t bcdUSB;             // USB spec version
    usb_interface_t interfaces[USB_MAX_INTERFACES];
    int num_interfaces;
    // ... other fields (config desc, speed, hub)
};

struct usb_class_driver {
    uint8_t class_code;         // USB class (e.g., 0x08 for MSC)
    int (*probe)(usb_device_t *dev, usb_interface_t *intf);
};

void usb_register_class_driver(usb_class_driver_t *driver);

int usb_bulk_transfer(usb_endpoint_t *ep, void *data, size_t len, int timeout);
int usb_bulk_transfer_stream(usb_endpoint_t *ep, void *data, size_t len, int timeout, uint16_t stream_id);

#endif /* USB_H */