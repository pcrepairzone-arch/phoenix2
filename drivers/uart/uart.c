/*
 * uart.c - PL011 UART Driver for Raspberry Pi 4/5
 */

#include "kernel.h"
#include <stdint.h>

#define UART0_BASE      0xFE201000ULL
#define UART_DR         (UART0_BASE + 0x00)
#define UART_FR         (UART0_BASE + 0x18)
#define UART_IBRD       (UART0_BASE + 0x24)
#define UART_FBRD       (UART0_BASE + 0x28)
#define UART_LCRH       (UART0_BASE + 0x2C)
#define UART_CR         (UART0_BASE + 0x30)
#define UART_ICR        (UART0_BASE + 0x44)
#define UART_FR_TXFF    (1 << 5)
#define UART_FR_RXFE    (1 << 4)

#define GPIO_BASE       0xFE200000ULL
#define GPFSEL1         (GPIO_BASE + 0x04)
#define GPPUD           (GPIO_BASE + 0x94)
#define GPPUDCLK0       (GPIO_BASE + 0x98)

static inline void mmio_write(uint64_t reg, uint32_t data) {
    *(volatile uint32_t *)reg = data;
}

static inline uint32_t mmio_read(uint64_t reg) {
    return *(volatile uint32_t *)reg;
}

static void uart_delay(int32_t count) {
    while (count-- > 0) __asm__ volatile("nop");
}

void uart_init(void) {
    mmio_write(UART_CR, 0);
    
    uint32_t selector = mmio_read(GPFSEL1);
    selector &= ~((7 << 12) | (7 << 15));
    selector |= (4 << 12) | (4 << 15);
    mmio_write(GPFSEL1, selector);
    
    mmio_write(GPPUD, 0);
    uart_delay(150);
    mmio_write(GPPUDCLK0, (1 << 14) | (1 << 15));
    uart_delay(150);
    mmio_write(GPPUDCLK0, 0);
    
    mmio_write(UART_ICR, 0x7FF);
    mmio_write(UART_IBRD, 26);
    mmio_write(UART_FBRD, 3);
    mmio_write(UART_LCRH, (1 << 4) | (1 << 5) | (1 << 6));
    mmio_write(UART_CR, (1 << 0) | (1 << 8) | (1 << 9));
}

void uart_putc(char c) {
    while (mmio_read(UART_FR) & UART_FR_TXFF) __asm__ volatile("nop");
    mmio_write(UART_DR, (uint32_t)c);
}

void uart_puts(const char *str) {
    while (*str) {
        if (*str == '\n') uart_putc('\r');
        uart_putc(*str++);
    }
}

int uart_getc(void) {
    if (mmio_read(UART_FR) & UART_FR_RXFE) return -1;
    return mmio_read(UART_DR) & 0xFF;
}

void uart_hex(uint64_t value) {
    const char hex[] = "0123456789ABCDEF";
    uart_puts("0x");
    for (int i = 60; i >= 0; i -= 4) {
        uart_putc(hex[(value >> i) & 0xF]);
    }
}

void uart_dec(int64_t value) {
    if (value < 0) { uart_putc('-'); value = -value; }
    if (value == 0) { uart_putc('0'); return; }
    
    char buf[32];
    int i = 0;
    while (value > 0) { buf[i++] = '0' + (value % 10); value /= 10; }
    while (i > 0) uart_putc(buf[--i]);
}
