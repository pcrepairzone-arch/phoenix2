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

void uart_putc(char c) {
    if (!uart_base) uart_base = get_uart_base();
    while (UART_FR(uart_base) & UART_FR_TXFF);
    UART_DR(uart_base) = (uint32_t)c;
}

void uart_puts(const char *s) { while(*s) uart_putc(*s++); }
