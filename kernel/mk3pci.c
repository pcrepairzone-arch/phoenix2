/*
 * pci.c - PCI/PCIe Bus Driver for Phoenix RISC OS
 * Adapted for Raspberry Pi 4 PCIe (MMIO-based)
 */

#include "kernel.h"
#include "pci.h"

/* External UART functions */
extern void uart_puts(const char *s);
extern void uart_putc(char c);

/* Helper to print hex values */
static void print_hex32(uint32_t val)
{
    uart_puts("0x");
    for (int i = 28; i >= 0; i -= 4) {
        int nibble = (val >> i) & 0xF;
        uart_putc(nibble < 10 ? '0' + nibble : 'a' + (nibble - 10));
    }
}

static void print_hex8(uint8_t val)
{
    for (int i = 4; i >= 0; i -= 4) {
        int nibble = (val >> i) & 0xF;
        uart_putc(nibble < 10 ? '0' + nibble : 'a' + (nibble - 10));
    }
}

/* Pi 4 PCIe Controller Base */
#define PCIE_RC_BASE        0xFD500000ULL
#define PCIE_EXT_CFG_INDEX  (PCIE_RC_BASE + 0x9000)
#define PCIE_EXT_CFG_DATA   (PCIE_RC_BASE + 0x9004)
#define RGR1_SW_INIT_1      (PCIE_RC_BASE + 0x9210)
#define MISC_PCIE_STATUS    (PCIE_RC_BASE + 0x4068)

static pci_driver_t *pci_drivers[32];
static int num_drivers = 0;

/* Read PCI config space - WINDOWED METHOD */
uint32_t pci_read_config(pci_dev_t *dev, uint32_t offset)
{
    uint32_t idx = ((uint32_t)dev->bus << 20) |
                   ((uint32_t)dev->dev << 15) |
                   ((uint32_t)dev->func << 12) |
                   (offset & ~0xFFFU);
    
    /* Write index to select config window */
    writel(idx, (void *)PCIE_EXT_CFG_INDEX);
    
    /* Read from data window */
    return readl((void *)(PCIE_EXT_CFG_DATA + (offset & 0xFFC)));
}

/* Write PCI config space */
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
    uart_puts("[PCI] Initializing PCIe hardware...\n");

    /* Read and print initial reset state */
    uint32_t reg = readl((void *)RGR1_SW_INIT_1);
    uart_puts("[PCI] Reset register = ");
    print_hex32(reg);
    uart_puts("\n");

    /* Deassert resets */
    reg &= ~0x3;
    writel(reg, (void *)RGR1_SW_INIT_1);

    /* Wait for link training */
    for (volatile int i = 0; i < 50000000; i++) { }

    /* Check link status */
    reg = readl((void *)MISC_PCIE_STATUS);
    uart_puts("[PCI] Link status = ");
    print_hex32(reg);
    uart_puts("\n");

    return 0;
}

/* Scan PCI bus - ONLY scan bus 1 device 0 for now! */
static void pci_scan_bus(void)
{
    uart_puts("[PCI] Scanning for VL805 at bus 1, device 0...\n");
    
    pci_dev_t pdev;
    pdev.bus = 1;
    pdev.dev = 0;
    pdev.func = 0;
    
    /* Read vendor/device ID */
    uint32_t vendor_dev = pci_read_config(&pdev, 0);
    
    uart_puts("[PCI] Read from bus 1 dev 0: ");
    print_hex32(vendor_dev);
    uart_puts("\n");
    
    if (vendor_dev == 0 || vendor_dev == 0xFFFFFFFF) {
        uart_puts("[PCI] No device found at 01:00.0\n");
        return;
    }
    
    uart_puts("[PCI] DEVICE FOUND!\n");
    
    pdev.vendor_id = vendor_dev & 0xFFFF;
    pdev.device_id = vendor_dev >> 16;
    
    uart_puts("[PCI]   Vendor: 0x");
    print_hex8(pdev.vendor_id);
    uart_puts(" Device: 0x");
    print_hex8(pdev.device_id);
    uart_puts("\n");
    
    /* Check if it's VL805 */
    if (pdev.vendor_id == 0x1106 && pdev.device_id == 0x3483) {
        uart_puts("[USB] *** VL805 DETECTED! ***\n");
    }
}

/* Initialize PCI subsystem */
void pci_init(void)
{
    uart_puts("[PCI] Initializing PCI subsystem\n");
    
    pci_hw_init();
    pci_scan_bus();
    
    uart_puts("[PCI] PCI subsystem ready\n");
}

/* Register PCI driver */
void pci_register_driver(pci_driver_t *driver)
{
    if (num_drivers < 32) {
        pci_drivers[num_drivers++] = driver;
        uart_puts("[PCI] Registered driver: ");
        uart_puts(driver->name);
        uart_puts("\n");
    }
}

/* Enable bus mastering for DMA */
void pci_enable_busmaster(pci_dev_t *dev)
{
    uint32_t cmd = pci_read_config(dev, 0x04);
    cmd |= (1 << 2) | (1 << 1);
    pci_write_config(dev, 0x04, cmd);
}

/* Get BAR physical address */
uint64_t pci_bar_start(pci_dev_t *dev, int bar)
{
    if (bar < 0 || bar >= PCI_BAR_COUNT) return 0;
    
    uint64_t addr = dev->bar[bar];
    if (addr & 1) return 0;
    
    if ((addr & 0x6) == 0x4) {
        if (bar + 1 < PCI_BAR_COUNT) {
            addr = (addr & ~0xFULL) | ((uint64_t)dev->bar[bar + 1] << 32);
        }
    } else {
        addr &= ~0xFULL;
    }
    
    return addr;
}
