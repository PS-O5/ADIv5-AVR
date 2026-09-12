#include <avr/io.h>
#include "swd.h"
#include "dp.h"
#include "ap.h"
#include "cortex.h"
#include "flash.h"
#include "uart.h"

#define LED_BIT PB5
#define TEST_ADDR 0x08000000UL

static void show(const char *label, uint8_t ack, uint32_t value)
{
    uart_puts(label);
    uart_puts(" ack=0x");
    uart_print_hex8(ack);
    uart_puts(" = 0x");
    uart_print_hex32(value);
    uart_puts("\r\n");
}

static void dump(const char *label, uint32_t addr)
{
    uint32_t value = 0;
    uint8_t ack = mem_ap_read_word(addr, &value);
    show(label, ack, value);
}

static void step(const char *label, uint8_t ack)
{
    uart_puts(label);
    uart_puts(" ack=0x");
    uart_print_hex8(ack);
    uart_puts("\r\n");
}

int main(void)
{
    DDRB |= (1 << LED_BIT);
    uart_init();

    uart_puts("\r\n--- ADIv5-AVR flash ---\r\n");

    swd_init();
    swd_connect();

    uint32_t idcode = 0;
    step("IDCODE  ", dp_read(DP_DPIDR, &idcode));
    dp_power_up();
    dp_clear_errors();
    step("MEM-AP  ", mem_ap_init());
    step("rst+halt", cortex_reset_halt());

    uart_puts("\r\n");
    dump("before  ", TEST_ADDR);
    dump("before+4", TEST_ADDR + 4);

    uart_puts("\r\n");
    step("unlock  ", flash_unlock());
    step("write0  ", flash_program_word(TEST_ADDR, 0xDEADBEEFUL));
    step("write1  ", flash_program_word(TEST_ADDR + 4, 0xCAFEBABEUL));

    uint32_t sr = 0;
    show("FLASH_SR", flash_read_sr(&sr), sr);

    uart_puts("\r\n");
    dump("after   ", TEST_ADDR);
    dump("after+4 ", TEST_ADDR + 4);

    uart_puts("\r\n");
    step("erase s0", flash_erase_sector(0));
    dump("erased  ", TEST_ADDR);
    dump("erased+4", TEST_ADDR + 4);

    step("lock    ", flash_lock());

    uart_puts("--- done ---\r\n");

    PORTB |= (1 << LED_BIT);

    for (;;)
        ;
}
