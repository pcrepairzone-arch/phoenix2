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

/* boot266: BCM2711 PM watchdog disable.
 *
 * The VideoCore firmware arms the hardware watchdog when it loads kernel8.img.
 * Default timeout is ~16 seconds.  Our USB MSC enumeration spends up to 20 s
 * on retry loops for slow/stalled devices, which fires the watchdog and causes
 * a hard reboot exactly as the WIMP task starts.
 *
 * Disable immediately after peripheral_base is known, before any long ops.
 *
 * BCM2711 PM registers (periph_base + 0x100000):
 *   PM_RSTC  (+0x1c)  bits[5:4] = WRCFG — 0x00=no-reset 0x20=full-reset
 *   PM_WDOG  (+0x24)  countdown timer — 0 = stopped
 *   All writes require the 'password' in bits[31:24] = 0x5A.
 */
void wdog_disable(void)
{
    volatile uint32_t *pm_rstc =
        (volatile uint32_t *)(peripheral_base + 0x10001cULL);
    volatile uint32_t *pm_wdog =
        (volatile uint32_t *)(peripheral_base + 0x100024ULL);
    const uint32_t PASSWD = 0x5A000000U;

    uint32_t rstc_before = *pm_rstc;
    uint32_t wdog_before = *pm_wdog;

    *pm_wdog = PASSWD | 0U;                          /* stop countdown        */
    *pm_rstc = PASSWD | (rstc_before & ~0x30U);      /* WRCFG = no-reset      */
    /* memory barrier so writes reach the PMIC before anything else runs */
    __asm__ volatile("dsb sy" ::: "memory");

    /* Log via uart_puts (UART not yet init'd — but we're called after
     * detect_peripheral_base, which only reads timers; uart_init() happens
     * next.  So use debug_print only if uart already works.  Print after
     * uart_init() instead: kernel.c will print the confirm line.             */
    (void)wdog_before; /* available for debug if needed */
}
