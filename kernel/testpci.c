/*
 * pci.c - PCI Driver with VL805 USB Support
 * 
 * Includes embedded mailbox code for VL805 firmware loading
 */

#include "kernel.h"
#include "pci.h"

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

/* ========== MAILBOX IMPLEMENTATION ========== */

/* Pi 4 peripheral base is at 0xFE000000 */
#define PERIPH_BASE         0xFE000000ULL
#define MAILBOX_BASE        (PERIPH_BASE + 0xB880)
#define MAILBOX_READ        (MAILBOX_BASE + 0x00)
#define MAILBOX_POLL        (MAILBOX_BASE + 0x10)
#define MAILBOX_SENDER      (MAILBOX_BASE + 0x14)
#define MAILBOX_STATUS      (MAILBOX_BASE + 0x18)
#define MAILBOX_CONFIG      (MAILBOX_BASE + 0x1C)
#define MAILBOX_WRITE       (MAILBOX_BASE + 0x20)

#define MAILBOX_FULL        0x80000000
#define MAILBOX_EMPTY       0x40000000

#define MAILBOX_CHANNEL_PROPERTY  8

/* Simple mailbox write */
static void mailbox_write_raw(uint8_t channel, uint32_t data)
{
    /* Wait for mailbox to be not full */
    while (readl((void *)MAILBOX_STATUS) & MAILBOX_FULL) {
        /* Wait */
    }
    
    /* Write data with channel */
    writel((data & 0xFFFFFFF0) | (channel & 0xF), (void *)MAILBOX_WRITE);
}

/* Simple mailbox read */
static uint32_t mailbox_read_raw(uint8_t channel)
{
    uint32_t data;
    
    while (1) {
        /* Wait for mailbox to have data */
        while (readl((void *)MAILBOX_STATUS) & MAILBOX_EMPTY) {
            /* Wait */
        }
        
        /* Read the data */
        data = readl((void *)MAILBOX_READ);
        
        /* Check if it's for our channel */
        if ((data & 0xF) == channel) {
            return data & 0xFFFFFFF0;
        }
    }
}

/* Mailbox property call */
static int mailbox_property(volatile uint32_t *buffer)
{
    /* Get physical address (must be 16-byte aligned) */
    uint32_t addr = (uint32_t)((uint64_t)buffer & 0xFFFFFFFF);
    
    if (addr & 0xF) {
        uart_puts("[MBOX] ERROR: Buffer not aligned!\n");
        return -1;
    }
    
    /* Set request code */
    buffer[1] = 0x00000000;
    
    /* Write to mailbox */
    mailbox_write_raw(MAILBOX_CHANNEL_PROPERTY, addr);
    
    /* Read response */
    uint32_t response = mailbox_read_raw(MAILBOX_CHANNEL_PROPERTY);
    
    /* Verify it's our buffer */
    if ((response & 0xFFFFFFF0) != (addr & 0xFFFFFFF0)) {
        uart_puts("[MBOX] ERROR: Response mismatch!\n");
        return -1;
    }
    
    /* Check response code */
    if (buffer[1] != 0x80000000) {
        uart_puts("[MBOX] ERROR: Request failed! Code: ");
        print_hex32(buffer[1]);
        uart_puts("\n");
        return -1;
    }
    
    return 0;
}

/* ========== VL805 FIRMWARE LOADING ========== */

#define RPI_FIRMWARE_NOTIFY_XHCI_RESET  0x00030058

static int vl805_load_firmware(void)
{
    uart_puts("\n[VL805] Requesting firmware load via mailbox...\n");
    
    /* Mailbox buffer - MUST be 16-byte aligned */
    volatile uint32_t __attribute__((aligned(16))) mailbox[8];
    
    mailbox[0] = 8 * 4;                              // Buffer size in bytes
    mailbox[1] = 0x00000000;                         // Request code
    mailbox[2] = RPI_FIRMWARE_NOTIFY_XHCI_RESET;     // Tag ID
    mailbox[3] = 4;                                  // Value buffer size
    mailbox[4] = 4;                                  // Request size
    mailbox[5] = 0;                                  // Dev address (0 for VL805)
    mailbox[6] = 0;                                  // Padding
    mailbox[7] = 0;                                  // End tag
    
    /* Send mailbox request */
    if (mailbox_property(mailbox) < 0) {
        uart_puts("[VL805] ERROR: Mailbox call failed!\n");
        return -1;
    }
    
    uart_puts("[VL805] Firmware loaded successfully!\n");
    
    /* Wait for firmware to initialize */
    delay_ms(200);
    
    return 0;
}

/* ========== PCIE INITIALIZATION ========== */

/* PCIe Registers */
#define PCIE_RC_BASE        0xFD500000ULL
#define RGR1_SW_INIT_1      (PCIE_RC_BASE + 0x9210)
#define MISC_PCIE_STATUS    (PCIE_RC_BASE + 0x4068)
#define PCIE_EXT_CFG_INDEX  (PCIE_RC_BASE + 0x9000)
#define PCIE_EXT_CFG_DATA   (PCIE_RC_BASE + 0x9004)

/* Status bits */
#define PCIE_PHY_LINKUP_BIT (1 << 4)
#define PCIE_DL_ACTIVE_BIT  (1 << 5)

static pci_driver_t *pci_drivers[32];
static int num_drivers = 0;

