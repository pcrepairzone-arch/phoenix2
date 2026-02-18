/*
 * usb_gadget.c – USB Gadget Driver for RISC OS Phoenix
 * Device-side USB (e.g., Pi as mass storage or RNDIS Ethernet)
 * Uses DWC2 controller on Pi (configurable for host/device)
 * Author:R Andrews Grok 4 – 06 Feb 2026
 */

#include "kernel.h"
#include "usb_gadget.h"
#include "vfs.h"
#include "blockdev.h"
// // #include <string.h> /* removed - use kernel.h */ /* removed - use kernel.h */

#define DWC2_BASE       0xFE980000ULL  // Pi USB DWC2
#define DWC2_MAX_EPS    8

typedef struct dwc2_ctrl {
    void *regs;
    uint32_t mode;  // 0=host, 1=device
    usb_gadget_t gadget;
} dwc2_ctrl_t;

static dwc2_ctrl_t *dwc2_ctrl;

/* DWC2 registers (simplified) */
#define DWC2_GOTGCTL    0x0000
#define DWC2_GUSBCFG    0x0008
#define DWC2_GRSTCTL    0x0020
#define DWC2_GINTSTS    0x0014
#define DWC2_DCTL       0x134
#define DWC2_DSTS       0x138
#define DWC2_DIEPCTL(n) (0x220 + (n)*0x20)  // Device IN EP control
#define DWC2_DOEPCTL(n) (0xB00 + (n)*0x20)  // Device OUT EP control

/* Gadget mode init */
static int dwc2_init_gadget(void) {
    dwc2_ctrl = kmalloc(sizeof(dwc2_ctrl_t));
    if (!dwc2_ctrl) return -1;

    dwc2_ctrl->regs = ioremap(DWC2_BASE, 0x1000);
    if (!dwc2_ctrl->regs) goto fail;

    // Soft reset
    writel(0x80000000, dwc2_ctrl->regs + DWC2_GRSTCTL);
    while (readl(dwc2_ctrl->regs + DWC2_GRSTCTL) & 0x80000000);

    // Set device mode
    writel(0x00000003, dwc2_ctrl->regs + DWC2_GUSBCFG);  // PHY device mode

    // Enable global interrupts
    writel(0x00004000, dwc2_ctrl->regs + DWC2_GINTMSK);  // Device interrupts

    dwc2_ctrl->gadget.idVendor = USB_GADGET_VENDOR_ID;
    dwc2_ctrl->gadget.idProduct = USB_GADGET_PRODUCT_ID;
    dwc2_ctrl->gadget.bcdUSB = 0x0200;  // USB 2.0

    debug_print("DWC2 gadget initialized\n");
    return 0;

fail:
    kfree(dwc2_ctrl);
    return -1;
}

/* Start gadget */
int usb_gadget_start(usb_gadget_t *gadget) {
    // Pull-up enable
    writel(readl(dwc2_ctrl->regs + DWC2_DCTL) | (1 << 16), dwc2_ctrl->regs + DWC2_DCTL);

    gadget->configured = 0;
    debug_print("USB gadget started\n");
    return 0;
}

/* Stop gadget */
void usb_gadget_stop(usb_gadget_t *gadget) {
    writel(readl(dwc2_ctrl->regs + DWC2_DCTL) & ~(1 << 16), dwc2_ctrl->regs + DWC2_DCTL);
    debug_print("USB gadget stopped\n");
}

/* Endpoint queue (stub – TRB setup) */
int usb_gadget_ep_queue(usb_gadget_endpoint_t *ep, void *req, size_t len) {
    // Queue to EP ring (stub)
    return 0;
}

/* Endpoint dequeue */
int usb_gadget_ep_dequeue(usb_gadget_endpoint_t *ep, void *req) {
    // Cancel request (stub)
    return 0;
}

/* Mass storage function (example) */
static int mass_storage_bind(usb_gadget_config_t *config) {
    // Add MSC endpoints (EP1 IN/OUT bulk)
    usb_gadget_endpoint_t *in_ep = &config->gadget->endpoints[1];
    in_ep->bEndpointAddress = 0x81;  // EP1 IN
    in_ep->bmAttributes = 0x02;     // Bulk
    in_ep->wMaxPacketSize = 512;

    usb_gadget_endpoint_t *out_ep = &config->gadget->endpoints[2];
    out_ep->bEndpointAddress = 0x01;  // EP1 OUT
    out_ep->bmAttributes = 0x02;
    out_ep->wMaxPacketSize = 512;

    config->gadget->num_endpoints = 3;  // + EP0
    debug_print("Mass storage function bound\n");
    return 0;
}

static void mass_storage_setup(usb_gadget_t *gadget, usb_setup_t *setup) {
    // Handle SCSI commands via blockdev (stub)
    if (setup->bRequest == 0xFE) {  // SCSI command
        // Dispatch to blockdev_read/write
    }
}

usb_gadget_function_t mass_storage_func = {
    .name = "mass_storage",
    .bind = mass_storage_bind,
    .setup = mass_storage_setup
};

/* Default composite config */
usb_gadget_config_t default_config = {
    .bConfigurationValue = 1,
    .bmAttributes = 0x80,  // Bus-powered
    .bMaxPower = 50,       // 100 mA
    .functions = { &mass_storage_func },
    .num_functions = 1
};

/* Module init */
_kernel_oserror *module_init(const char *arg, int podule)
{
    usb_gadget_register_config(&default_config);
    dwc2_init_gadget();
    usb_gadget_start(&default_config.gadget);
    debug_print("USB Gadget loaded – device mode ready\n");
    return NULL;
}