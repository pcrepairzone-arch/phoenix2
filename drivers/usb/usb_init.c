/**
 * @file usb_init.c
 * @brief USB Subsystem Initialization
 * 
 * Integrates USB host controller with PCI bus and initializes USB stack.
 * 
 * @author Phoenix RISC OS Team
 * @since v56
 */

#include "kernel.h"
#include "pci.h"
#include "usb_xhci.h"

/* VL805 xHCI Controller IDs */
#define VL805_VENDOR_ID  0x1106  /* VIA Technologies */
#define VL805_DEVICE_ID  0x3483  /* VL805 USB 3.0 Host Controller */

/**
 * @brief PCI probe function for VL805
 * 
 * Called by PCI subsystem when VL805 xHCI controller is found.
 * 
 * @param pdev PCI device structure
 * @return 0 on success, negative on error
 */
static int vl805_probe(pci_dev_t *pdev)
{
    debug_print("[USB] VL805 xHCI controller found!\n");
    debug_print("[USB]   Vendor: 0x%04x Device: 0x%04x\n",
                pdev->vendor_id, pdev->device_id);
    debug_print("[USB]   Class: 0x%04x (USB xHCI)\n", pdev->class_code);
    
    /* Enable bus mastering for DMA */
    pci_enable_busmaster(pdev);
    debug_print("[USB] Bus mastering enabled\n");
    
    /* Get BAR0 (xHCI registers) */
    uint64_t bar0 = pci_bar_start(pdev, 0);
    if (!bar0) {
        debug_print("[USB] Failed to get BAR0!\n");
        return -1;
    }
    
    debug_print("[USB] xHCI registers at BAR0: 0x%llx\n", bar0);
    
    /* Initialize xHCI controller */
    int ret = xhci_init(bar0);
    if (ret < 0) {
        debug_print("[USB] xHCI initialization failed!\n");
        return ret;
    }
    
    debug_print("[USB] xHCI controller initialized successfully\n");
    
    /* Scan for connected devices */
    int num_devices = xhci_scan_ports();
    debug_print("[USB] Port scan complete: %d device(s) found\n", num_devices);
    
    return 0;
}

/**
 * @brief PCI driver structure for VL805
 */
static pci_driver_t vl805_driver = {
    .name = "VL805 xHCI",
    .vendor_id = VL805_VENDOR_ID,
    .device_id = VL805_DEVICE_ID,
    .class_code = 0x0C0330,  /* USB xHCI controller */
    .probe = vl805_probe
};

/**
 * @brief Initialize USB subsystem
 * 
 * Registers USB drivers with PCI subsystem.
 * Should be called after PCI initialization.
 * 
 * @return 0 on success
 * @since v56
 */
int usb_init(void)
{
    debug_print("\n[USB] Initializing USB subsystem\n");
    
    /* Register VL805 driver with PCI */
    pci_register_driver(&vl805_driver);
    debug_print("[USB] VL805 driver registered\n");
    
    /* PCI scan will call our probe function if VL805 is found */
    debug_print("[USB] USB subsystem ready\n");
    debug_print("[USB] Waiting for PCI scan to detect VL805...\n");
    
    return 0;
}
