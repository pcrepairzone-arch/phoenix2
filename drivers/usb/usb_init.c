/*
 * usb_init.c - USB Subsystem Initialization — DWC2 ONLY MODE
 */

#include "kernel.h"
#include "pci.h"
#include "usb_xhci.h"
#include "usb_dwc2.h"

/* VL805 IDs (kept for future use) */
#define VL805_VENDOR_ID  0x1106
#define VL805_DEVICE_ID  0x3483

/* Explicit declarations */
extern usb_hc_ops_t g_dwc2_hc_ops;
extern int  dwc2_scan_ports(void);
extern void uart_puts(const char *s);

/* PCI probe for VL805 (stub — does nothing) */
static int vl805_probe(pci_dev_t *pdev)
{
    debug_print("[USB] VL805 xHCI found but SKIPPED (DWC2-only mode)\n");
    return 0;
}

static pci_driver_t vl805_driver = {
    .name       = "VL805 xHCI",
    .vendor_id  = VL805_VENDOR_ID,
    .device_id  = VL805_DEVICE_ID,
    .class_code = 0x0C0330,
    .probe      = vl805_probe
};

/* Main USB init — DWC2 runs FIRST and ALWAYS */
int usb_init(void)
{
    uart_puts("\n");
    uart_puts("=== DWC2 TEST STRING 2026-04-02 21:20 ===\n");
    uart_puts("================================================\n");
    uart_puts("=== usb_init() CALLED — DWC2 ONLY MODE ===\n");
    uart_puts("=== STARTING DWC2 USB 2.0 DRIVER NOW ===\n");
    uart_puts("=== xHCI / VL805 PATH SKIPPED (watchdog freeze) ===\n");
    uart_puts("================================================\n\n");

    debug_print("[USB] Initializing USB subsystem (DWC2 first)\n");

    /* Register VL805 driver (harmless stub) */
    pci_register_driver(&vl805_driver);

    /* DWC2 USB 2.0 SoC controller — runs unconditionally */
    if (dwc2_init() == 0) {
        usb_register_hc(&g_dwc2_hc_ops);
        dwc2_scan_ports();
        debug_print("[USB] DWC2 USB 2.0 fallback ready\n");
    } else {
        uart_puts("[USB] DWC2 initialization failed — OTG port unavailable\n");
    }

    return 0;
}
