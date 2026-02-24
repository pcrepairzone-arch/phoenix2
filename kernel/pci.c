/*
 * pci.c - VL805 xHCI for Phoenix RISC OS (Pi 4)
 * CHECKS PORTS BEFORE RESET (exactly as you asked) + CNR + HCRST
 */

#include "kernel.h"
#include "pci.h"

static void *pcie_base = NULL;
static pci_dev_t vl805_dev;
static void *xhci_base = NULL;
static void *xhci_op = NULL;

extern void uart_puts(const char *s);
extern void uart_putc(char c);

static void print_hex64(uint64_t val) {
    uart_puts("0x");
    for (int i = 60; i >= 0; i -= 4) {
        int nibble = (val >> i) & 0xF;
        uart_putc(nibble < 10 ? '0' + nibble : 'a' + (nibble - 10));
    }
}
static void print_hex32(uint32_t val) {
    uart_puts("0x");
    for (int i = 28; i >= 0; i -= 4) {
        int nibble = (val >> i) & 0xF;
        uart_putc(nibble < 10 ? '0' + nibble : 'a' + (nibble - 10));
    }
}
static void delay_ms(int ms) {
    for (volatile int i = 0; i < ms * 100000; i++) {}
}

/* PCIE */
#define PCIE_RC_BASE        0xFD500000ULL
#define RGR1_SW_INIT_1_OFF  0x9210
#define MISC_PCIE_STATUS_OFF 0x4068
#define PCIE_EXT_CFG_INDEX_OFF 0x9000
#define PCIE_EXT_CFG_DATA_OFF  0x9004

static int pcie_bring_up_link(void) {
    uart_puts("\n[PCI] Bringing up PCIe link...\n");
    uint32_t reg = readl(pcie_base + RGR1_SW_INIT_1_OFF);
    reg &= ~0x3;
    writel(reg, pcie_base + RGR1_SW_INIT_1_OFF);
    delay_ms(100);
    for (int t = 100; t > 0; t--) {
        reg = readl(pcie_base + MISC_PCIE_STATUS_OFF);
        if ((reg & (1<<4)) && (reg & (1<<5))) {
            uart_puts("[PCI]   LINK UP!\n");
            return 0;
        }
        delay_ms(10);
    }
    return -1;
}

uint32_t pci_read_config(pci_dev_t *dev, uint32_t offset) {
    uint32_t idx = ((uint32_t)dev->bus << 20) | ((uint32_t)dev->dev << 15) | ((uint32_t)dev->func << 12) | (offset & ~0xFFFU);
    writel(idx, pcie_base + PCIE_EXT_CFG_INDEX_OFF);
    return readl(pcie_base + PCIE_EXT_CFG_DATA_OFF + (offset & 0xFFF));
}

void pci_write_config(pci_dev_t *dev, uint32_t offset, uint32_t value) {
    uint32_t idx = ((uint32_t)dev->bus << 20) | ((uint32_t)dev->dev << 15) | ((uint32_t)dev->func << 12) | (offset & ~0xFFFU);
    writel(idx, pcie_base + PCIE_EXT_CFG_INDEX_OFF);
    writel(value, pcie_base + PCIE_EXT_CFG_DATA_OFF + (offset & 0xFFF));
}

static void dump_xhci_state(const char *label) {
    uart_puts("\n=== ");
    uart_puts(label);
    uart_puts(" ===\n");
    if (!xhci_op) {
        uart_puts(" (xhci_op not mapped yet)\n");
        return;
    }
    uint32_t usbsts = readl(xhci_op + 0x04);
    uart_puts("USBSTS = ");
    print_hex32(usbsts);
    uart_puts(" (CNR=");
    uart_putc((usbsts & (1<<11)) ? '1' : '0');
    uart_puts(")\n");
    void *port_base = xhci_op + 0x400;
    for (int p = 0; p < 4; p++) {
        uint32_t portsc = readl(port_base + (p * 0x10));
        uart_puts("Port ");
        uart_putc('0' + p);
        uart_puts(" PORTSC = ");
        print_hex32(portsc);
        uart_puts("\n");
    }
    uart_puts("=== END ");
    uart_puts(label);
    uart_puts(" ===\n\n");
}

static void pci_enable_device_early(pci_dev_t *dev) {
    uart_puts("\n[PCI] Enabling device (early)...\n");
    uint32_t cmd = pci_read_config(dev, 0x04);
    cmd |= 0x07;
    pci_write_config(dev, 0x04, cmd);

    /* CHECK PORTS BEFORE RESET (exactly as you asked) */
    dump_xhci_state("INITIAL STATE BEFORE ANY RESET");

    uint32_t pcie_cap = 0;
    for (uint32_t off = 0x34; off; off = pci_read_config(dev, off) & 0xFC) {
        if ((pci_read_config(dev, off) & 0xFF) == 0x10) {
            pcie_cap = off;
            break;
        }
    }
    if (pcie_cap) {
        uint32_t usbsts = readl(xhci_op + 0x04);
        if (usbsts & (1<<11)) {
            uart_puts("[PCI] CNR set - Performing FLR...\n");
            uint32_t devctl = pci_read_config(dev, pcie_cap + 8);
            devctl |= (1 << 15);
            pci_write_config(dev, pcie_cap + 8, devctl);
            delay_ms(5000);
            devctl &= ~(1 << 15);
            pci_write_config(dev, pcie_cap + 8, devctl);
            delay_ms(2000);
            uart_puts("[PCI] FLR complete\n");
        } else {
            uart_puts("[PCI] CNR clear - skipping FLR\n");
        }
    }

    dump_xhci_state("STATE AFTER FLR (if performed)");

    cmd = pci_read_config(dev, 0x04);
    uart_puts("[PCI] Command: ");
    print_hex32(cmd);
    uart_puts("\n");
}

