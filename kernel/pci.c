/*
 * pci.c - PCI/PCIe Bus Driver for Phoenix RISC OS
 * Adapted for Raspberry Pi 4 PCIe (MMIO-based)
 */

#include "kernel.h"
#include "pci.h"

/* Pi 4 PCIe Controller Base */
#define PCIE_BASE       0xFD500000ULL
#define PCIE_CONFIG     (PCIE_BASE + 0x00)
#define PCIE_EXT_CFG    (PCIE_BASE + 0x4000)

static pci_driver_t *pci_drivers[32];
static int num_drivers = 0;

/* Read PCI config space (Pi 4 MMIO method) */
uint32_t pci_read_config(pci_dev_t *dev, uint32_t offset)
{
    /* Pi 4 PCIe uses MMIO config space */
    uint64_t addr = PCIE_EXT_CFG + 
                    ((dev->bus << 20) | (dev->dev << 15) | 
                     (dev->func << 12) | (offset & 0xFFF));
    
    return readl((void *)addr);
}

/* Write PCI config space */
void pci_write_config(pci_dev_t *dev, uint32_t offset, uint32_t value)
{
    uint64_t addr = PCIE_EXT_CFG + 
                    ((dev->bus << 20) | (dev->dev << 15) | 
                     (dev->func << 12) | (offset & 0xFFF));
    
    writel(value, (void *)addr);
}

/* Initialize PCIe bridge for Pi 4 */
static void pcie_bridge_init(void)
{
    debug_print("[PCI] Initializing Pi 4 PCIe bridge\n");
    
    /* Enable PCIe controller */
    /* TODO: Set up bridge registers properly */
    
    debug_print("[PCI] PCIe bridge ready\n");
}

/* Scan PCI bus */
static void pci_scan_bus(void)
{
    debug_print("[PCI] Scanning PCIe bus...\n");
    
    /* Pi 4 VL805 is at bus 1, device 0, function 0 */
    /* Scan only this specific location */
    
    int bus = 1;
    int dev = 0;
    int func = 0;
    
    pci_dev_t pdev;
    pdev.bus = bus;
    pdev.dev = dev;
    pdev.func = func;
    
    uint32_t vendor = pci_read_config(&pdev, 0);
    
    debug_print("[PCI] Read vendor at 01:00.0 = 0x%08x\n", vendor);
    
    if ((vendor & 0xFFFF) == 0xFFFF || vendor == 0) {
        debug_print("[PCI] No device at 01:00.0\n");
        return;
    }
    
    pdev.vendor_id = vendor & 0xFFFF;
    pdev.device_id = vendor >> 16;
    
    uint32_t class_rev = pci_read_config(&pdev, 0x08);
    pdev.class_code = class_rev >> 16;
    
    /* Read BARs */
    for (int bar = 0; bar < PCI_BAR_COUNT; bar++) {
        pdev.bar[bar] = pci_read_config(&pdev, 0x10 + bar * 4);
    }
    
    pdev.irq_line = pci_read_config(&pdev, 0x3C) & 0xFF;
    
    debug_print("[PCI] Found device %02x:%02x.%x\n", bus, dev, func);
    debug_print("[PCI]   Vendor: 0x%04x Device: 0x%04x\n", 
                pdev.vendor_id, pdev.device_id);
    debug_print("[PCI]   Class: 0x%04x\n", pdev.class_code);
    
    /* Probe drivers */
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
    
    debug_print("[PCI] Scan complete\n");
}

/* Initialize PCI subsystem */
void pci_init(void)
{
    debug_print("[PCI] Initializing PCI subsystem\n");
    
    pcie_bridge_init();
    pci_scan_bus();
    
    debug_print("[PCI] PCI subsystem ready\n");
}

/* Register PCI driver */
void pci_register_driver(pci_driver_t *driver)
{
    if (num_drivers < 32) {
        pci_drivers[num_drivers++] = driver;
        debug_print("[PCI] Registered driver: %s\n", driver->name);
    }
}

/* Enable bus mastering for DMA */
void pci_enable_busmaster(pci_dev_t *dev)
{
    uint32_t cmd = pci_read_config(dev, 0x04);
    cmd |= (1 << 2);  /* Bus master enable */
    pci_write_config(dev, 0x04, cmd);
    
    debug_print("[PCI] Bus master enabled for %02x:%02x.%x\n",
               dev->bus, dev->dev, dev->func);
}

/* Get BAR physical address */
uint64_t pci_bar_start(pci_dev_t *dev, int bar)
{
    if (bar < 0 || bar >= PCI_BAR_COUNT) return 0;
    
    uint64_t addr = dev->bar[bar];
    
    /* Check if 64-bit BAR */
    if ((addr & 0x6) == 0x4) {
        /* 64-bit BAR - combine with next BAR */
        if (bar + 1 < PCI_BAR_COUNT) {
            addr = (addr & ~0xFULL) | ((uint64_t)dev->bar[bar + 1] << 32);
        }
    } else {
        /* 32-bit BAR */
        addr &= ~0xFULL;
    }
    
    return addr;
}
