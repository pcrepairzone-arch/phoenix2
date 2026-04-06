/*
 * usb_init.c - USB Subsystem Initialization
 *
 * Pi 4 has two completely independent USB controllers:
 *
 *   1. VL805 xHCI (PCIe) -- 4x USB-A ports  -- usb_xhci.c
 *   2. DWC2 OTG  (SoC)   -- USB-C OTG port  -- usb_dwc2.c
 *
 * xHCI registers itself via pci.c/vl805_probe -- no g_xhci_hc_ops needed here.
 * DWC2 registers via g_dwc2_hc_ops from usb_dwc2.c.
 */

#include "kernel.h"
#include "pci.h"
#include "usb_xhci.h"
#include "usb_dwc2.h"

#define VL805_VENDOR_ID  0x1106
#define VL805_DEVICE_ID  0x3483

extern usb_hc_ops_t g_dwc2_hc_ops;
extern void uart_puts(const char *s);

/* PCI probe for VL805 xHCI */
static int vl805_probe(pci_dev_t *pdev)
{
    void *xhci_base = (void *)0x600000000ULL;
    (void)pdev;
    debug_print("[USB] VL805 xHCI probed\n");
    return xhci_init(xhci_base);
}

static pci_driver_t vl805_driver = {
    .name       = "VL805 xHCI",
    .vendor_id  = VL805_VENDOR_ID,
    .device_id  = VL805_DEVICE_ID,
    .class_code = 0x0C0330,
    .probe      = vl805_probe
};

int usb_init(void)
{
    debug_print("[USB] Initializing USB subsystem\n");

    /* Track 1: VL805 xHCI (USB-A ports via PCIe) */
    pci_register_driver(&vl805_driver);

    /* Track 2: DWC2 OTG (USB-C port, SoC internal) */
    if (dwc2_init() == 0) {
        usb_register_hc(&g_dwc2_hc_ops);
        dwc2_scan_ports();
        debug_print("[USB] DWC2 USB 2.0 OTG ready\n");
    } else {
        uart_puts("[USB] DWC2 initialization failed\n");
    }

    return 0;
}
