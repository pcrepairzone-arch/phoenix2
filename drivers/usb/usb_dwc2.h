/*
 * usb_dwc2.h — Synopsys DWC2 USB OTG controller (Pi 4 SoC internal)
 *
 * The Pi 4 has two independent USB subsystems:
 *
 *   1. VL805 xHCI (PCIe)  — 4× USB-A ports — usb_xhci.c
 *   2. DWC2 OTG  (SoC)    — USB-C OTG port  — usb_dwc2.c  ← this file
 *
 * These controllers are completely independent.  DWC2 init and operation
 * has no effect on the VL805 and vice versa.  xHCI should NOT hand off
 * any ports to DWC2 — the port scan in usb_xhci.c handles all VL805
 * ports (including USB2 companion ports with DR=1) independently.
 *
 * DWC2 physical address: 0xFE980000 (CPU), 0x7E980000 (VideoCore bus).
 * Already in Device nGnRnE zone in Phoenix OS identity map — no MMU
 * changes needed.
 */

#ifndef USB_DWC2_H
#define USB_DWC2_H

#include <stdint.h>

/* Initialise the DWC2 controller and detect any connected device.
 * Returns 0 on success, -1 if controller not present or init failed.
 * Safe to call even if no device is connected.                       */
int  dwc2_init(void);

/* Returns 1 if dwc2_init() succeeded and a device was detected.    */
int  dwc2_device_present(void);

/* Called from the main USB subsystem init — runs completely separate
 * from xHCI.  Does its own port detection and device enumeration.   */
int  dwc2_start(void);

#endif /* USB_DWC2_H */
