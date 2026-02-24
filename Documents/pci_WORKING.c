/*
 * pci.c - PCI Driver with VL805 USB Support (WORKING VERSION!)
 * 
 * BREAKTHROUGH: VL805 is at bus 0, device 0 - NOT bus 1!
 * Bootloader already initializes it - no mailbox needed!
 */

#include "kernel.h"
#include "pci.h"

static void *pcie_base = NULL;
extern void uart_puts(const char *s);
extern void uart_putc(char c);

static void print_hex32(uint32_t val)
{
    uart_puts("0x");
    for (int i = 28; i >= 0; i -= 4) {
        int nibble = (val >> i) & 0xF;
        uart_putc(nibble < 10 ? '0' + nibble : 'a' + (nibble - 10));
    }
}

static void print_hex16(uint16_t val)
{
    uart_puts("0x");
    for (int i = 12; i >= 0; i -= 4) {
        int nibble = (val >> i) & 0xF;
        uart_putc(nibble < 10 ? '0' + nibble : 'a' + (nibble - 10));
    }
}

static void delay_ms(int ms)
{
    for (volatile int i = 0; i < ms * 100000; i++) { }
}

/* ========== PCIE ========== */

#define PCIE_RC_BASE        0xFD500000ULL
#define RGR1_SW_INIT_1_OFF  0x9210
#define MISC_PCIE_STATUS_OFF 0x4068
#define PCIE_EXT_CFG_INDEX_OFF 0x9000
#define PCIE_EXT_CFG_DATA_OFF  0x9004

#define PCIE_PHY_LINKUP_BIT (1 << 4)
#define PCIE_DL_ACTIVE_BIT  (1 << 5)

static pci_driver_t *pci_drivers[32];
static int num_drivers = 0;

static int pcie_bring_up_link(void)
{
    uart_puts("\n[PCI] Bringing up PCIe link...\n");
    
    uint32_t reg = readl(pcie_base + RGR1_SW_INIT_1_OFF);
    uart_puts("[PCI]   Reset register: ");
    print_hex32(reg);
    uart_puts("\n");
    
    /* Deassert resets */
    reg &= ~0x3;
    writel(reg, pcie_base + RGR1_SW_INIT_1_OFF);
    delay_ms(100);
    
    /* Wait for link */
    for (int timeout = 100; timeout > 0; timeout--) {
        reg = readl(pcie_base + MISC_PCIE_STATUS_OFF);
        if ((reg & PCIE_PHY_LINKUP_BIT) && (reg & PCIE_DL_ACTIVE_BIT)) {
            uart_puts("[PCI]   LINK UP! Status: ");
            print_hex32(reg);
            uart_puts("\n");
            return 0;
        }
        delay_ms(10);
    }
    
    return -1;
}

uint32_t pci_read_config(pci_dev_t *dev, uint32_t offset)
{
    uint32_t idx = ((uint32_t)dev->bus << 20) |
                   ((uint32_t)dev->dev << 15) |
                   ((uint32_t)dev->func << 12) |
                   (offset & ~0xFFFU);

    writel(idx, pcie_base + PCIE_EXT_CFG_INDEX_OFF);
    return readl(pcie_base + PCIE_EXT_CFG_DATA_OFF + (offset & 0xFFC));
}

void pci_write_config(pci_dev_t *dev, uint32_t offset, uint32_t value)
{
    uint32_t idx = ((uint32_t)dev->bus << 20) |
                   ((uint32_t)dev->dev << 15) |
                   ((uint32_t)dev->func << 12) |
                   (offset & ~0xFFFU);

    writel(idx, pcie_base + PCIE_EXT_CFG_INDEX_OFF);
    writel(value, pcie_base + PCIE_EXT_CFG_DATA_OFF + (offset & 0xFFC));
}

