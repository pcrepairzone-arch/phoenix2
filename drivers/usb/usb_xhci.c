/**
 * @file usb_xhci.c
 * @brief xHCI Host Controller Driver Implementation
 * 
 * Implements USB 3.0 xHCI host controller support for VL805 on Pi 4.
 * This is a minimal implementation focused on getting keyboard/mouse working.
 * 
 * @author Phoenix RISC OS Team
 * @since v56 (February 2026)
 */

#include "kernel.h"
#include "usb_xhci.h"
#include <string.h>

static xhci_controller_t xhci_ctrl;

/**
 * @brief Initialize xHCI controller
 */
int xhci_init(uint64_t base_addr)
{
    debug_print("[xHCI] Initializing controller at 0x%llx\n", base_addr);
    
    memset(&xhci_ctrl, 0, sizeof(xhci_ctrl));
    
    /* Map capability registers */
    xhci_ctrl.cap_regs = ioremap(base_addr, 0x10000);
    if (!xhci_ctrl.cap_regs) {
        debug_print("[xHCI] Failed to map registers\n");
        return -1;
    }
    
    xhci_ctrl.cap_regs_phys = base_addr;
    
    /* Read capability register length */
    uint8_t cap_length = readb(xhci_ctrl.cap_regs + XHCI_CAP_CAPLENGTH);
    uint16_t hci_version = readw(xhci_ctrl.cap_regs + XHCI_CAP_HCIVERSION);
    
    debug_print("[xHCI] Capability length: %d bytes\n", cap_length);
    debug_print("[xHCI] HCI Version: 0x%04x\n", hci_version);
    
    /* Calculate register base addresses */
    xhci_ctrl.op_regs = xhci_ctrl.cap_regs + cap_length;
    
    uint32_t rtsoff = readl(xhci_ctrl.cap_regs + XHCI_CAP_RTSOFF) & ~0x1F;
    uint32_t dboff = readl(xhci_ctrl.cap_regs + XHCI_CAP_DBOFF) & ~0x3;
    
    xhci_ctrl.runtime_regs = xhci_ctrl.cap_regs + rtsoff;
    xhci_ctrl.doorbell_regs = xhci_ctrl.cap_regs + dboff;
    
    debug_print("[xHCI] Operational regs at offset 0x%02x\n", cap_length);
    debug_print("[xHCI] Runtime regs at offset 0x%x\n", rtsoff);
    debug_print("[xHCI] Doorbell regs at offset 0x%x\n", dboff);
    
    /* Read controller parameters */
    uint32_t hcsparams1 = readl(xhci_ctrl.cap_regs + XHCI_CAP_HCSPARAMS1);
    xhci_ctrl.max_slots = (hcsparams1 & 0xFF);
    xhci_ctrl.max_ports = (hcsparams1 >> 24) & 0xFF;
    xhci_ctrl.num_intrs = (hcsparams1 >> 8) & 0x7FF;
    
    debug_print("[xHCI] Max slots: %d\n", xhci_ctrl.max_slots);
    debug_print("[xHCI] Max ports: %d\n", xhci_ctrl.max_ports);
    debug_print("[xHCI] Interrupters: %d\n", xhci_ctrl.num_intrs);
    
    /* Check if controller is running */
    uint32_t usbsts = readl(xhci_ctrl.op_regs + XHCI_OP_USBSTS);
    if (!(usbsts & XHCI_STS_HCH)) {
        debug_print("[xHCI] Controller is running, stopping it first\n");
        
        /* Stop the controller */
        uint32_t usbcmd = readl(xhci_ctrl.op_regs + XHCI_OP_USBCMD);
        usbcmd &= ~XHCI_CMD_RUN;
        writel(usbcmd, xhci_ctrl.op_regs + XHCI_OP_USBCMD);
        
        /* Wait for halt */
        int timeout = 1000;
        while (timeout-- > 0) {
            usbsts = readl(xhci_ctrl.op_regs + XHCI_OP_USBSTS);
            if (usbsts & XHCI_STS_HCH) break;
        }
        
        if (!(usbsts & XHCI_STS_HCH)) {
            debug_print("[xHCI] Failed to stop controller\n");
            return -1;
        }
    }
    
    debug_print("[xHCI] Controller halted\n");
    
    /* Reset the controller */
    debug_print("[xHCI] Resetting controller...\n");
    
    uint32_t usbcmd = readl(xhci_ctrl.op_regs + XHCI_OP_USBCMD);
    usbcmd |= XHCI_CMD_HCRST;
    writel(usbcmd, xhci_ctrl.op_regs + XHCI_OP_USBCMD);
    
    /* Wait for reset to complete */
    int timeout = 1000;
    while (timeout-- > 0) {
        usbcmd = readl(xhci_ctrl.op_regs + XHCI_OP_USBCMD);
        usbsts = readl(xhci_ctrl.op_regs + XHCI_OP_USBSTS);
        
        if (!(usbcmd & XHCI_CMD_HCRST) && !(usbsts & XHCI_STS_CNR)) {
            break;
        }
    }
    
    if (usbcmd & XHCI_CMD_HCRST) {
        debug_print("[xHCI] Reset timeout!\n");
        return -1;
    }
    
    debug_print("[xHCI] Reset complete\n");
    
    /* Set page size (should be 4K) */
    uint32_t pagesize = readl(xhci_ctrl.op_regs + XHCI_OP_PAGESIZE);
    debug_print("[xHCI] Page size: 0x%x (4K)\n", pagesize);
    
    /* Allocate Device Context Base Address Array */
    xhci_ctrl.dcbaa = kcalloc(xhci_ctrl.max_slots + 1, sizeof(uint64_t));
    if (!xhci_ctrl.dcbaa) {
        debug_print("[xHCI] Failed to allocate DCBAA\n");
        return -1;
    }
    
    xhci_ctrl.dcbaa_phys = virt_to_phys(xhci_ctrl.dcbaa);
    
    /* Set DCBAAP register */
    writeq(xhci_ctrl.dcbaa_phys, xhci_ctrl.op_regs + XHCI_OP_DCBAAP);
    
    debug_print("[xHCI] DCBAA allocated at 0x%llx (phys: 0x%llx)\n",
                (uint64_t)xhci_ctrl.dcbaa, xhci_ctrl.dcbaa_phys);
    
    /* TODO: Set up command ring */
    /* TODO: Set up event ring */
    /* TODO: Enable interrupts */
    
    /* For now, just mark as initialized for port scanning */
    xhci_ctrl.initialized = 1;
    
    debug_print("[xHCI] Basic initialization complete\n");
    debug_print("[xHCI] Ready to scan ports\n");
    
    return 0;
}

