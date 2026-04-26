/*
 * usb_init.c - USB Subsystem Initialization
 *
 * Pi 4 has two completely independent USB controllers:
 *
 *   1. VL805 xHCI (PCIe) -- 4x USB-A ports  -- usb_xhci.c
 *   2. DWC2 OTG  (SoC)   -- USB-C OTG port  -- usb_dwc2.c
 *
 * boot165 init ordering:
 *   pci_init() (kernel step 7) calls xhci_init() directly — hardware
 *   is ready but port_scan() is intentionally deferred (removed from
 *   xhci_init).  usb_init() (kernel step 8) registers class drivers
 *   first, then calls xhci_scan_ports() so enumeration sees the drivers.
 *   DWC2 initialises last but does NOT call usb_register_hc() — xHCI owns
 *   g_hc_ops permanently.  usb_bulk_transfer() now routes xHCI endpoints
 *   directly via ep->slot_id (usb_core.c boot165 fix) so g_hc_ops is only
 *   consulted for non-xHCI bulk (none currently exist on this hardware).
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
    /* boot178: input event queues must be ready before the first HID probe */
    extern int keyboard_init(void)         __attribute__((weak));
    extern int mouse_init(void)            __attribute__((weak));

    if (usb_hub_init)          usb_hub_init();
    if (hid_init)              hid_init();
    if (usb_mass_storage_init) usb_mass_storage_init();
    if (keyboard_init)         keyboard_init();
    if (mouse_init)            mouse_init();

    /* ── Step 2: xHCI port scan (VL805, USB-A ports) ───────────────────────
     * xhci_init() was called by pci_init() in step 7 — hardware is ready.
     * port_scan() was deferred until now so class drivers are available.    */
    xhci_scan_ports();

    /* ── Step 3: DWC2 OTG (USB-C port, SoC internal) ───────────────────────
     * boot165: do NOT call usb_register_hc(&g_dwc2_hc_ops) — that overwrites
     * g_hc_ops and breaks all post-enumeration xHCI bulk transfers.
     * DWC2 is a device-mode (gadget) controller on the Pi 4 USB-C port;
     * it does not need to own the host-side g_hc_ops.
     * xHCI bulk routing is now done directly via ep->slot_id in usb_core.c,
     * so g_hc_ops can stay pointing at xHCI for any remaining host ops.     */
    if (dwc2_init() == 0) {
        dwc2_scan_ports();
        debug_print("[USB] DWC2 USB 2.0 OTG ready\n");
    } else {
        uart_puts("[USB] DWC2 initialization failed\n");
    }

    return 0;
}
