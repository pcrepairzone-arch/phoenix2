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

/* USB Core Functions */
void usb_register_class_driver(usb_class_driver_t *driver);
int usb_control_transfer(usb_device_t *dev, uint8_t request_type,
                         uint8_t request, uint16_t value, uint16_t index,
                         void *data, uint16_t length, int timeout);
int usb_bulk_transfer(usb_endpoint_t *ep, void *data, size_t len, int timeout);
int usb_interrupt_transfer(usb_endpoint_t *ep, void *data, size_t len, int timeout);

/* For UASP multi-stream */
int usb_bulk_transfer_stream(usb_endpoint_t *ep, void *data, size_t len,
                             int timeout, uint16_t stream_id);

/*
 * usb_enumerate_device — probe class drivers for a newly-addressed device.
 *
 * Called after the HCD has assigned a USB address and retrieved the device
 * descriptor into @dev. Walks dev->interfaces[] against registered class
 * drivers and calls driver->probe() on each class-code match.
 *
 * If no interfaces are populated (num_interfaces == 0) the function logs a
 * diagnostic and returns 0 — the HCD must parse the Configuration Descriptor
 * and populate dev->interfaces[] before calling this.
 *
 * @dev   Fully (or partially) populated USB device struct
 * @port  Zero-based port index (for logging only)
 * @return 0 always; partial bind failures are logged, not fatal
 */
int usb_enumerate_device(usb_device_t *dev, int port);

/*
 * usb_hc_ops_t — Host Controller Operations table.
 *
 * The USB core is host-controller-agnostic. Any HCD (Host Controller Driver)
 * registers itself here via usb_register_hc(). The core transfer functions
 * (usb_control_transfer etc.) call through this table.
 *
 * Currently only one HCD is supported (xHCI for VL805).
 * Future: support Pi 5 RP1 xHCI as a second HCD instance.
 */
typedef struct usb_hc_ops {
    /*
     * control_transfer — perform a USB control transfer on EP0.
     *
     * @dev          Target USB device (address, slot, speed)
     * @request_type bmRequestType byte (direction | type | recipient)
     * @request      bRequest byte
     * @value        wValue
     * @index        wIndex (language ID for string descriptors)
     * @data         Data buffer (IN: filled by HCD; OUT: sent to device)
     * @length       Number of bytes to transfer
     * @timeout      Timeout in milliseconds
     * @return       Bytes transferred (>=0) or -1 on error
     */
    int (*control_transfer)(usb_device_t *dev,
                            uint8_t request_type, uint8_t request,
                            uint16_t value, uint16_t index,
                            void *data, uint16_t length, int timeout);

    /*
     * bulk_transfer — perform a USB bulk transfer.
     *
     * @ep       Bulk endpoint (address encodes direction in bit 7)
     * @data     Data buffer
     * @len      Number of bytes
     * @timeout  Timeout in milliseconds
     * @return   Bytes transferred or -1 on error
     */
    int (*bulk_transfer)(usb_endpoint_t *ep, void *data, size_t len, int timeout);

    /*
     * interrupt_transfer — perform a USB interrupt transfer (polling).
     *
     * @ep       Interrupt endpoint
     * @data     Data buffer (filled on IN)
     * @len      Max bytes to transfer
     * @timeout  Timeout in milliseconds (0 = non-blocking check)
     * @return   Bytes received or -1 on error/timeout
     */
    int (*interrupt_transfer)(usb_endpoint_t *ep, void *data, size_t len, int timeout);

    /*
     * enumerate_device — assign a USB address and set up device context.
     *
     * Called by usb_core after port reset. The HCD assigns an address,
     * populates dev->address, and prepares EP0 for control transfers.
     *
     * @dev   Partially-filled device struct (speed, port known)
     * @port  Zero-based port index
     * @return 0 on success, -1 on failure
     */
    int (*enumerate_device)(usb_device_t *dev, int port);
} usb_hc_ops_t;

/*
 * usb_register_hc — register a host controller with the USB core.
 *
 * Must be called by the HCD (e.g. xhci_init()) after the hardware is
 * ready. After registration, usb_control_transfer() etc. will route
 * through the ops table.
 *
 * @ops  Pointer to a static usb_hc_ops_t populated by the HCD.
 */
void usb_register_hc(usb_hc_ops_t *ops);

/* USB Initialization */
int usb_init(void);

#endif /* DRIVERS_USB_H */
