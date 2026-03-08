/*
 * usb_gadget.h – USB Gadget Headers for RISC OS Phoenix
 * Defines gadget structures for device-side USB (e.g., mass storage, Ethernet)
 * Supports USB 2.0/3.0 on Raspberry Pi (DWC2/DWC3 controllers)
 * Author: R Andrews Grok 4 – 06 Feb 2026
 */

#ifndef USB_GADGET_H
#define USB_GADGET_H

#include <stdint.h>
#include "usb.h"

#define USB_GADGET_MAX_FUNCS  8
#define USB_GADGET_MAX_EPS    16
#define USB_GADGET_VENDOR_ID  0x1D6B  // Linux Foundation
#define USB_GADGET_PRODUCT_ID 0x0104  // Multifunction Composite

typedef struct usb_gadget usb_gadget_t;
typedef struct usb_gadget_config usb_gadget_config_t;
typedef struct usb_gadget_function usb_gadget_function_t;
typedef struct usb_gadget_endpoint usb_gadget_endpoint_t;

struct usb_gadget_endpoint {
    uint8_t bEndpointAddress;   // EP number + direction
    uint8_t bmAttributes;       // Transfer type
    uint16_t wMaxPacketSize;
    uint8_t bInterval;
    // ... (max_burst for HS/SS)
};

struct usb_gadget_function {
    const char *name;           // e.g., "mass_storage", "ethernet"
    int (*bind)(usb_gadget_config_t *config);  // Setup endpoints
    void (*unbind)(usb_gadget_config_t *config);
    void (*setup)(usb_gadget_t *gadget, usb_setup_t *setup);  // Handle control requests
    // ... (disconnect, suspend, etc.)
};

struct usb_gadget_config {
    uint8_t bConfigurationValue;
    uint8_t bmAttributes;       // Power, self-powered, etc.
    uint8_t bMaxPower;          // mA / 2
    usb_gadget_function_t *functions[USB_GADGET_MAX_FUNCS];
    int num_functions;
};

struct usb_gadget {
    uint16_t idVendor;
    uint16_t idProduct;
    uint16_t bcdUSB;
    usb_gadget_config_t *config;
    usb_gadget_endpoint_t endpoints[USB_GADGET_MAX_EPS];
    int num_endpoints;
    int speed;  // USB_SPEED_FULL, HIGH, SUPER
    void *ctrl; // Controller private (DWC2/DWC3)
    int configured;
    // ... (strings, descriptors)
};

/* Gadget functions */
int usb_gadget_register_config(usb_gadget_config_t *config);
int usb_gadget_start(usb_gadget_t *gadget);
void usb_gadget_stop(usb_gadget_t *gadget);
int usb_gadget_ep_queue(usb_gadget_endpoint_t *ep, void *req, size_t len);
int usb_gadget_ep_dequeue(usb_gadget_endpoint_t *ep, void *req);

extern usb_gadget_t default_gadget;

#endif /* USB_GADGET_H */