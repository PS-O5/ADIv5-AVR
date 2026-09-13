#include <avr/io.h>
#include <util/delay.h>
#include "swd.h"

#define SWCLK_BIT PB0
#define SWDIO_BIT PB1
#define NRST_BIT  PB2

/* Half-period in ~250ns loop iterations. 0 runs at the bit-bang loop's own rate. */
static uint8_t half_period = 0;

void swd_set_speed(uint8_t loops)
{
    half_period = loops;
}

static inline void swd_delay(void)
{
    for (uint8_t i = 0; i < half_period; i++)
        __asm__ __volatile__("nop");
}

static inline void clock_low(void)
{
    PORTB &= ~(1 << SWCLK_BIT);
    swd_delay();
}

static inline void clock_high(void)
{
    PORTB |= (1 << SWCLK_BIT);
    swd_delay();
}

static inline void swdio_drive_low(void)
{
    DDRB |= (1 << SWDIO_BIT);
    PORTB &= ~(1 << SWDIO_BIT);
}

static inline void swdio_listen(void)
{
    DDRB &= ~(1 << SWDIO_BIT);
    PORTB &= ~(1 << SWDIO_BIT);
}

/*
 * NRST is only ever pulled low or released, never driven high. The pin is
 * bidirectional: the reset button and the target's own reset sources pull it
 * low too, so driving it high would mean driving into a short. The internal
 * pull-up restores the high level, which is why this needs no resistor.
 */
void swd_reset_assert(void)
{
    DDRB |= (1 << NRST_BIT);
    PORTB &= ~(1 << NRST_BIT);
}

void swd_reset_release(void)
{
    DDRB &= ~(1 << NRST_BIT);
    PORTB &= ~(1 << NRST_BIT);
}

void swd_init(void)
{
    DDRB |= (1 << SWCLK_BIT);
    PORTB &= ~(1 << SWCLK_BIT);
    swdio_listen();
    swd_reset_release();
}

void swd_write_bit(uint8_t bit)
{
    if (bit)
        swdio_listen();
    else
        swdio_drive_low();

    clock_high();
    clock_low();
}

uint8_t swd_read_bit(void)
{
    clock_high();
    uint8_t bit = (PINB >> SWDIO_BIT) & 1;
    clock_low();
    return bit;
}

/*
 * Target-to-host handoff. One clock is the protocol turnaround, the second
 * covers the phase offset between the target releasing SWDIO and this loop
 * driving it. With only one, the target flags WDATAERR on the write data
 * phase; with three it does too. The host-to-target direction needs no extra
 * clock, since the park bit already leaves the line undriven.
 */
#define TURNAROUND_CYCLES 2

void swd_turnaround_to_host(void)
{
    for (uint8_t i = 0; i < TURNAROUND_CYCLES; i++) {
        clock_high();
        clock_low();
    }
}

#define JTAG_TO_SWD_SEQUENCE 0xE79E

static void swd_line_reset(void)
{
    for (uint8_t i = 0; i < 56; i++)
        swd_write_bit(1);
}

/*
 * Driven-low idle cycles. The host never drives SWDIO high (it releases and
 * lets the pull-up work), so the target can only frame a start bit if the line
 * was actively low beforehand.
 */
void swd_idle(void)
{
    for (uint8_t i = 0; i < 8; i++)
        swd_write_bit(0);
}

void swd_connect(void)
{
    swd_idle();
    swd_line_reset();

    for (uint8_t i = 0; i < 16; i++)
        swd_write_bit((JTAG_TO_SWD_SEQUENCE >> i) & 1);

    swd_line_reset();
    swd_idle();
}

static uint8_t parity_of(uint32_t value, uint8_t bits)
{
    uint8_t parity = 0;
    for (uint8_t i = 0; i < bits; i++)
        parity ^= (value >> i) & 1;
    return parity;
}

uint8_t swd_transfer(uint8_t ap_ndp, uint8_t read, uint8_t addr, uint32_t *data)
{
    uint8_t a2 = (addr >> 2) & 1;
    uint8_t a3 = (addr >> 3) & 1;
    uint8_t request_parity = ap_ndp ^ read ^ a2 ^ a3;

    uint8_t request = 1
        | (ap_ndp << 1)
        | (read << 2)
        | (a2 << 3)
        | (a3 << 4)
        | (request_parity << 5)
        | (0 << 6)
        | (1 << 7);

    for (uint8_t i = 0; i < 8; i++)
        swd_write_bit((request >> i) & 1);

    /* The park bit released the line; the target drives ACK on the next clock. */
    uint8_t ack = 0;
    for (uint8_t i = 0; i < 3; i++)
        ack |= swd_read_bit() << i;

    if (ack != SWD_ACK_OK) {
        swd_turnaround_to_host();
        swd_idle();
        return ack;
    }

    if (read) {
        uint32_t value = 0;
        for (uint8_t i = 0; i < 32; i++)
            value |= (uint32_t)swd_read_bit() << i;
        uint8_t received_parity = swd_read_bit();
        swd_turnaround_to_host();
        swd_idle();

        if (received_parity != parity_of(value, 32))
            return SWD_PARITY_ERROR;

        *data = value;
    } else {
        swd_turnaround_to_host();
        for (uint8_t i = 0; i < 32; i++)
            swd_write_bit((*data >> i) & 1);
        swd_write_bit(parity_of(*data, 32));
        swd_idle();
    }

    return ack;
}

#define SWD_WAIT_RETRIES 16

uint8_t swd_transfer_retry(uint8_t ap_ndp, uint8_t read, uint8_t addr, uint32_t *data)
{
    uint8_t ack;

    for (uint8_t i = 0; i < SWD_WAIT_RETRIES; i++) {
        ack = swd_transfer(ap_ndp, read, addr, data);
        if (ack != SWD_ACK_WAIT)
            break;
    }

    return ack;
}
