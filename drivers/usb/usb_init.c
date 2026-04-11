/*
 * usb_init.c - USB Subsystem Initialization
 *
 * Pi 4 has two completely independent USB controllers:
 *
 *   1. VL805 xHCI (PCIe) -- 4x USB-A ports  -- usb_xhci.c
 *   2. DWC2 OTG  (SoC)   -- USB-C OTG port  -- usb_dwc2.c
 *
 * boot157 init ordering:
 *   pci_init() (kernel step 7) calls xhci_init() directly — hardware
 *   is ready but port_scan() is intentionally deferred (removed from
 *   xhci_init).  usb_init() (kernel step 8) registers class drivers
 *   first, then calls xhci_scan_ports() so enumeration sees the drivers.
 *   DWC2 comes last because usb_register_hc(&g_dwc2_hc_ops) overwrites
 *   the xHCI HC ops — xhci_scan_ports() must complete before that happens.
 */

#include "kernel.h"
#include "usb_xhci.h"
#include "usb_dwc2.h"

extern usb_hc_ops_t g_dwc2_hc_ops;
extern void uart_puts(const char *s);

int usb_init(void)
{
    debug_print("[USB] Initializing USB subsystem\n");

    /* ── Step 1: Register class drivers ────────────────────────────────────
     * Must happen before xhci_scan_ports() so usb_enumerate_device() finds
     * drivers when the first device is presented.
     * Hub is first — it must enumerate downstream devices before mass-storage
     * tries to bind to them.                                                */
    extern int usb_hub_init(void)          __attribute__((weak));
    extern int hid_init(void)              __attribute__((weak));
    extern int usb_mass_storage_init(void) __attribute__((weak));

    if (usb_hub_init)          usb_hub_init();
    if (hid_init)              hid_init();
    if (usb_mass_storage_init) usb_mass_storage_init();

    /* ── Step 2: xHCI port scan (VL805, USB-A ports) ───────────────────────
     * xhci_init() was called by pci_init() in step 7 — hardware is ready.
     * port_scan() was deferred until now so class drivers are available.    */
    xhci_scan_ports();

    /* ── Step 3: DWC2 OTG (USB-C port, SoC internal) ───────────────────────
     * usb_register_hc(&g_dwc2_hc_ops) overwrites g_hc_ops — xhci_scan_ports
     * must be complete before this runs.                                     */
    if (dwc2_init() == 0) {
        usb_register_hc(&g_dwc2_hc_ops);
        dwc2_scan_ports();
        debug_print("[USB] DWC2 USB 2.0 OTG ready\n");
    } else {
        uart_puts("[USB] DWC2 initialization failed\n");
    }

    return 0;
}