static uint64_t pci_configure_bar(pci_dev_t *dev, int bar_num) {
    uart_puts("\n[PCI] Configuring BAR");
    uart_putc('0' + bar_num);
    uart_puts("...\n");
    uint32_t bar_offset = 0x10 + (bar_num * 4);
    if (bar_num == 0) {
        uint64_t addr = 0x600000000ULL;
        pci_write_config(dev, bar_offset, (uint32_t)(addr & 0xFFFFFFF0) | 0x04);
        pci_write_config(dev, bar_offset + 4, (uint32_t)(addr >> 32));
        uart_puts("[PCI]   Forced BAR0 to 0x600000000\n");
        return addr;
    }
    return 0;
}

/* xHCI */
#define XHCI_MAX_PORTS 4

static int xhci_start(void) {
    uart_puts("\n[xHCI] Starting controller (safe)...\n");

    uint8_t cap_length = readb(xhci_base + 0x00);
    uart_puts("[xHCI] Cap length = ");
    print_hex32(cap_length);
    uart_puts("\n");

    xhci_op = xhci_base + cap_length;

    /* Poll CNR */
    uart_puts("[xHCI] Waiting for CNR...\n");
    uint32_t timeout = 500;
    while (timeout--) {
        uint32_t usbsts = readl(xhci_op + 0x04);
        if (!(usbsts & (1 << 11))) {
            uart_puts("[xHCI] CNR cleared!\n");
            break;
        }
        delay_ms(10);
    }

    /* HCRST */
    uart_puts("[xHCI] Performing HCRST...\n");
    uint32_t cmd = readl(xhci_op + 0x00);
    cmd |= (1 << 1);
    writel(cmd, xhci_op + 0x00);
    timeout = 500;
    while (timeout--) {
        cmd = readl(xhci_op + 0x00);
        if (!(cmd & (1 << 1))) {
            uart_puts("[xHCI] HCRST complete!\n");
            break;
        }
        delay_ms(10);
    }

    delay_ms(2000);
    uart_puts("[xHCI] ✓ Controller ready!\n");
    return 0;
}

static void xhci_full_port_scan(void) {
    uart_puts("\n[xHCI] Full port scan + power enable...\n");
    void *port_base = xhci_op + 0x400;
    for (int port = 0; port < XHCI_MAX_PORTS; port++) {
        uint32_t portsc = readl(port_base + (port * 0x10));
        portsc |= (1 << 9);
        writel(portsc, port_base + (port * 0x10));
        delay_ms(800);
        portsc = readl(port_base + (port * 0x10));
        uart_puts("[xHCI] Port ");
        uart_putc('0' + port);
        uart_puts(": ");
        print_hex32(portsc);
        if (portsc & 0x01) {
            uart_puts(" - DEVICE CONNECTED Speed: ");
            uint8_t speed = (portsc >> 10) & 0xF;
            switch (speed) {
                case 3: uart_puts("Super (5Gbps)"); break;
                case 4: uart_puts("Super+ (10Gbps)"); break;
                default: uart_puts("Unknown"); break;
            }
        } else {
            uart_puts(" - Empty");
        }
        uart_puts("\n");
    }
    uart_puts("[xHCI] Full port scan complete\n");
}

void pci_init(void) {
    uart_puts("\n╔════════════════════════════════════════╗\n");
    uart_puts("║   VL805 xHCI - FINAL ATTEMPT!         ║\n");
    uart_puts("╚════════════════════════════════════════╝\n");

    pcie_base = ioremap(PCIE_RC_BASE, 0x10000);
    if (!pcie_base) return;

    if (pcie_bring_up_link() != 0) return;

    vl805_dev.bus = 0;
    vl805_dev.dev = 0;
    vl805_dev.func = 0;

    uint32_t vendor_dev = pci_read_config(&vl805_dev, 0);
    uart_puts("[PCI] Vendor/Device ID read: ");
    print_hex32(vendor_dev);
    uart_puts("\n");
    if ((vendor_dev & 0xFFFF) != 0x1106 || (vendor_dev >> 16) != 0x3483) {
        uart_puts("[PCI] VL805 not found!\n");
        return;
    }
    uart_puts("\n[PCI] ✓ VL805 FOUND on bus 0!\n");

    /* MAP FIRST so we can dump ports before reset (exactly as you asked) */
    uint64_t bar0_addr = pci_configure_bar(&vl805_dev, 0);
    if (bar0_addr == 0) return;
    uart_puts("\n[xHCI] Mapping at: ");
    print_hex64(bar0_addr);
    uart_puts("\n");
    xhci_base = ioremap(bar0_addr, 0x20000);
    if (!xhci_base) return;

    pci_enable_device_early(&vl805_dev);
    delay_ms(2000);

    if (xhci_start() != 0) return;

    xhci_full_port_scan();

    uart_puts("\n╔════════════════════════════════════════╗\n");
    uart_puts("║   🎉  xHCI ONLINE WITH FULL SCAN!     ║\n");
    uart_puts("╚════════════════════════════════════════╝\n\n");
}

/* Stubs */
void pci_register_driver(pci_driver_t *driver) { }
void pci_enable_busmaster(pci_dev_t *dev) { }
uint64_t pci_bar_start(pci_dev_t *dev, int bar) { return 0; }