#include "kernel.h"
#include <stdint.h>

extern uint64_t get_uart_base(void);
extern uint64_t get_gpio_base(void);

static uint64_t uart_base = 0;

#define UART_DR(b)    (*(volatile uint32_t *)((b) + 0x00))
#define UART_FR(b)    (*(volatile uint32_t *)((b) + 0x18))
#define UART_IBRD(b)  (*(volatile uint32_t *)((b) + 0x24))
#define UART_FBRD(b)  (*(volatile uint32_t *)((b) + 0x28))
#define UART_LCRH(b)  (*(volatile uint32_t *)((b) + 0x2C))
#define UART_CR(b)    (*(volatile uint32_t *)((b) + 0x30))
#define UART_ICR(b)   (*(volatile uint32_t *)((b) + 0x44))
#define UART_FR_TXFF  (1 << 5)
#define UART_FR_TXFE  (1 << 7)   /* TX FIFO empty */
#define UART_CR_UARTEN (1 << 0)
#define UART_CR_TXE    (1 << 8)
#define UART_CR_RXE    (1 << 9)

void uart_init(void) {
    uart_base = get_uart_base();
    uint64_t gpio_base = get_gpio_base();
    
    volatile uint32_t *gpfsel1 = (volatile uint32_t *)(gpio_base + 0x04);
    volatile uint32_t *gppud   = (volatile uint32_t *)(gpio_base + 0x94);
    volatile uint32_t *gppudclk0 = (volatile uint32_t *)(gpio_base + 0x98);
    
    UART_CR(uart_base) = 0;
    
    uint32_t val = *gpfsel1;
    val &= ~((7<<12) | (7<<15));
    val |= (4<<12) | (4<<15);
    *gpfsel1 = val;
    
    *gppud = 0;
    for (int i = 0; i < 150; i++) __asm__ volatile("nop");
    *gppudclk0 = (1<<14) | (1<<15);
    for (int i = 0; i < 150; i++) __asm__ volatile("nop");
    *gppudclk0 = 0;
    
    UART_ICR(uart_base) = 0x7FF;
    UART_IBRD(uart_base) = 26;
    UART_FBRD(uart_base) = 3;
    UART_LCRH(uart_base) = (1<<4) | (3<<5);
    UART_CR(uart_base) = (1<<0) | (1<<8) | (1<<9);
}

/* boot256: quiet flag suppresses uart_puts during verbose FileCore/IDA scan.
 * uart_putc (used by debug_print via lib.c) is always live.
 * Set to 1 after USB init, back to 0 before WIMP starts.               */
volatile int g_uart_quiet = 0;

void uart_set_quiet(int q) { g_uart_quiet = q; }

/* boot271: stall counter — counts how many times uart_putc hit a stuck FIFO */
volatile uint32_t g_uart_stalls = 0;

/* uart_reinit: re-enable UART if CR was cleared (e.g. by a firmware callback).
 * Call this if TXFF is stuck and UARTEN/TXE look wrong.                      */
static void uart_reinit_if_needed(void) {
    uint32_t cr = UART_CR(uart_base);
    if (!(cr & (UART_CR_UARTEN | UART_CR_TXE))) {
        /* UART was disabled — re-enable exactly as uart_init did */
        UART_CR(uart_base) = UART_CR_UARTEN | UART_CR_TXE | UART_CR_RXE;
    }
}

void uart_putc(char c) {
    if (!uart_base) uart_base = get_uart_base();

    /* boot271: timeout-guarded TXFF busy-wait.
     * Previously this was an unconditional infinite loop — if the UART TX
     * FIFO filled and the UART clock stopped (firmware callback, clock gate,
     * etc.) the kernel would hang here permanently with no output or banner.
     * Now we time-out after ~5 ms (7.5M iterations at 1.5 GHz) and attempt
     * to recover by re-enabling the UART CR before retrying once.          */
    int timeout = 7500000;
    while ((UART_FR(uart_base) & UART_FR_TXFF) && --timeout > 0)
        ;
    if (timeout <= 0) {
        g_uart_stalls++;
        /* FIFO is stuck — try re-enabling the UART and wait once more */
        uart_reinit_if_needed();
        timeout = 7500000;
        while ((UART_FR(uart_base) & UART_FR_TXFF) && --timeout > 0)
            ;
        /* If still stuck, drop the character rather than hang forever */
        if (timeout <= 0) return;
    }
    UART_DR(uart_base) = (uint32_t)c;
}

/* uart_drain: busy-wait until the TX FIFO is fully empty.
 * Call before entering a timing-sensitive section to ensure previous
 * output has been transmitted and the FIFO is clear.                 */
void uart_drain(void) {
    if (!uart_base) return;
    int timeout = 20000000;
    while (!(UART_FR(uart_base) & UART_FR_TXFE) && --timeout > 0)
        ;
}

void uart_puts(const char *s) {
    if (g_uart_quiet) return;
    while(*s) uart_putc(*s++);
}
