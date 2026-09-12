#include <avr/io.h>
#include "uart.h"

#define BAUD 115200UL
#define UBRR_VALUE ((F_CPU / (8UL * BAUD)) - 1)

void uart_init(void)
{
    UCSR0A = (1 << U2X0);
    UBRR0H = (uint8_t)(UBRR_VALUE >> 8);
    UBRR0L = (uint8_t)UBRR_VALUE;
    UCSR0B = (1 << TXEN0);
    UCSR0C = (1 << UCSZ01) | (1 << UCSZ00);
}

void uart_putc(char c)
{
    while (!(UCSR0A & (1 << UDRE0)))
        ;
    UDR0 = c;
}

void uart_puts(const char *s)
{
    while (*s)
        uart_putc(*s++);
}

static char nibble_to_hex(uint8_t nibble)
{
    return nibble < 10 ? '0' + nibble : 'A' + (nibble - 10);
}

void uart_print_hex8(uint8_t value)
{
    uart_putc(nibble_to_hex(value >> 4));
    uart_putc(nibble_to_hex(value & 0xF));
}

void uart_print_hex32(uint32_t value)
{
    for (int8_t shift = 24; shift >= 0; shift -= 8)
        uart_print_hex8((uint8_t)(value >> shift));
}
