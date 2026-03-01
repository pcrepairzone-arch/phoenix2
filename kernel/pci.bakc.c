
 /*
 * pci.c - PCI/PCIe Bus Driver for Phoenix RISC OS
 * Adapted for Raspberry Pi 4 PCIe (indirect access)
 */

#include "kernel.h"
#include "pci.h"

/* Pi 4 PCIe Controller Base */
#define PCIE_PHY_LINKUP_BIT (1 << 4)  // Bit 4: PHY_LINKUP_MASK
#define PCIE_DL_ACTIVE_BIT  (1 << 5)  // Bit 5: DL_ACTIVE_MASK
#define PCIE_HARD_DEBUG_OFFSET 0x4204  // PCIE_MISC_HARD_PCIE_HARD_DEBUG
#define PCIE_SERDES_IDDQ_MASK (1 << 28)  // SERDES_IDDQ bit
#define PCIE_SSC_CNTL_OFFSET 0x2  // For MDIO SSC
#define PCIE_MDIO_PORT0 0  // MDIO port

/* MDIO helpers (from driver; implement if not in mmio.c) */
static int brcm_pcie_mdio_write(void *base, int port, int offset, u32 val) {
    // Implement MDIO write (driver uses mdiobus)
    // Stub for now: writel(val, base + offset); // Adjust for actual MDIO regs
    return 0;
}
static int brcm_pcie_mdio_read(void *base, int port, int offset, u32 *val) {
    // Stub: *val = readl(base + offset);
    return 0;
}

/* Initialize PCIe hardware */
int pcie_bridge_init(void) {
    debug_print("[PCI] Initializing Pi 4 PCIe bridge\n");
    
    uint32_t reg = readl(pcie_base + RGR1_SW_INIT_1);
    debug_print("[PCI] Initial reset reg: 0x%08x\n", reg);
    
    // Assert bridge reset (bit 0 = 1)
    reg |= 0x1;
    writel(reg, pcie_base + RGR1_SW_INIT_1);
    usleep(100);  // Use timer or NOP for delay
    
    // Assert PERST# (bit 1 = 1 for BCM2711)
    reg |= 0x2;
    writel(reg, pcie_base + RGR1_SW_INIT_1);
    usleep(100);
    
    // Deassert bridge reset (bit 0 = 0)
    reg &= ~0x1;
    writel(reg, pcie_base + RGR1_SW_INIT_1);
    usleep(100);
    
    // Confirm resets
    reg = readl(pcie_base + RGR1_SW_INIT_1);
    debug_print("[PCI] Reset reg after bridge deassert: 0x%08x\n", reg);
    if (reg & 0x1) debug_print("[PCI] WARNING: Bridge deassert failed!\n");
    
    // Disable SerDes IDDQ
    reg = readl(pcie_base + PCIE_HARD_DEBUG_OFFSET);
    reg &= ~PCIE_SERDES_IDDQ_MASK;
    writel(reg, pcie_base + PCIE_HARD_DEBUG_OFFSET);
    usleep(100);
    
    // Enable SSC (spread spectrum clocking)
    brcm_pcie_mdio_write(pcie_base, PCIE_MDIO_PORT0, 0x0, 0x1d1);  // Set addr to SSC regs
    brcm_pcie_mdio_read(pcie_base, PCIE_MDIO_PORT0, PCIE_SSC_CNTL_OFFSET, &reg);
    reg |= (1 << 15) | (1 << 14);  // OVRD_EN and OVRD_VAL for SSC
    brcm_pcie_mdio_write(pcie_base, PCIE_MDIO_PORT0, PCIE_SSC_CNTL_OFFSET, reg);
    usleep(1000);
    // Check SSC status (optional poll)
    
    // Deassert PERST# (bit 1 = 0)
    reg &= ~0x2;
    writel(reg, pcie_base + RGR1_SW_INIT_1);
    debug_print("[PCI] Reset reg after PERST deassert: 0x%08x\n", reg);
    if (reg & 0x2) debug_print("[PCI] WARNING: PERST deassert failed!\n");
    
    // Wait 100ms per PCIe spec
    usleep(100000);
    
    // Poll for link up (5ms intervals, up to 100ms)
    int timeout = 20;  // 20 * 5ms = 100ms
    while (timeout--) {
        reg = readl(pcie_base + MISC_PCIE_STATUS);
        bool phy_up = (reg & PCIE_PHY_LINKUP_BIT);
        bool dl_up = (reg & PCIE_DL_ACTIVE_BIT);
        debug_print("[PCI] Link poll: 0x%08x (PHY:%d DL:%d)\n", reg, phy_up, dl_up);
        if (phy_up && dl_up) {
            debug_print("[PCI] PCIe link up!\n");
            return 0;
        }
        usleep(5000);  // 5ms
    }
    
    debug_print("[PCI] ERROR: Link timeout!\n");
    return -1;
}

