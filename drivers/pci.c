/*
 * pci.c - Try direct VL805 MMIO access
 * Bypass PCIe config space entirely!
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

static pci_driver_t *pci_drivers[32];
static int num_drivers = 0;

void pci_init(void)
{
    uart_puts("\n");
    uart_puts("╔════════════════════════════════════════╗\n");
    uart_puts("║  DIRECT VL805 MMIO ACCESS TEST         ║\n");
    uart_puts("╚════════════════════════════════════════╝\n\n");
    
    /* The VL805 xHCI controller should be memory-mapped */
    /* According to Pi 4 docs, PCIe devices map at 0x600000000 */
    
    uart_puts("[DIRECT] Attempting to access VL805 xHCI directly...\n\n");
    
    /* Try different possible base addresses */
    uint64_t test_addresses[] = {
        0x600000000ULL,  /* PCIe memory window start */
        0x600010000ULL,  /* Offset */
        0x600100000ULL,  /* Different offset */
        0xC0000000ULL,   /* Legacy 32-bit BAR */
        0xFD500000ULL,   /* PCIe controller base */
    };
    
    for (int i = 0; i < 5; i++) {
        uint64_t addr = test_addresses[i];
        
        uart_puts("[DIRECT] Testing address ");
        print_hex32((uint32_t)(addr >> 32));
        print_hex32((uint32_t)addr);
        uart_puts(":\n");
        
        /* Read first few dwords */
        volatile uint32_t *ptr = (volatile uint32_t *)addr;
        
        uart_puts("[DIRECT]   +0x00: ");
        print_hex32(ptr[0]);
        uart_puts("\n");
        
        uart_puts("[DIRECT]   +0x04: ");
        print_hex32(ptr[1]);
        uart_puts("\n");
        
        uart_puts("[DIRECT]   +0x08: ");
        print_hex32(ptr[2]);
        uart_puts("\n");
        
        uart_puts("[DIRECT]   +0x0C: ");
        print_hex32(ptr[3]);
        uart_puts("\n\n");
        
        /* Check if this looks like xHCI capability registers */
        uint32_t caplength = ptr[0] & 0xFF;
        uint32_t hciversion = ptr[0] >> 16;
        
        if (hciversion == 0x0100 || hciversion == 0x0110 || hciversion == 0x0120) {
            uart_puts("*** POSSIBLE xHCI CONTROLLER FOUND! ***\n");
            uart_puts("[DIRECT] HCI Version: ");
            print_hex32(hciversion);
            uart_puts("\n");
            uart_puts("[DIRECT] Capability Length: ");
            print_hex32(caplength);
            uart_puts("\n\n");
        }
    }
    
    uart_puts("\n[DIRECT] If all addresses show zeros, VL805 is powered off.\n");
    uart_puts("[DIRECT] If we find xHCI signature, we can use it directly!\n\n");
}

uint32_t pci_read_config(pci_dev_t *dev, uint32_t offset) { return 0xFFFFFFFF; }
void pci_write_config(pci_dev_t *dev, uint32_t offset, uint32_t value) {}
void pci_register_driver(pci_driver_t *driver) {}
void pci_enable_busmaster(pci_dev_t *dev) {}
uint64_t pci_bar_start(pci_dev_t *dev, int bar) { return 0; }
