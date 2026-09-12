#ifndef XMODEM_H
#define XMODEM_H

#include <stdint.h>

#define XMODEM_OK       0
#define XMODEM_TIMEOUT  1
#define XMODEM_CANCELED 2
#define XMODEM_FLASHERR 3

/* Diagnostics, since the transfer shares the UART and cannot print as it goes. */
typedef struct {
    uint16_t naks_sent;
    uint16_t blocks_ok;
    uint16_t bad_checksum;
    uint16_t bad_blocknum;
    uint16_t resyncs;
    int16_t first_byte;
    int16_t last_blk;
    int16_t last_inv;
} xmodem_stats;

extern xmodem_stats xm_stats;

uint8_t xmodem_receive_to_flash(uint32_t addr, uint32_t *bytes_written);

#endif
