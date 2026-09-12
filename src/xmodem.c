#include "swd.h"
#include "flash.h"
#include "uart.h"
#include "xmodem.h"

#define SOH 0x01
#define EOT 0x04
#define ACK 0x06
#define NAK 0x15
#define CAN 0x18

#define BLOCK 128
#define WORDS (BLOCK / 4)

/* One NAK per second, so this is also how long the sender has to get going. */
#define START_RETRIES 60
#define BLOCK_TIMEOUT_MS 1000

static uint32_t words[WORDS];

xmodem_stats xm_stats;

/* Little-endian target, so file bytes map straight onto memory. */
static void pack(const uint8_t *bytes)
{
    for (uint8_t i = 0; i < WORDS; i++)
        words[i] = (uint32_t)bytes[i * 4]
                 | ((uint32_t)bytes[i * 4 + 1] << 8)
                 | ((uint32_t)bytes[i * 4 + 2] << 16)
                 | ((uint32_t)bytes[i * 4 + 3] << 24);
}

uint8_t xmodem_receive_to_flash(uint32_t addr, uint32_t *bytes_written)
{
    uint8_t data[BLOCK];
    uint8_t expected = 1;

    *bytes_written = 0;

    xm_stats.naks_sent = 0;
    xm_stats.blocks_ok = 0;
    xm_stats.bad_checksum = 0;
    xm_stats.bad_blocknum = 0;
    xm_stats.resyncs = 0;
    xm_stats.first_byte = -1;
    xm_stats.last_blk = -1;
    xm_stats.last_inv = -1;

    /* Checksum mode: NAK until the sender starts, or it gives up. */
    for (uint8_t i = 0; i < START_RETRIES; i++) {
        uart_putc(NAK);
        xm_stats.naks_sent++;

        int16_t c = uart_getc_timeout(BLOCK_TIMEOUT_MS);

        if (xm_stats.first_byte < 0 && c >= 0)
            xm_stats.first_byte = c;
        if (c == SOH || c == EOT) {
            /* Fall into the main loop with this byte already in hand. */
            for (;;) {
                if (c == EOT) {
                    uart_putc(ACK);
                    return XMODEM_OK;
                }

                if (c == CAN)
                    return XMODEM_CANCELED;

                if (c != SOH) {
                    xm_stats.resyncs++;
                    c = uart_getc_timeout(BLOCK_TIMEOUT_MS);
                    if (c < 0)
                        return XMODEM_TIMEOUT;
                    continue;
                }

                int16_t blk = uart_getc_timeout(BLOCK_TIMEOUT_MS);
                int16_t inv = uart_getc_timeout(BLOCK_TIMEOUT_MS);
                xm_stats.last_blk = blk;
                xm_stats.last_inv = inv;
                if (blk < 0 || inv < 0)
                    return XMODEM_TIMEOUT;

                uint8_t sum = 0;
                for (uint16_t j = 0; j < BLOCK; j++) {
                    int16_t b = uart_getc_timeout(BLOCK_TIMEOUT_MS);
                    if (b < 0)
                        return XMODEM_TIMEOUT;
                    data[j] = (uint8_t)b;
                    sum += (uint8_t)b;
                }

                int16_t their_sum = uart_getc_timeout(BLOCK_TIMEOUT_MS);
                if (their_sum < 0)
                    return XMODEM_TIMEOUT;

                uint8_t good = (blk == (255 - inv)) && ((uint8_t)their_sum == sum);

                if (!good) {
                    if (blk != (255 - inv))
                        xm_stats.bad_blocknum++;
                    else
                        xm_stats.bad_checksum++;
                }

                if (good && (uint8_t)blk == expected) {
                    xm_stats.blocks_ok++;
                    pack(data);
                    if (flash_write(addr, words, WORDS) != SWD_ACK_OK) {
                        uart_putc(CAN);
                        return XMODEM_FLASHERR;
                    }
                    addr += BLOCK;
                    *bytes_written += BLOCK;
                    expected++;
                    uart_putc(ACK);
                } else if (good && (uint8_t)blk == (uint8_t)(expected - 1)) {
                    /* Sender missed our ACK and resent; acknowledge again. */
                    uart_putc(ACK);
                } else {
                    uart_putc(NAK);
                }

                c = uart_getc_timeout(BLOCK_TIMEOUT_MS);
                if (c < 0)
                    return XMODEM_TIMEOUT;
            }
        }
    }

    return XMODEM_TIMEOUT;
}
