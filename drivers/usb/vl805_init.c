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
 * GPU_addr = (ARM_phys & 0x3FFFFFFF) | 0xC0000000
 *
 * Author: R Andrews – 27 Feb 2026
 */

#include "kernel.h"

extern void uart_puts(const char *s);
extern void uart_putc(char c);

/* ── Peripheral addresses ──────────────────────────────────────── */
#define PI4_PERIPH_BASE     0xFE000000ULL
#define MBOX_BASE           (PI4_PERIPH_BASE + 0xB880)

#define MBOX_READ_REG       0x00
#define MBOX_STATUS_REG     0x18
#define MBOX_WRITE_REG      0x20

#define MBOX_FULL           0x80000000U
#define MBOX_EMPTY          0x40000000U
#define MBOX_CHAN_PROP      8

/* ── Low-level mailbox ─────────────────────────────────────────── */

static void mbox_write(uint32_t chan, uint32_t gpu_addr)
{
    volatile uint32_t *status = (volatile uint32_t *)(MBOX_BASE + MBOX_STATUS_REG);
    volatile uint32_t *write  = (volatile uint32_t *)(MBOX_BASE + MBOX_WRITE_REG);
    while (*status & MBOX_FULL) asm volatile("nop");
    asm volatile("dmb sy" ::: "memory");
    *write = (gpu_addr & ~0xFU) | (chan & 0xFU);
}

static uint32_t mbox_read(uint32_t chan)
{
    volatile uint32_t *status = (volatile uint32_t *)(MBOX_BASE + MBOX_STATUS_REG);
    volatile uint32_t *read   = (volatile uint32_t *)(MBOX_BASE + MBOX_READ_REG);
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
    uint32_t gpu_addr = (arm_phys & 0x3FFFFFFFU) | 0xC0000000U;

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
    uart_puts("[VL805] Requesting firmware via mailbox 0x00030058...\n");

    static volatile uint32_t __attribute__((aligned(16))) buf[8];
    buf[0] = 8 * 4;
    buf[1] = 0;
    buf[2] = 0x00030058U;   /* NOTIFY_XHCI_RESET */
    buf[3] = 4;
    buf[4] = 4;
    buf[5] = 0;             /* PCIe function addr: 0 = auto */
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
