/*
 * usb_core.c - USB Core for Phoenix RISC OS
 *
 * Provides a host-controller-agnostic USB API that class drivers (HID,
 * mass storage) use without knowing whether the HCD is xHCI, OHCI, etc.
 *
 * ── ARCHITECTURE ────────────────────────────────────────────────────────────
 *
 *   Class Driver (usb_hid.c, usb_mass_storage.c)
 *         │  calls  usb_control_transfer() / usb_bulk_transfer() / etc.
 *         ▼
 *   USB Core (usb_core.c)  ← you are here
 *         │  dispatches via g_hc_ops (usb_hc_ops_t)
 *         ▼
 *   Host Controller Driver (usb_xhci.c)
 *         │  programs xHCI rings, waits for CCE
 *         ▼
 *   VL805 xHCI hardware / VL805 MCU firmware
 *
 * ── CURRENT STATUS ──────────────────────────────────────────────────────────
 *
 *   The xHCI driver (usb_xhci.c) performs inline enumeration during init
 *   and populates device descriptors directly.  Once the Enable Slot race
 *   is resolved (CRCR split-write fix + doorbell timing fix applied in
 *   this patch), the inline enumeration will succeed and class drivers
 *   can be probed.
 *
 *   To connect class drivers to a discovered device:
 *     1. xhci_init() successfully enumerates a device
 *     2. Populate a usb_device_t from the descriptor bytes
 *     3. Call usb_enumerate_device(dev, port)
 *     4. usb_enumerate_device probes registered class drivers
 *     5. Class driver calls usb_control_transfer / usb_bulk_transfer
 *        which now routes through g_hc_ops to the xHCI driver
 *
 * ── REGISTERING THE HCD ─────────────────────────────────────────────────────
 *
 *   In xhci_init() (after the controller is running), call:
 *
 *     static usb_hc_ops_t xhci_ops = {
 *         .control_transfer   = xhci_control_transfer,
 *         .bulk_transfer      = xhci_bulk_transfer,
 *         .interrupt_transfer = xhci_interrupt_transfer,
 *         .enumerate_device   = xhci_enumerate_device,
 *     };
 *     usb_register_hc(&xhci_ops);
 *
 *   The xhci_*_transfer() functions are thin wrappers around the existing
 *   ep0_get_descriptor / cmd_address_device machinery in usb_xhci.c.
 */

#include "kernel.h"
#include "usb.h"

/* ── Global state ───────────────────────────────────────────────────────── */

/* Registered host controller operations (set by usb_register_hc()) */
static usb_hc_ops_t *g_hc_ops = NULL;

/* Registered class drivers (up to 16) */
static usb_class_driver_t *class_drivers[16];
static int class_driver_count = 0;

/* ═══════════════════════════════════════════════════════════════════════════
 * HCD REGISTRATION
 * ═══════════════════════════════════════════════════════════════════════════ */

/*
 * usb_register_hc — register the active Host Controller Driver.
 *
 * Called once by the HCD (xhci_init) after hardware is ready.
 * All subsequent usb_*_transfer() calls route through @ops.
 */
void usb_register_hc(usb_hc_ops_t *ops) {
    if (!ops) {
        debug_print("[USB] usb_register_hc: NULL ops — ignored\n");
        return;
    }
    g_hc_ops = ops;
    debug_print("[USB] Host controller registered\n");
    if (!ops->control_transfer)
        debug_print("[USB]   WARNING: control_transfer not implemented\n");
    if (!ops->bulk_transfer)
        debug_print("[USB]   WARNING: bulk_transfer not implemented\n");
    if (!ops->interrupt_transfer)
        debug_print("[USB]   WARNING: interrupt_transfer not implemented\n");
}

/* ═══════════════════════════════════════════════════════════════════════════
 * CLASS DRIVER REGISTRATION
 * ═══════════════════════════════════════════════════════════════════════════ */

