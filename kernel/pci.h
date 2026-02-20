/*
 * pci.h - PCI/PCIe Bus Headers for Phoenix RISC OS
 * Adapted for Raspberry Pi 4 PCIe
 */

#ifndef KERNEL_PCI_H
#define KERNEL_PCI_H

#include <stdint.h>

#define PCI_MAX_DEVICES     256
#define PCI_BAR_COUNT       6

typedef struct pci_dev {
    uint8_t  bus;
    uint8_t  dev;
    uint8_t  func;
    uint16_t vendor_id;
    uint16_t device_id;
    uint16_t class_code;
    uint64_t bar[PCI_BAR_COUNT];
    int      irq_line;
} pci_dev_t;

typedef struct pci_driver {
    const char *name;
    uint16_t vendor_id;  /* 0xFFFF for any */
    uint16_t device_id;  /* 0xFFFF for any */
    uint32_t class_code; /* 0xFFFFFF for any */
    int (*probe)(pci_dev_t *dev);
} pci_driver_t;

/* PCI Functions */
void pci_init(void);
void pci_register_driver(pci_driver_t *driver);
void pci_enable_busmaster(pci_dev_t *dev);
uint64_t pci_bar_start(pci_dev_t *dev, int bar);
uint32_t pci_read_config(pci_dev_t *dev, uint32_t offset);
void pci_write_config(pci_dev_t *dev, uint32_t offset, uint32_t value);

#endif /* KERNEL_PCI_H */
