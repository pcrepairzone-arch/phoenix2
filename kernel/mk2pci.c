/*
 * pci.c - PCI/PCIe Bus Driver for Phoenix RISC OS
 * Adapted for Raspberry Pi 4 PCIe (MMIO-based)
 */

#include "kernel.h"
#include "pci.h"

/* External UART functions */
extern void uart_puts(const char *s);
extern void uart_putc(char c);

/* Pi 4 PCIe Controller Base */
#define PCIE_RC_BASE        0xFD500000ULL
#define PCIE_EXT_CFG_INDEX  (PCIE_RC_BASE + 0x9000)
#define PCIE_EXT_CFG_DATA   (PCIE_RC_BASE + 0x9004)
#define RGR1_SW_INIT_1      (PCIE_RC_BASE + 0x9210)
#define MISC_PCIE_STATUS    (PCIE_RC_BASE + 0x4068)

#define PCIE_LINK_UP_BIT    0x1

static pci_driver_t *pci_drivers[32];
static int num_drivers = 0;

/* Read PCI config space - CORRECTED */
uint32_t pci_read_config(pci_dev_t *dev, uint32_t offset)
{
    uint32_t idx = ((uint32_t)dev->bus << 20) |
                   ((uint32_t)dev->dev << 15) |
                   ((uint32_t)dev->func << 12) |
                   (offset & ~0xFFFU);
    
    /* Write index to select config window */
    writel(idx, (void *)PCIE_EXT_CFG_INDEX);
    
    /* Read from data window - use 0xFFC for dword alignment */
    return readl((void *)(PCIE_EXT_CFG_DATA + (offset & 0xFFC)));
}

/* Write PCI config space - CORRECTED */
void pci_write_config(pci_dev_t *dev, uint32_t offset, uint32_t value)
{
    uint32_t idx = ((uint32_t)dev->bus << 20) |
                   ((uint32_t)dev->dev << 15) |
                   ((uint32_t)dev->func << 12) |
                   (offset & ~0xFFFU);
    
    writel(idx, (void *)PCIE_EXT_CFG_INDEX);
    writel(value, (void *)(PCIE_EXT_CFG_DATA + (offset & 0xFFC)));
}

/* Initialize PCIe controller hardware */
static int pci_hw_init(void)
{
    uint32_t reg;

    debug_print("[PCI] Initializing PCIe hardware...\n");

    /* Deassert resets */
    reg = readl((void *)RGR1_SW_INIT_1);
    reg &= ~0x3;  /* Bit 0: core init, Bit 1: PERST# */
    writel(reg, (void *)RGR1_SW_INIT_1);

    /* Wait for link training (~500ms) */
    for (volatile int i = 0; i < 50000000; i++) { }

    /* Check link status */
    reg = readl((void *)MISC_PCIE_STATUS);
    debug_print("[PCI] Link status register: 0x%08x\n", reg);
    
    if (!(reg & PCIE_LINK_UP_BIT)) {
        debug_print("[PCI] PCIe link not up\n");
        /* Continue anyway - firmware may have set it up */
    } else {
        debug_print("[PCI] PCIe link up successfully\n");
    }

    return 0;
}

/* Scan PCI bus */
static void pci_scan_bus(void)
{
    debug_print("[PCI] Scanning PCIe bus...\n");
    
    /* Scan bus 0 (root) and bus 1 (VL805) */
    for (int bus = 0; bus < 2; bus++) {
        for (int dev = 0; dev < 32; dev++) {
            for (int func = 0; func < 8; func++) {
                pci_dev_t pdev;
                pdev.bus = bus;
                pdev.dev = dev;
                pdev.func = func;
                
                uint32_t vendor = pci_read_config(&pdev, 0);
                if (vendor == 0 || vendor == 0xFFFFFFFF) continue;
                
                debug_print("[PCI] Found device %02x:%02x.%x\n", bus, dev, func);
                
                pdev.vendor_id = vendor & 0xFFFF;
                pdev.device_id = vendor >> 16;
                
                uint32_t class_rev = pci_read_config(&pdev, 0x08);
                pdev.class_code = class_rev >> 8;
                
                /* Read BARs */
                for (int bar = 0; bar < PCI_BAR_COUNT; bar++) {
                    pdev.bar[bar] = pci_read_config(&pdev, 0x10 + bar * 4);
                }
                
                pdev.irq_line = pci_read_config(&pdev, 0x3C) & 0xFF;
                
                debug_print("[PCI]   Vendor: 0x%04x Device: 0x%04x\n", 
                            pdev.vendor_id, pdev.device_id);
                debug_print("[PCI]   Class: 0x%06x\n", pdev.class_code);
                
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
            }
        }
    }
    
    debug_print("[PCI] Scan complete\n");
}

/* Initialize PCI subsystem */
void pci_init(void)
{
    debug_print("[PCI] Initializing PCI subsystem\n");
    
    if (pci_hw_init() != 0) {
        return;
    }
    
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
    cmd |= (1 << 2) | (1 << 1);  /* Bus master + memory space */
    pci_write_config(dev, 0x04, cmd);
    
    debug_print("[PCI] Bus master enabled for %02x:%02x.%x\n",
               dev->bus, dev->dev, dev->func);
}

/* Get BAR physical address */
uint64_t pci_bar_start(pci_dev_t *dev, int bar)
{
    if (bar < 0 || bar >= PCI_BAR_COUNT) return 0;
    
    uint64_t addr = dev->bar[bar];
    if (addr & 1) return 0;  /* Skip I/O BARs */
    
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
