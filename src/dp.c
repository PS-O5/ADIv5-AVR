#include "swd.h"
#include "dp.h"

uint8_t dp_read(uint8_t addr, uint32_t *data)
{
    return swd_transfer_retry(0, 1, addr, data);
}

uint8_t dp_write(uint8_t addr, uint32_t data)
{
    return swd_transfer_retry(0, 0, addr, &data);
}

/* ORUNERRCLR | WDERRCLR | STKERRCLR | STKCMPCLR */
#define ABORT_CLEAR_ALL 0x1E

uint8_t dp_clear_errors(void)
{
    return dp_write(DP_ABORT, ABORT_CLEAR_ALL);
}

#define POWER_UP_RETRIES 32

uint8_t dp_power_up(void)
{
    uint32_t ctrlstat = CTRLSTAT_CSYSPWRUPREQ | CTRLSTAT_CDBGPWRUPREQ;
    uint8_t ack = dp_write(DP_CTRLSTAT, ctrlstat);
    if (ack != SWD_ACK_OK)
        return ack;

    uint32_t want_acks = CTRLSTAT_CSYSPWRUPACK | CTRLSTAT_CDBGPWRUPACK;

    for (uint8_t i = 0; i < POWER_UP_RETRIES; i++) {
        ack = dp_read(DP_CTRLSTAT, &ctrlstat);
        if (ack != SWD_ACK_OK)
            return ack;
        if ((ctrlstat & want_acks) == want_acks)
            return SWD_ACK_OK;
    }

    return SWD_TIMEOUT;
}
