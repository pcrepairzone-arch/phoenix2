#ifndef UART_H
#define UART_H
#include <stdint.h>
void uart_init(void);
void uart_putc(char c);
void uart_puts(const char *str);
int uart_getc(void);
void uart_hex(uint64_t value);
void uart_dec(int64_t value);
#endif
