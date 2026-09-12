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

    uint32_t csw = CSW_SIZE_WORD | CSW_ADDRINC_SINGLE;
    return ap_write(AP_CSW, csw);
}
