#include <avr/io.h>
#include <avr/interrupt.h>
#include "swd.h"
#include "uart.h"
#include "shell.h"

#define LED_BIT PB5

int main(void)
{
    DDRB |= (1 << LED_BIT);

    uart_init();
    sei();  /* the receive ring only fills with interrupts on */

    swd_init();

    PORTB |= (1 << LED_BIT);

    shell_run();
}
