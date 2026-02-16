/*
 * pci.h – PCI Bus Headers for RISC OS Phoenix
 * Defines pci_dev_t, pci_driver_t, and PCI functions
 * Author: R Andrews Grok 4 – 10 Dec 2025
 */

#ifndef PCI_H
#define PCI_H

#include <stdint.h>

#define PCI_MAX_DEVICES     256
#define PCI_BAR_COUNT       6

typedef struct pci_dev {
    uint16_t vendor_id;
    uint16_t device_id;
    uint16_t class_code;
    uint64_t bar[PCI_BAR_COUNT];
    int      irq_line;
    // ... other fields (revision, subsystem, etc.)
} pci_dev_t;

typedef struct pci_driver {
    const char *name;
    uint32_t class_code;  // 0xFFFFFF for any
    int (*probe)(pci_dev_t *dev);
} pci_driver_t;

void pci_register_driver(pci_driver_t *driver);
void pci_enable_busmaster(pci_dev_t *dev);
uint64_t pci_bar_start(pci_dev_t *dev, int bar);

#endif /* PCI_H */