/*
 * usb_register_class_driver — register a USB class driver.
 *
 * Class drivers (HID, mass storage) call this during their init.
 * The driver is probed for every newly enumerated device.
 */
void usb_register_class_driver(usb_class_driver_t *driver) {
    if (class_driver_count >= 16) {
        debug_print("[USB] usb_register_class_driver: table full\n");
        return;
    }
    class_drivers[class_driver_count++] = driver;
    debug_print("[USB] Registered class driver: %s (class 0x%02x)\n",
                driver->name, driver->class_code);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * TRANSFER API
 * ═══════════════════════════════════════════════════════════════════════════ */

/*
 * usb_control_transfer — perform a USB control transfer on EP0.
 *
 * Routes to the registered HCD's control_transfer() method.
 *
 * @dev          Target device (must be addressed; dev->address != 0)
 * @request_type Standard USB bmRequestType byte
 * @request      Standard USB bRequest byte
 * @value        wValue field
 * @index        wIndex field (lang ID for string descriptors)
 * @data         Buffer for data phase (IN: filled; OUT: consumed)
 * @length       Bytes to transfer in data phase
 * @timeout      Milliseconds before giving up
 * @return       Bytes transferred (>=0) or -1 on error
 */
int usb_control_transfer(usb_device_t *dev, uint8_t request_type,
                         uint8_t request, uint16_t value, uint16_t index,
                         void *data, uint16_t length, int timeout) {
    if (!g_hc_ops) {
        debug_print("[USB] usb_control_transfer: no HCD registered\n");
        return -1;
    }
    if (!g_hc_ops->control_transfer) {
        debug_print("[USB] usb_control_transfer: HCD has no control_transfer impl\n");
        return -1;
    }
    return g_hc_ops->control_transfer(dev, request_type, request,
                                      value, index, data, length, timeout);
}

/*
 * usb_bulk_transfer — perform a USB bulk transfer.
 *
 * Used by mass storage class drivers for SCSI command/data phases.
 *
 * @ep       Bulk endpoint (ep->bEndpointAddress bit 7 = direction)
 * @data     Transfer buffer
 * @len      Number of bytes
 * @timeout  Milliseconds
 * @return   Bytes transferred or -1 on error
 */
int usb_bulk_transfer(usb_endpoint_t *ep, void *data, size_t len, int timeout) {
    if (!g_hc_ops) {
        debug_print("[USB] usb_bulk_transfer: no HCD registered\n");
        return -1;
    }
    if (!g_hc_ops->bulk_transfer) {
        debug_print("[USB] usb_bulk_transfer: HCD has no bulk_transfer impl\n");
        return -1;
    }
    return g_hc_ops->bulk_transfer(ep, data, len, timeout);
}

/*
 * usb_interrupt_transfer — perform a USB interrupt transfer (polled).
 *
 * Used by HID class drivers to poll keyboard/mouse state.
 * With timeout=0: non-blocking check (returns -1 if nothing pending).
 * With timeout>0: blocks until data arrives or timeout expires.
 *
 * @ep       Interrupt IN endpoint
 * @data     Buffer to fill
 * @len      Max bytes
 * @timeout  Milliseconds (0 = non-blocking)
 * @return   Bytes received or -1 on error/timeout
 */
int usb_interrupt_transfer(usb_endpoint_t *ep, void *data, size_t len, int timeout) {
    if (!g_hc_ops) {
        debug_print("[USB] usb_interrupt_transfer: no HCD registered\n");
        return -1;
    }
    if (!g_hc_ops->interrupt_transfer) {
        debug_print("[USB] usb_interrupt_transfer: HCD has no interrupt_transfer impl\n");
        return -1;
    }
    return g_hc_ops->interrupt_transfer(ep, data, len, timeout);
}

/*
 * usb_bulk_transfer_stream — bulk transfer with stream ID (USB 3.0 UASP).
 *
 * Stream IDs allow multiple outstanding bulk transfers on the same endpoint.
 * Currently a stub — full implementation requires xHCI stream ring support.
 */
int usb_bulk_transfer_stream(usb_endpoint_t *ep, void *data, size_t len,
                             int timeout, uint16_t stream_id) {
    /* TODO: Pass stream_id to HCD bulk_transfer implementation.
     * For now: fall back to regular bulk (stream_id ignored).
     * UASP performance will be degraded but SCSI commands will work. */
    debug_print("[USB] usb_bulk_transfer_stream: stream_id=%u (using regular bulk)\n",
                stream_id);
    (void)stream_id;
    return usb_bulk_transfer(ep, data, len, timeout);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * DEVICE ENUMERATION
 * ═══════════════════════════════════════════════════════════════════════════ */

/*
 * usb_enumerate_device — probe class drivers for a newly-addressed device.
 *
 * Called after the HCD has:
 *   1. Reset the port
 *   2. Assigned a USB address (dev->address != 0)
 *   3. Retrieved the device descriptor (dev->bDeviceClass etc. populated)
 *   4. Retrieved configuration descriptors (dev->interfaces[] populated)
 *
 * Walks each interface and each registered class driver looking for a
 * class-code match. On match, calls driver->probe(). If probe succeeds
 * (returns 0) the driver binds to the interface.
 *
 * @dev   Fully-populated USB device struct
 * @port  Zero-based port index (for logging)
 * @return 0 (always — partial bind failures are logged, not fatal)
 */
int usb_enumerate_device(usb_device_t *dev, int port) {
    debug_print("[USB] Enumerating device on port %d: VID=%04x PID=%04x class=%u\n",
                port, dev->idVendor, dev->idProduct, dev->bDeviceClass);

    if (dev->num_interfaces == 0) {
        debug_print("[USB]   No interfaces populated — descriptor parse needed\n");
        debug_print("[USB]   TODO: parse configuration descriptor into dev->interfaces[]\n");
        return 0;
    }

    int bound = 0;
    for (int i = 0; i < dev->num_interfaces; i++) {
        usb_interface_t *intf = &dev->interfaces[i];
        debug_print("[USB]   Interface %d: class=0x%02x sub=0x%02x proto=0x%02x\n",
                    intf->bInterfaceNumber,
                    intf->bInterfaceClass,
                    intf->bInterfaceSubClass,
                    intf->bInterfaceProtocol);

        for (int j = 0; j < class_driver_count; j++) {
            usb_class_driver_t *drv = class_drivers[j];
            if (intf->bInterfaceClass != drv->class_code) continue;

            debug_print("[USB]   Probing driver '%s' for interface %d\n",
                        drv->name, intf->bInterfaceNumber);
            if (drv->probe(dev, intf) == 0) {
                debug_print("[USB]   Driver '%s' bound to interface %d\n",
                            drv->name, intf->bInterfaceNumber);
                bound++;
            } else {
                debug_print("[USB]   Driver '%s' declined interface %d\n",
                            drv->name, intf->bInterfaceNumber);
            }
        }
    }

    debug_print("[USB] Port %d: %d class driver(s) bound\n", port, bound);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * SUBSYSTEM INIT
 * ═══════════════════════════════════════════════════════════════════════════ */

/*
 * usb_init — initialise the USB subsystem.
 *
 * Called from kernel_main() after pci_init() has set up the VL805.
 * The xHCI driver (xhci_init) is called separately via pci_init() and
 * registers the HCD ops table via usb_register_hc() during its init.
 *
 * @return 0 always (failures are logged but non-fatal at subsystem level)
 */
int usb_init(void) {
    debug_print("[USB] Initialising USB subsystem\n");
    debug_print("[USB]   g_hc_ops = %s\n",
                g_hc_ops ? "registered" : "NULL (HCD not yet initialised)");
    debug_print("[USB]   Class drivers registered: %d\n", class_driver_count);
    debug_print("[USB] USB subsystem ready\n");
    return 0;
}
