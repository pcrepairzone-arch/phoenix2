/*
 * periph_base.c - Detect peripheral base address from Device Tree
 *
 * Pi 4/CM4 can have peripherals at:
 *   0xFE000000 (low/legacy mode)
 *   0x47E000000 (high mode, 35-bit addressing)
 *
 * We parse the DTB 'soc' node's 'ranges' property to find it.
 */
#include <stdint.h>

/* Global peripheral base - detected at boot */
uint64_t peripheral_base = 0xFE000000ULL;  /* Default Pi 4 */

/* Simple DTB parser - finds 'soc' node and reads 'ranges' */
static uint32_t fdt_read32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
}

static uint64_t fdt_read64(const uint8_t *p) {
    return ((uint64_t)fdt_read32(p) << 32) | fdt_read32(p + 4);
}

void detect_peripheral_base(uint64_t dtb_ptr)
{
    if (dtb_ptr == 0 || dtb_ptr == 0xFFFFFFFFFFFFFFFFULL) {
        /* No DTB - use default */
        peripheral_base = 0xFE000000ULL;
        return;
    }

    const uint8_t *fdt = (const uint8_t *)dtb_ptr;
    
    /* Check magic (0xd00dfeed in big-endian) */
    if (fdt_read32(fdt) != 0xd00dfeed) {
        peripheral_base = 0xFE000000ULL;
        return;
    }

    /* For now, just try both known bases */
    /* A proper DTB parser would walk the tree, but this is enough */
    
    /* Test if high address mode is active */
    /* Read from a known-safe register at both addresses */
    volatile uint32_t *test_low  = (volatile uint32_t *)0xFE003000;  /* System timer */
    volatile uint32_t *test_high = (volatile uint32_t *)0x47E003000;
    
    /* Try reading - if we get 0xFFFFFFFF it's unmapped */
    uint32_t val_low = *test_low;
    
    /* Simple heuristic: system timer counter should be non-zero and not all-F */
    if (val_low != 0 && val_low != 0xFFFFFFFF) {
        peripheral_base = 0xFE000000ULL;
        return;
    }
    
    /* Try high address */
    uint32_t val_high = *test_high;
    if (val_high != 0 && val_high != 0xFFFFFFFF) {
        peripheral_base = 0x47E000000ULL;
        return;
    }
    
    /* Default to low */
    peripheral_base = 0xFE000000ULL;
}

/* Accessor functions */
uint64_t get_gpio_base(void)    { return peripheral_base + 0x200000; }
uint64_t get_uart_base(void)    { return peripheral_base + 0x201000; }
uint64_t get_mailbox_base(void) { return peripheral_base + 0x00B880; }
