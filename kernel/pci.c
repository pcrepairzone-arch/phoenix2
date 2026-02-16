/*
 * pci.c – PCI Bus Driver for RISC OS Phoenix
 * Scans PCI bus, registers devices, maps BARs
 * Author: R Andrews Grok 4 – 10 Dec 2025
 */

#include "kernel.h"
#include "pci.h"
#include <stdint.h>

#define PCI_CONFIG_ADDRESS  0xCF8
#define PCI_CONFIG_DATA     0xCFC

#define PCI_MAX_BUS         256
#define PCI_MAX_DEV         32
#define PCI_MAX_FUNC        8

static pci_driver_t *pci_drivers[32];
static int num_drivers = 0;
static spinlock_t pci_lock = SPINLOCK_INIT;

/* Read PCI config dword */
static uint32_t pci_config_read(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset) {
    uint32_t addr = (1ULL << 31) | (bus << 16) | (dev << 11) | (func << 8) | (offset & 0xFC);
    outl(PCI_CONFIG_ADDRESS, addr);
    return inl(PCI_CONFIG_DATA);
}

/* Write PCI config dword */
static void pci_config_write(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset, uint32_t val) {
    uint32_t addr = (1ULL << 31) | (bus << 16) | (dev << 11) | (func << 8) | (offset & 0xFC);
    outl(PCI_CONFIG_ADDRESS, addr);
    outl(PCI_CONFIG_DATA, val);
}

/* Scan PCI bus and probe drivers */
void pci_scan_bus(void) {
    for (int bus = 0; bus < PCI_MAX_BUS; bus++) {
        for (int dev = 0; dev < PCI_MAX_DEV; dev++) {
            for (int func = 0; func < PCI_MAX_FUNC; func++) {
                uint32_t vendor = pci_config_read(bus, dev, func, 0);
                if ((vendor & 0xFFFF) == 0xFFFF) continue;

                pci_dev_t pdev;
                pdev.vendor_id = vendor & 0xFFFF;
                pdev.device_id = vendor >> 16;
                pdev.class_code = pci_config_read(bus, dev, func, 0x08) >> 8;

                for (int bar = 0; bar < PCI_BAR_COUNT; bar++) {
                    pdev.bar[bar] = pci_config_read(bus, dev, func, 0x10 + bar * 4);
                }

                pdev.irq_line = pci_config_read(bus, dev, func, 0x3C) & 0xFF;

                // Probe drivers
                for (int i = 0; i < num_drivers; i++) {
                    pci_driver_t *drv = pci_drivers[i];
                    if (drv->class_code == 0xFFFFFF || drv->class_code == pdev.class_code) {
                        if (drv->probe(&pdev) == 0) {
                            debug_print("PCI: Probed %s (VID:0x%04x DID:0x%04x)\n", drv->name, pdev.vendor_id, pdev.device_id);
                        }
                    }
                }
            }
        }
    }
}

/* Register PCI driver */
void pci_register_driver(pci_driver_t *driver) {
    unsigned long flags;
    spin_lock_irqsave(&pci_lock, &flags);

    if (num_drivers < 32) {
        pci_drivers[num_drivers++] = driver;
    }

    spin_unlock_irqrestore(&pci_lock, flags);
}

/* Enable bus mastering for DMA */
void pci_enable_busmaster(pci_dev_t *dev) {
    uint32_t cmd = pci_config_read(dev->bus, dev->dev, dev->func, 0x04);
    pci_config_write(dev->bus, dev->dev, dev->func, 0x04, cmd | (1 << 2));  // Bus master bit
}

/* Get BAR start address */
uint64_t pci_bar_start(pci_dev_t *dev, int bar) {
    return dev->bar[bar] & ~0xF;  // Mask flags
}

/* Module init – scan bus */
_kernel_oserror *module_init(const char *arg, int podule)
{
    pci_scan_bus();
    debug_print("PCI bus scanned\n");
    return NULL;
}