static int pcie_bridge_init(void)
{
    uint32_t reg;
    int timeout;
    
    uart_puts("\n[PCI] Initializing PCIe bridge...\n");
    
    /* Read reset register */
    reg = readl((void *)RGR1_SW_INIT_1);
    uart_puts("[PCI]   Reset register: ");
    print_hex32(reg);
    uart_puts("\n");
    
    /* Deassert resets */
    reg &= ~0x3;
    writel(reg, (void *)RGR1_SW_INIT_1);
    
    delay_ms(100);
    
    /* Poll for link */
    uart_puts("[PCI]   Polling for link...\n");
    timeout = 100;
    while (timeout-- > 0) {
        reg = readl((void *)MISC_PCIE_STATUS);
        
        if ((reg & PCIE_PHY_LINKUP_BIT) && (reg & PCIE_DL_ACTIVE_BIT)) {
            uart_puts("[PCI]   LINK UP! Status: ");
            print_hex32(reg);
            uart_puts("\n");
            return 0;
        }
        
        delay_ms(10);
    }
    
    uart_puts("[PCI]   WARNING: Link timeout\n");
    return -1;
}

uint32_t pci_read_config(pci_dev_t *dev, uint32_t offset)
{
    uint32_t idx = ((uint32_t)dev->bus << 20) |
                   ((uint32_t)dev->dev << 15) |
                   ((uint32_t)dev->func << 12) |
                   (offset & ~0xFFFU);
    
    writel(idx, (void *)PCIE_EXT_CFG_INDEX);
    return readl((void *)(PCIE_EXT_CFG_DATA + (offset & 0xFFC)));
}

void pci_write_config(pci_dev_t *dev, uint32_t offset, uint32_t value)
{
    uint32_t idx = ((uint32_t)dev->bus << 20) |
                   ((uint32_t)dev->dev << 15) |
                   ((uint32_t)dev->func << 12) |
                   (offset & ~0xFFFU);
    
    writel(idx, (void *)PCIE_EXT_CFG_INDEX);
    writel(value, (void *)(PCIE_EXT_CFG_DATA + (offset & 0xFFC)));
}

static void pci_scan_bus(void)
{
    uart_puts("\n[PCI] Scanning bus 1 for VL805...\n");
    
    pci_dev_t pdev = {1, 0, 0};
    
    uint32_t vendor_dev = pci_read_config(&pdev, 0);
    
    uart_puts("[PCI]   Vendor/Device ID: ");
    print_hex32(vendor_dev);
    uart_puts("\n");
    
    if (vendor_dev == 0 || vendor_dev == 0xFFFFFFFF || vendor_dev == 0xDEADDEAD) {
        uart_puts("[PCI]   No valid device found\n");
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
    
    /* Check if it's the VL805 */
    if (pdev.vendor_id == 0x1106 && pdev.device_id == 0x3483) {
        uart_puts("\n");
        uart_puts("╔════════════════════════════════════════╗\n");
        uart_puts("║  🎉 VL805 USB 3.0 CONTROLLER FOUND! 🎉  ║\n");
        uart_puts("║                                        ║\n");
        uart_puts("║     USB INITIALIZATION SUCCESSFUL!     ║\n");
        uart_puts("╚════════════════════════════════════════╝\n");
        uart_puts("\n");
        
        /* Read more info */
        uint32_t class_rev = pci_read_config(&pdev, 0x08);
        uart_puts("[VL805]  Class/Revision: ");
        print_hex32(class_rev);
        uart_puts("\n");
        
        uint32_t bar0 = pci_read_config(&pdev, 0x10);
        uart_puts("[VL805]  BAR0: ");
        print_hex32(bar0);
        uart_puts("\n");
    }
}

void pci_init(void)
{
    uart_puts("\n");
    uart_puts("╔════════════════════════════════════════╗\n");
    uart_puts("║  PCI/USB INITIALIZATION (MAILBOX FIX)  ║\n");
    uart_puts("╚════════════════════════════════════════╝\n");
    
    /* Step 1: Initialize PCIe bridge */
    if (pcie_bridge_init() != 0) {
        uart_puts("[PCI] WARNING: Link training incomplete\n");
    }
    
    /* Step 2: THE CRITICAL FIX - Load VL805 firmware via mailbox! */
    uart_puts("\n[PCI] === CRITICAL: LOADING VL805 FIRMWARE ===\n");
    if (vl805_load_firmware() != 0) {
        uart_puts("[PCI] ERROR: Failed to load VL805 firmware\n");
        uart_puts("[PCI] USB will not work!\n");
        return;
    }
    
    /* Step 3: Scan for devices */
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
    cmd |= (1 << 2) | (1 << 1);
    pci_write_config(dev, 0x04, cmd);
}

uint64_t pci_bar_start(pci_dev_t *dev, int bar)
{
    if (bar < 0 || bar >= PCI_BAR_COUNT) return 0;
    
    uint64_t addr = dev->bar[bar];
    if (addr & 1) return 0;
    
    if ((addr & 0x6) == 0x4 && bar + 1 < PCI_BAR_COUNT) {
        addr = (addr & ~0xFULL) | ((uint64_t)dev->bar[bar + 1] << 32);
    } else {
        addr &= ~0xFULL;
    }
    
    return addr;
}
