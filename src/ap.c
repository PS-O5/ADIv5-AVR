#include "swd.h"
#include "dp.h"
#include "ap.h"

uint8_t ap_select(uint8_t ap_index, uint8_t bank)
{
    uint32_t select = ((uint32_t)ap_index << 24) | ((uint32_t)(bank & 0xF) << 4);
    return dp_write(DP_SELECT, select);
}

uint8_t ap_read(uint8_t addr, uint32_t *data)
{
    uint32_t discard;
    uint8_t ack = swd_transfer_retry(1, 1, addr, &discard);
    if (ack != SWD_ACK_OK)
        return ack;

    return dp_read(DP_RDBUFF, data);
}

uint8_t ap_write(uint8_t addr, uint32_t data)
{
    return swd_transfer_retry(1, 0, addr, &data);
}

uint8_t mem_ap_init(void)
{
    uint8_t ack = ap_select(0, 0x0);
    if (ack != SWD_ACK_OK)
        return ack;

    uint32_t csw = CSW_PROT_DEBUG | CSW_SIZE_WORD | CSW_ADDRINC_SINGLE;
    return ap_write(AP_CSW, csw);
}

static uint8_t read_word_once(uint32_t addr, uint32_t *value)
{
    uint8_t ack = ap_select(0, 0x0);
    if (ack != SWD_ACK_OK)
        return ack;

    ack = ap_write(AP_TAR, addr);
    if (ack != SWD_ACK_OK)
        return ack;

    return ap_read(AP_DRW, value);
}

static uint8_t write_word_once(uint32_t addr, uint32_t value)
{
    uint8_t ack = ap_select(0, 0x0);
    if (ack != SWD_ACK_OK)
        return ack;

    ack = ap_write(AP_TAR, addr);
    if (ack != SWD_ACK_OK)
        return ack;

    return ap_write(AP_DRW, value);
}

/*
 * A bus fault, from a bad address say, latches STICKYERR, and after that every
 * AP transaction is discarded until it is cleared. Clear it on the way out so
 * one bad access fails on its own rather than taking the session with it.
 */
uint8_t mem_ap_read_word(uint32_t addr, uint32_t *value)
{
    uint8_t ack = read_word_once(addr, value);

    if (ack == SWD_ACK_FAULT)
        dp_clear_errors();

    return ack;
}

uint8_t mem_ap_write_word(uint32_t addr, uint32_t value)
{
    uint8_t ack = write_word_once(addr, value);

    if (ack == SWD_ACK_FAULT)
        dp_clear_errors();

    return ack;
}

/*
 * AP reads are posted, so a DRW read returns the previous access and starts the
 * next. Priming once and collecting the final value from RDBUFF gets this to
 * roughly one transaction per word instead of three.
 */
static uint8_t read_run(uint32_t addr, uint32_t *out, uint16_t count)
{
    uint8_t ack = ap_write(AP_TAR, addr);
    if (ack != SWD_ACK_OK)
        return ack;

    uint32_t discard = 0;
    ack = swd_transfer_retry(1, 1, AP_DRW, &discard);
    if (ack != SWD_ACK_OK)
        return ack;

    for (uint16_t i = 0; i + 1 < count; i++) {
        ack = swd_transfer_retry(1, 1, AP_DRW, &out[i]);
        if (ack != SWD_ACK_OK)
            return ack;
    }

    return dp_read(DP_RDBUFF, &out[count - 1]);
}

uint8_t mem_ap_read_block(uint32_t addr, uint32_t *out, uint16_t count)
{
    uint8_t ack = ap_select(0, 0x0);
    if (ack != SWD_ACK_OK)
        return ack;

    uint16_t done = 0;

    while (done < count) {
        uint32_t target = addr + 4UL * done;
        uint16_t room = (uint16_t)((TAR_BOUNDARY - (target & (TAR_BOUNDARY - 1))) / 4);
        uint16_t run = count - done;

        if (run > room)
            run = room;

        ack = read_run(target, out + done, run);
        if (ack != SWD_ACK_OK) {
            if (ack == SWD_ACK_FAULT)
                dp_clear_errors();
            return ack;
        }

        done += run;
    }

    return SWD_ACK_OK;
}
