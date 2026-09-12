#include "swd.h"
#include "ap.h"
#include "fpb.h"

static uint8_t slots;
static uint8_t revision;

uint8_t fpb_slots(void)
{
    return slots;
}

uint8_t fpb_revision(void)
{
    return revision;
}

/*
 * NUM_CODE is split across the register: bits [7:4] are the low nibble and
 * bits [14:12] the high three bits. ENABLE only takes if KEY is written with it.
 */
uint8_t fpb_init(void)
{
    uint32_t ctrl = 0;
    uint8_t ack = mem_ap_read_word(FP_CTRL, &ctrl);
    if (ack != SWD_ACK_OK)
        return ack;

    revision = (uint8_t)((ctrl >> 28) & 0xF);
    slots = (uint8_t)(((ctrl >> 4) & 0xF) | (((ctrl >> 12) & 0x7) << 4));

    return mem_ap_write_word(FP_CTRL, FP_CTRL_KEY | FP_CTRL_ENABLE);
}

uint8_t fpb_set(uint8_t slot, uint32_t addr)
{
    if (slot >= slots)
        return FPB_NO_SLOT;

    uint32_t comp;

    if (revision == 0) {
        /*
         * v1 matches on a word and picks a halfword with REPLACE, so it only
         * reaches the code region below 0x20000000.
         */
        if (addr >= 0x20000000UL)
            return FPB_OUT_OF_RANGE;

        uint32_t replace = (addr & 2) ? 2UL : 1UL;
        comp = (replace << 30) | (addr & 0x1FFFFFFCUL) | 1UL;
    } else {
        comp = (addr & 0xFFFFFFFEUL) | 1UL;
    }

    return mem_ap_write_word(FP_COMP0 + 4UL * slot, comp);
}

uint8_t fpb_clear(uint8_t slot)
{
    if (slot >= slots)
        return FPB_NO_SLOT;

    return mem_ap_write_word(FP_COMP0 + 4UL * slot, 0);
}

uint8_t fpb_get(uint8_t slot, uint32_t *comp, uint32_t *addr, uint8_t *enabled)
{
    if (slot >= slots)
        return FPB_NO_SLOT;

    uint8_t ack = mem_ap_read_word(FP_COMP0 + 4UL * slot, comp);
    if (ack != SWD_ACK_OK)
        return ack;

    *enabled = (*comp & 1) ? 1 : 0;

    if (revision == 0) {
        uint32_t replace = (*comp >> 30) & 3;
        *addr = (*comp & 0x1FFFFFFCUL) | ((replace == 2) ? 2UL : 0UL);
    } else {
        *addr = *comp & 0xFFFFFFFEUL;
    }

    return SWD_ACK_OK;
}