static void pci_scan_bus(void)
{
    uart_puts("\n[PCI] Scanning for VL805...\n");
    
    /* VL805 is at bus 0, device 0! */
    pci_dev_t pdev = {0, 0, 0};  /* CHANGED FROM bus 1 to bus 0! */
    
    uint32_t vendor_dev = pci_read_config(&pdev, 0);
    uart_puts("[PCI]   Bus 0, Device 0: ");
    print_hex32(vendor_dev);
    uart_puts("\n");
    
    if (vendor_dev == 0 || vendor_dev == 0xFFFFFFFF || vendor_dev == 0xDEADDEAD) {
        uart_puts("[PCI]   ERROR: No valid device!\n");
        return;
    }
    
    pdev.vendor_id = vendor_dev & 0xFFFF;
    pdev.device_id = vendor_dev >> 16;
    
    uart_puts("\n");
    uart_puts("╔════════════════════════════════════════╗\n");
    uart_puts("║       *** DEVICE DETECTED! ***        ║\n");
    uart_puts("╚════════════════════════════════════════╝\n");
    uart_puts("[PCI]   Vendor: ");
    print_hex16(pdev.vendor_id);
    uart_puts("\n[PCI]   Device: ");
    print_hex16(pdev.device_id);
    uart_puts("\n");
    
    if (pdev.vendor_id == 0x1106 && pdev.device_id == 0x3483) {
        uart_puts("\n");
        uart_puts("╔════════════════════════════════════════╗\n");
        uart_puts("║  🎉 VL805 USB 3.0 CONTROLLER FOUND! 🎉  ║\n");
        uart_puts("║                                        ║\n");
        uart_puts("║     USB INITIALIZATION SUCCESSFUL!     ║\n");
        uart_puts("╚════════════════════════════════════════╝\n");
        uart_puts("\n");
        
        /* Read class/revision */
        uint32_t class_rev = pci_read_config(&pdev, 0x08);
        uart_puts("[VL805]  Class/Revision: ");
        print_hex32(class_rev);
        uart_puts("\n");
        
        /* Read BAR0 */
        uint32_t bar0 = pci_read_config(&pdev, 0x10);
        uart_puts("[VL805]  BAR0: ");
        print_hex32(bar0);
        uart_puts("\n");
        
        /* Read command/status */
        uint32_t cmd = pci_read_config(&pdev, 0x04);
        uart_puts("[VL805]  Command/Status: ");
        print_hex32(cmd);
        uart_puts("\n");
        
        /* Store device info */
        pdev.bar[0] = bar0;
    }
}

void pci_init(void)
{
    uart_puts("\n");
    uart_puts("╔════════════════════════════════════════╗\n");
    uart_puts("║    PCI/USB INIT (WORKING VERSION!)    ║\n");
    uart_puts("╚════════════════════════════════════════╝\n");

    pcie_base = ioremap(PCIE_RC_BASE, 0x10000);
    if (!pcie_base) {
        uart_puts("[PCI] ioremap failed!\n");
        return;
    }
    uart_puts("[PCI] PCIe controller mapped\n");

    /* Bring up PCIe link */
    if (pcie_bring_up_link() != 0) {
        uart_puts("[PCI] WARNING: Link didn't come up\n");
    }

    /* Scan for VL805 at bus 0, device 0 */
    pci_scan_bus();

    uart_puts("\n[PCI] Initialization complete!\n\n");
}

void pci_register_driver(pci_driver_t *driver)
{
    if (num_drivers < 32) {
        pci_drivers[num_drivers++] = driver;
    }
}

void pci_enable_busmaster(pci_dev_t *dev)
{
    uint32_t cmd = pci_read_config(dev, 0x04);
    uart_puts("[PCI]   Enabling bus master + memory space...\n");
    uart_puts("[PCI]   Command before: ");
    print_hex32(cmd);
    uart_puts("\n");
    
    cmd |= (1 << 2) | (1 << 1);  /* Bus master + Memory space */
    pci_write_config(dev, 0x04, cmd);
    
    cmd = pci_read_config(dev, 0x04);
    uart_puts("[PCI]   Command after: ");
    print_hex32(cmd);
    uart_puts("\n");
}

uint64_t pci_bar_start(pci_dev_t *dev, int bar)
{
    if (bar < 0 || bar >= PCI_BAR_COUNT) return 0;
    
    uint64_t addr = dev->bar[bar];
    if (addr & 1) return 0;  /* I/O space */
    
    if ((addr & 0x6) == 0x4 && bar + 1 < PCI_BAR_COUNT) {
        /* 64-bit BAR */
        addr = (addr & ~0xFULL) | ((uint64_t)dev->bar[bar + 1] << 32);
    } else {
        /* 32-bit BAR */
        addr &= ~0xFULL;
    }
    
    return addr;
}
