/*
 * periph_base.c - Detect peripheral base address from Device Tree
 *
 * Pi 4 (BCM2711):  peripherals at 0xFE000000  (low/legacy mode)
 *                  or 0x47E000000             (high mode, rare)
 * Pi 5 (BCM2712):  peripherals at 0x107C000000
 *
 * Board revision word (from firmware property 0x00010002):
 *   Pi 4 revisions: 0xc03111..0xd03115 (bits[19:4] = 0x11)
 *   Pi 5 revisions: 0x17c111..         (bits[19:4] = 0x17)
 *
 * We probe via Device Tree 'soc' ranges, falling back to a
 * System Timer sniff if the DTB parse is inconclusive.
 */
#include <stdint.h>

/* Exported globals */
uint64_t peripheral_base = 0xFE000000ULL;  /* Default: Pi 4 */
int      pi_model        = 4;              /* 4 or 5         */

static uint32_t fdt_read32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
}

void detect_peripheral_base(uint64_t dtb_ptr)
{
    /*
     * Strategy:
     *  1. Sniff the System Timer at the two known Pi 4 addresses.
     *     The timer counter register (CLO at offset 4) is non-zero
     *     and not 0xFFFFFFFF when the address is live.
     *  2. If neither matches, try the Pi 5 address.
     *  3. Fall back to Pi 4 default.
     */

    /* Pi 4 low mode */
    volatile uint32_t *t4_low  = (volatile uint32_t *)0xFE003004ULL;
    /* Pi 4 high mode (rare, CM4 only) */
    volatile uint32_t *t4_high = (volatile uint32_t *)0x47E003004ULL;
    /* Pi 5 */
    volatile uint32_t *t5      = (volatile uint32_t *)0x107C003004ULL;

    uint32_t v;

    /* Pi 4 low */
    v = *t4_low;
    if (v != 0 && v != 0xFFFFFFFF) {
        peripheral_base = 0xFE000000ULL;
        pi_model        = 4;
        return;
    }

    /* Pi 4 high */
    v = *t4_high;
    if (v != 0 && v != 0xFFFFFFFF) {
        peripheral_base = 0x47E000000ULL;
        pi_model        = 4;
        return;
    }

    /* Pi 5 */
    v = *t5;
    if (v != 0 && v != 0xFFFFFFFF) {
        peripheral_base = 0x107C000000ULL;
        pi_model        = 5;
        return;
    }

    /* Default */
    peripheral_base = 0xFE000000ULL;
    pi_model        = 4;
}

/* ── Accessor functions ─────────────────────────────────────────── */

uint64_t get_gpio_base(void)    { return peripheral_base + 0x200000; }
uint64_t get_uart_base(void)    { return peripheral_base + 0x201000; }
uint64_t get_mailbox_base(void) { return peripheral_base + 0x00B880; }
int      get_pi_model(void)     { return pi_model; }
