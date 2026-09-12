#include <avr/io.h>
#include <avr/interrupt.h>
#include <util/delay.h>
#include "uart.h"

#define BAUD 115200UL
#define UBRR_VALUE ((F_CPU / (8UL * BAUD)) - 1)

/*
 * Receive ring. Without it anything arriving while the firmware is busy is
 * lost: the hardware holds two bytes and there is nothing to move them. That
 * silently drops commands sent by a script rather than typed, and the symptom
 * is a later operation failing for no visible reason.
 */
#define RX_SIZE 64

static volatile uint8_t rx_buf[RX_SIZE];
static volatile uint8_t rx_head;
static volatile uint8_t rx_tail;

ISR(USART_RX_vect)
{
    uint8_t c = UDR0;
    uint8_t next = (uint8_t)((rx_head + 1) & (RX_SIZE - 1));

    /* Drop on overflow rather than overwrite what has not been read yet. */
    if (next != rx_tail) {
        rx_buf[rx_head] = c;
        rx_head = next;
    }
}

void uart_init(void)
{
    UCSR0A = (1 << U2X0);
    UBRR0H = (uint8_t)(UBRR_VALUE >> 8);
    UBRR0L = (uint8_t)UBRR_VALUE;
    UCSR0B = (1 << TXEN0) | (1 << RXEN0) | (1 << RXCIE0);
    UCSR0C = (1 << UCSZ01) | (1 << UCSZ00);
}

void uart_putc(char c)
{
    while (!(UCSR0A & (1 << UDRE0)))
        ;
    UDR0 = c;
}

uint8_t uart_available(void)
{
    return rx_head != rx_tail;
}

static char rx_take(void)
{
    char c = (char)rx_buf[rx_tail];
    rx_tail = (uint8_t)((rx_tail + 1) & (RX_SIZE - 1));
    return c;
}

char uart_getc(void)
{
    while (rx_head == rx_tail)
        ;
    return rx_take();
}

int16_t uart_getc_timeout(uint16_t ms)
{
    while (ms--) {
        for (uint8_t i = 0; i < 10; i++) {
            if (rx_head != rx_tail)
                return (uint8_t)rx_take();
            _delay_us(100);
        }
    }

    return -1;
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

void uart_print_dec(uint32_t value)
{
    char buf[10];
    uint8_t n = 0;

    if (value == 0) {
        uart_putc('0');
        return;
    }

    while (value) {
        buf[n++] = '0' + (value % 10);
        value /= 10;
    }

    while (n)
        uart_putc(buf[--n]);
}

void uart_print_hex32(uint32_t value)
{
    for (int8_t shift = 24; shift >= 0; shift -= 8)
        uart_print_hex8((uint8_t)(value >> shift));
}
