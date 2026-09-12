#include <avr/io.h>
#include "swd.h"
#include "dp.h"
#include "ap.h"
#include "cortex.h"
#include "flash.h"
#include "uart.h"

#define LED_BIT PB5
#define TEST_ADDR 0x08000000UL

/* Timer1 at /1024: 15625 ticks per second, 64us per tick. */
static void timer_start(void)
{
    TCCR1A = 0;
    TCCR1B = (1 << CS12) | (1 << CS10);
    TCNT1 = 0;
}

static uint32_t timer_ms(void)
{
    return ((uint32_t)TCNT1 * 64UL) / 1000UL;
}

#define WORDS 64
static uint32_t buf[WORDS];

static void step(const char *label, uint8_t ack)
{
    uart_puts(label);
    uart_puts(" ack=0x");
    uart_print_hex8(ack);
    uart_puts("\r\n");
}

static uint8_t verify(uint32_t addr)
{
    for (uint16_t i = 0; i < WORDS; i++) {
        uint32_t got = 0;
        if (mem_ap_read_word(addr + 4UL * i, &got) != SWD_ACK_OK)
            return 0;
        if (got != buf[i])
            return 0;
    }
    return 1;
}

static void bulk(const char *label, uint32_t addr)
{
    for (uint16_t i = 0; i < WORDS; i++)
        buf[i] = 0xA5A50000UL | (uint32_t)i;

    timer_start();
    uint8_t ack = flash_write(addr, buf, WORDS);
    uint32_t ms = timer_ms();

    uart_puts(label);
    uart_puts(" ack=0x");
    uart_print_hex8(ack);
    uart_puts(" ");
    uart_print_dec(WORDS * 4UL);
    uart_puts(" bytes in ");
    uart_print_dec(ms);
    uart_puts(" ms  verify ");
    uart_puts(verify(addr) ? "OK" : "FAILED");
    uart_puts("\r\n");
}

int main(void)
{
    DDRB |= (1 << LED_BIT);
    uart_init();

    uart_puts("\r\n--- ADIv5-AVR bulk flash, max speed ---\r\n");

    swd_set_speed(0);
    swd_init();

    uint32_t idcode = 0;
    step("connect ", dp_connect(&idcode));
    uart_puts("IDCODE = 0x");
    uart_print_hex32(idcode);
    uart_puts("\r\n");

    dp_power_up();
    dp_clear_errors();
    step("MEM-AP  ", mem_ap_init());
    step("rst+halt", cortex_reset_halt());
    step("unlock  ", flash_unlock());
    step("erase s0", flash_erase_sector(0));

    uart_puts("\r\n");
    bulk("plain      ", TEST_ADDR);
    bulk("across 1KB ", TEST_ADDR + 0x3F0UL);

    uart_puts("\r\n");
    step("erase s0", flash_erase_sector(0));
    step("lock    ", flash_lock());

    uart_puts("--- done ---\r\n");

    PORTB |= (1 << LED_BIT);

    for (;;)
        ;
}