/**
 * @brief Check if controller is ready
 */
int xhci_is_ready(void)
{
    return xhci_ctrl.initialized;
}

/**
 * @brief Get controller instance
 */
xhci_controller_t *xhci_get_controller(void)
{
    if (!xhci_ctrl.initialized) return NULL;
    return &xhci_ctrl;
}

/**
 * @brief Scan USB ports for devices
 */
int xhci_scan_ports(void)
{
    if (!xhci_ctrl.initialized) {
        debug_print("[xHCI] Controller not initialized\n");
        return -1;
    }
    
    debug_print("[xHCI] Scanning %d ports for devices...\n", xhci_ctrl.max_ports);
    
    int devices_found = 0;
    
    /* Port registers start at op_base + 0x400, 16 bytes per port */
    for (int port = 0; port < xhci_ctrl.max_ports; port++) {
        uint32_t port_offset = 0x400 + (port * 0x10);
        uint32_t portsc = readl(xhci_ctrl.op_regs + port_offset + XHCI_PORT_PORTSC);
        
        /* Check if device is connected */
        if (portsc & XHCI_PORTSC_CCS) {
            int speed = (portsc & XHCI_PORTSC_SPEED_MASK) >> 10;
            const char *speed_str = "Unknown";
            
            switch (speed) {
                case XHCI_SPEED_FULL: speed_str = "Full-Speed (12 Mbps)"; break;
                case XHCI_SPEED_LOW: speed_str = "Low-Speed (1.5 Mbps)"; break;
                case XHCI_SPEED_HIGH: speed_str = "High-Speed (480 Mbps)"; break;
                case XHCI_SPEED_SUPER: speed_str = "SuperSpeed (5 Gbps)"; break;
            }
            
            debug_print("[xHCI] Port %d: Device connected (%s)\n", 
                       port + 1, speed_str);
            
            devices_found++;
            
            /* TODO: Enumerate device */
        }
    }
    
    if (devices_found == 0) {
        debug_print("[xHCI] No devices found\n");
    } else {
        debug_print("[xHCI] Found %d device(s)\n", devices_found);
    }
    
    return devices_found;
}
