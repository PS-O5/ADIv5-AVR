#include "swd.h"
#include "ap.h"
#include "cortex.h"
#include "dwt.h"

static uint8_t slots;

uint8_t dwt_slots(void)
{
    return slots;
}

/*
 * The DWT does nothing until TRCENA is set in DEMCR, so enable that first and
 * preserve whatever vector catch is already armed. NUMCOMP sits in the top
 * nibble of DWT_CTRL and reads as zero on a part without the unit.
 */
uint8_t dwt_init(void)
{
    uint32_t demcr = 0;
    uint8_t ack = mem_ap_read_word(DEMCR, &demcr);
    if (ack != SWD_ACK_OK)
        return ack;

    ack = mem_ap_write_word(DEMCR, demcr | DEMCR_TRCENA);
    if (ack != SWD_ACK_OK)
        return ack;

    uint32_t ctrl = 0;
    ack = mem_ap_read_word(DWT_CTRL, &ctrl);
    if (ack != SWD_ACK_OK)
        return ack;

    slots = (uint8_t)((ctrl >> 28) & 0xF);
    return SWD_ACK_OK;
}

uint8_t dwt_set(uint8_t slot, uint32_t addr, uint8_t function)
{
    if (slot >= slots)
        return DWT_NO_SLOT;

    uint8_t ack = mem_ap_write_word(DWT_COMP(slot), addr);
    if (ack != SWD_ACK_OK)
        return ack;

    /* MASK is how many low address bits to ignore; zero matches exactly. */
    ack = mem_ap_write_word(DWT_MASK(slot), 0);
    if (ack != SWD_ACK_OK)
        return ack;

    return mem_ap_write_word(DWT_FUNCTION(slot), function);
}

uint8_t dwt_clear(uint8_t slot)
{
    if (slot >= slots)
        return DWT_NO_SLOT;

    return mem_ap_write_word(DWT_FUNCTION(slot), DWT_FUNC_DISABLED);
}

uint8_t dwt_get(uint8_t slot, uint32_t *addr, uint8_t *function, uint8_t *matched)
{
    if (slot >= slots)
        return DWT_NO_SLOT;

    uint8_t ack = mem_ap_read_word(DWT_COMP(slot), addr);
    if (ack != SWD_ACK_OK)
        return ack;

    uint32_t func = 0;
    ack = mem_ap_read_word(DWT_FUNCTION(slot), &func);
    if (ack != SWD_ACK_OK)
        return ack;

    *function = (uint8_t)(func & 0xF);
    *matched = (func & DWT_FUNC_MATCHED) ? 1 : 0;

    return SWD_ACK_OK;
}
