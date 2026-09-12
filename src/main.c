#include <avr/io.h>
#include "swd.h"
#include "dp.h"
#include "ap.h"
#include "uart.h"

#define LED_BIT PB5

#define DHCSR     0xE000EDF0UL
#define DBGKEY    0xA05F0000UL  /* writes without this key are ignored */
#define C_DEBUGEN (1UL << 0)
#define C_HALT    (1UL << 1)
#define S_HALT    (1UL << 17)

static void show(const char *label, uint8_t ack, uint32_t value)
{
    uart_puts(label);
    uart_puts(" ack=0x");
    uart_print_hex8(ack);
    uart_puts(" = 0x");
    uart_print_hex32(value);
    uart_puts("\r\n");
}

static void read_word(const char *label, uint32_t addr)
{
    uint32_t value = 0;
    uint8_t ack = mem_ap_read_word(addr, &value);
    show(label, ack, value);

    if (ack != SWD_ACK_OK) {
        uint32_t ctrlstat = 0;
        dp_read(DP_CTRLSTAT, &ctrlstat);
        uart_puts("   CTRL/STAT=0x");
        uart_print_hex32(ctrlstat);
        uart_puts("\r\n");
        dp_clear_errors();
    }
}

int main(void)
{
    DDRB |= (1 << LED_BIT);
    uart_init();

    uart_puts("\r\n--- ADIv5-AVR memory read ---\r\n");

    swd_init();
    swd_connect();

    uint32_t idcode = 0;
    uint8_t ack = dp_read(DP_DPIDR, &idcode);
    show("IDCODE ", ack, idcode);

    ack = dp_power_up();
    uart_puts("PowerUp ack=0x");
    uart_print_hex8(ack);
    uart_puts("\r\n");

    ack = dp_clear_errors();
    uart_puts("ClrErr  ack=0x");
    uart_print_hex8(ack);
    uart_puts("\r\n");

    ack = mem_ap_init();
    uart_puts("MEM-AP  ack=0x");
    uart_print_hex8(ack);
    uart_puts("\r\n");

    uint32_t csw = 0;
    ack = ap_read(AP_CSW, &csw);
    show("CSW    ", ack, csw);
    uart_puts("\r\n");

    read_word("CPUID  ", 0xE000ED00UL);
    read_word("DHCSR  ", 0xE000EDF0UL);
    read_word("DBGMCU ", 0xE0042000UL);
    read_word("FLASH0 ", 0x08000000UL);
    read_word("UID0   ", 0x1FFF7A10UL);

    uart_puts("\r\nhalting core\r\n");
    ack = mem_ap_write_word(DHCSR, DBGKEY | C_DEBUGEN | C_HALT);
    uart_puts("DHCSR write ack=0x");
    uart_print_hex8(ack);
    uart_puts("\r\n");

    read_word("DHCSR  ", DHCSR);

    uart_puts("--- done ---\r\n");

    PORTB |= (1 << LED_BIT);

    for (;;)
        ;
}
