#include <avr/io.h>
#include "swd.h"
#include "dp.h"
#include "ap.h"
#include "uart.h"

#define LED_BIT PB5

static void report(const char *label, uint8_t ack, uint32_t data)
{
    uart_puts(label);
    uart_puts(" ack=0x");
    uart_print_hex8(ack);
    uart_puts(" data=0x");
    uart_print_hex32(data);
    uart_puts("\r\n");
}

int main(void)
{
    DDRB |= (1 << LED_BIT);
    uart_init();

    uart_puts("\r\n--- ADIv5-AVR ---\r\n");

    swd_init();
    swd_connect();

    uint32_t idcode = 0;
    uint8_t ack = dp_read(DP_DPIDR, &idcode);
    report("IDCODE   ", ack, idcode);

    ack = dp_power_up();
    uart_puts("PowerUp   ack=0x");
    uart_print_hex8(ack);
    uart_puts("\r\n");

    uint32_t ctrlstat = 0;
    ack = dp_read(DP_CTRLSTAT, &ctrlstat);
    report("CTRL/STAT", ack, ctrlstat);

    ack = mem_ap_init();
    uart_puts("CSW write ack=0x");
    uart_print_hex8(ack);
    uart_puts("\r\n");

    ack = ap_select(0, 0xF);
    uart_puts("AP select ack=0x");
    uart_print_hex8(ack);
    uart_puts("\r\n");

    uint32_t ap_idr = 0;
    ack = ap_read(AP_IDR, &ap_idr);
    report("AP IDR   ", ack, ap_idr);

    uart_puts("--- done ---\r\n");

    PORTB |= (1 << LED_BIT);

    for (;;)
        ;
}
