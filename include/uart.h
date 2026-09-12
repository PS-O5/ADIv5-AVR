#ifndef UART_H
#define UART_H

#include <stdint.h>

void uart_init(void);
void uart_putc(char c);
char uart_getc(void);
int16_t uart_getc_timeout(uint16_t ms);
void uart_puts(const char *s);
void uart_print_hex32(uint32_t value);
void uart_print_dec(uint32_t value);
void uart_print_hex8(uint8_t value);

#endif