/* Scan PCI bus */
static void pci_scan_bus(void) {
    debug_print("[PCI] Scanning PCIe bus...\n");
    
    // Scan bus 0-1, dev 0-31, func 0-7 (Pi 4 is simple x1)
    for (int bus = 0; bus < 2; bus++) {
        for (int dev = 0; dev < 32; dev++) {
            for (int func = 0; func < 8; func++) {
                pci_dev_t pdev = { .bus = bus, .dev = dev, .func = func };
                uint32_t vendor = pci_read_config(&pdev, 0);
                
                if (vendor == 0xFFFFFFFF || vendor == 0) continue;  // Invalid
                
                pdev.vendor_id = vendor & 0xFFFF;
                pdev.device_id = vendor >> 16;
                
                uint32_t class_rev = pci_read_config(&pdev, 0x08);
                pdev.class_code = class_rev >> 8;  // Full 24-bit
                
                // Read BARs...
                for (int bar = 0; bar < PCI_BAR_COUNT; bar++) {
                    pdev.bar[bar] = pci_read_config(&pdev, 0x10 + bar * 4);
                }
                pdev.irq_line = pci_read_config(&pdev, 0x3C) & 0xFF;
                
                debug_print("[PCI] Found %02x:%02x.%x Vendor:0x%04x Device:0x%04x Class:0x%06x\n",
                            bus, dev, func, pdev.vendor_id, pdev.device_id, pdev.class_code);
                
                // Probe drivers
	for (int i = 0; i < num_drivers; i++) {
    	pci_driver_t *drv = pci_drivers[i];
    	int match = 0;
    	if (drv->vendor_id == 0xFFFF || drv->vendor_id == pdev.vendor_id) {
        if (drv->device_id == 0xFFFF || drv->device_id == pdev.device_id) {
            if (drv->class_code == 0xFFFFFF || drv->class_code == pdev.class_code) {
                match = 1;
            }
        }
    }
    if (match && drv->probe) {
        if (drv->probe(&pdev) == 0) {
            debug_print("[PCI] %s claimed device\n", drv->name);
        }
    }
}
                
                // Special: If VL805, init USB
                if (pdev.vendor_id == 0x1106 && pdev.device_id == 0x3483) {
                    debug_print("[PCI] VL805 found - calling vl805_init()\n");
                    vl805_init();
                }
            }
        }
    }
    debug_print("[PCI] Scan complete\n");
}

/* Initialize PCI subsystem */
void pci_init(void) {
    debug_print("[PCI] Initializing PCI subsystem\n");
    
    if (pci_map_registers() != 0) return;
    if (pcie_bridge_init() != 0) {
        iounmap(pcie_base);
        return;
    }
    pci_scan_bus();
    debug_print("[PCI] PCI subsystem ready\n");
}
void pci_register_driver(pci_driver_t *driver) {
    if (num_drivers < 32) {
        pci_drivers[num_drivers++] = driver;
        debug_print("[PCI] Registered driver: %s\n", driver->name);
    }
}

void pci_enable_busmaster(pci_dev_t *dev) {
    uint32_t cmd = pci_read_config(dev, 0x04);
    cmd |= (1 << 2) | (1 << 1);  // Bus master + memory space
    pci_write_config(dev, 0x04, cmd);
    debug_print("[PCI] Bus master and memory enabled for %02x:%02x.%x\n", dev->bus, dev->dev, dev->func);
}

uint64_t pci_bar_start(pci_dev_t *dev, int bar) {
    if (bar < 0 || bar >= PCI_BAR_COUNT) return 0;
    uint64_t addr = dev->bar[bar];
    if (addr & 1) return 0;  // Skip I/O BARs
    if ((addr & 0x6) == 0x4 && bar + 1 < PCI_BAR_COUNT) {
        addr = (addr & ~0xFULL) | ((uint64_t)dev->bar[bar + 1] << 32);
    } else {
        addr &= ~0xFULL;
    }
    return addr;
}
// Rest of file (register_driver, enable_busmaster, bar_start) unchanged