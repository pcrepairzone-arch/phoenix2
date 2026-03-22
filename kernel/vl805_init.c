/*
 * vl805_init.c – VL805 xHCI firmware init via VideoCore mailbox
 *
 * The Pi 4 bootloader normally loads vl805.bin to the VL805 via the
 * VideoCore before handing off to the kernel.  If that doesn't happen
 * (vl805.bin missing from SD, or we're doing our own boot), the xHCI
 * registers read as garbage until we tell the VC firmware to initialise
 * the controller via mailbox tag 0x00030058 (NOTIFY_XHCI_RESET).
 *
 * Mailbox buffer addresses sent to the GPU must use the ARM→GPU bus alias:
 * Pi 4: GPU_addr = (ARM_phys & 0x0FFFFFFF) | 0x40000000
 * (0x40000000 = L2 cache-coherent DMA alias; VC reads ARM L2 cache directly.
 *  0xC0000000 is the NON-coherent alias — VC bypasses ARM L2 cache, reads
 *  stale DRAM, gets zeros → mailbox returns code=0x00000000 every time.)
 *
 * IMPORTANT: The mailbox base address is NOT hardcoded.  The BCM2711 has
 * two possible peripheral base addresses depending on boot mode:
 *   Pi 4B / CM4 low mode:   0xFE000000  → mailbox at 0xFE00B880
 *   CM4 high mode (rare):   0x47E000000 → mailbox at 0x47E00B880
 * periph_base.c detects the correct base at startup via System Timer sniff.
 * We call get_mailbox_base() here instead of hardcoding 0xFE000000.
 *
 * Author: R Andrews – 27 Feb 2026
 */

#include "kernel.h"

extern void uart_puts(const char *s);
extern void uart_putc(char c);
extern uint64_t get_mailbox_base(void);   /* periph_base.c — detects at runtime */

#define MBOX_READ_REG       0x00
#define MBOX_STATUS_REG     0x18
#define MBOX_WRITE_REG      0x20

#define MBOX_FULL           0x80000000U
#define MBOX_EMPTY          0x40000000U
#define MBOX_CHAN_PROP      8

/* ── Low-level mailbox ─────────────────────────────────────────── */

static void mbox_write(uint32_t chan, uint32_t gpu_addr)
{
    uint64_t mbox_base = get_mailbox_base();
    volatile uint32_t *status = (volatile uint32_t *)(mbox_base + MBOX_STATUS_REG);
    volatile uint32_t *write  = (volatile uint32_t *)(mbox_base + MBOX_WRITE_REG);
    while (*status & MBOX_FULL) asm volatile("nop");
    asm volatile("dmb sy" ::: "memory");
    *write = (gpu_addr & ~0xFU) | (chan & 0xFU);
}

static uint32_t mbox_read(uint32_t chan)
{
    uint64_t mbox_base = get_mailbox_base();
    volatile uint32_t *status = (volatile uint32_t *)(mbox_base + MBOX_STATUS_REG);
    volatile uint32_t *read   = (volatile uint32_t *)(mbox_base + MBOX_READ_REG);
    while (1) {
        while (*status & MBOX_EMPTY) asm volatile("nop");
        asm volatile("dmb sy" ::: "memory");
        uint32_t v = *read;
        if ((v & 0xFU) == chan) return v & ~0xFU;
    }
}

static int mbox_property(volatile uint32_t *buf)
{
    uint32_t arm_phys = (uint32_t)((uintptr_t)buf & 0xFFFFFFFFU);
    if (arm_phys & 0xFU) { uart_puts("[MBOX] not aligned\n"); return -1; }
    /* Pi 4 L2 cache-coherent DMA alias: 0x40000000 (NOT 0xC0000000).
     * Using 0xC0000000 causes the VC to bypass the ARM L2 cache and read
     * stale DRAM — the mailbox buffer appears as all-zeros to the VC and
     * it returns no response (code stays 0x00000000).
     * Using 0x40000000 the VC reads through the ARM L2 cache and sees the
     * freshly written buffer contents. */
    uint32_t gpu_addr = (arm_phys & 0x0FFFFFFFU) | 0x40000000U;

    buf[1] = 0;
    asm volatile("dmb sy" ::: "memory");
    mbox_write(MBOX_CHAN_PROP, gpu_addr);
    (void)mbox_read(MBOX_CHAN_PROP);
    asm volatile("dmb sy" ::: "memory");

    if (buf[1] != 0x80000000U) {
        uart_puts("[MBOX] call failed code=");
        uint32_t c = buf[1];
        for (int i = 28; i >= 0; i -= 4) {
            int n = (c >> i) & 0xF;
            uart_putc(n < 10 ? '0' + n : 'a' + n - 10);
        }
        uart_puts("\n");
        return -1;
    }
    return 0;
}

/* ── VL805 init ────────────────────────────────────────────────── */

int vl805_init(void)
{
    /* Print the mailbox base we're actually using — verifies periph detection */
    uint64_t mbox_base = get_mailbox_base();
    uart_puts("[VL805] Mailbox base=0x");
    for (int _i = 60; _i >= 0; _i -= 4) {
        int _n = (mbox_base >> _i) & 0xF;
        uart_putc(_n < 10 ? '0' + _n : 'a' + _n - 10);
    }
    uart_puts("\n");
    uart_puts("[VL805] Requesting firmware via mailbox 0x00030058...\n");

    static volatile uint32_t __attribute__((aligned(16))) buf[8];
    buf[0] = 8 * 4;
    buf[1] = 0;
    buf[2] = 0x00030058U;   /* NOTIFY_XHCI_RESET */
    buf[3] = 4;
    buf[4] = 4;
    buf[5] = 0x00100000U;  /* PCIe BDF: bus=1, dev=0, fn=0 (bits[23:20]=bus) */
    buf[6] = 0;
    buf[7] = 0;

    if (mbox_property(buf) < 0) {
        uart_puts("[VL805] Firmware load failed\n");
        return -1;
    }

    uart_puts("[VL805] Firmware loaded OK\n");

    /* VL805 needs ~100ms to come out of reset */
    for (volatile int i = 0; i < 1000000; i++) asm volatile("nop");

    return 0;
}
