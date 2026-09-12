#include "swd.h"
#include "ap.h"
#include "rtt.h"

/* "SEGGER RTT" padded to 16 bytes, as little-endian words. */
#define RTT_ID0 0x47474553UL
#define RTT_ID1 0x52205245UL
#define RTT_ID2 0x00005454UL

#define SCAN_WORDS 64

uint8_t rtt_find(uint32_t base, uint32_t length, uint32_t *cb_addr)
{
    uint32_t buf[SCAN_WORDS];

    /* Chunks overlap by two words so a block on the boundary is still seen. */
    for (uint32_t off = 0; off + 12 <= length; off += (SCAN_WORDS - 2) * 4UL) {
        uint16_t n = SCAN_WORDS;

        if (off + 4UL * n > length)
            n = (uint16_t)((length - off) / 4);
        if (n < 3)
            break;

        uint8_t ack = mem_ap_read_block(base + off, buf, n);
        if (ack != SWD_ACK_OK)
            return ack;

        for (uint16_t i = 0; i + 2 < n; i++) {
            if (buf[i] == RTT_ID0 && buf[i + 1] == RTT_ID1 && buf[i + 2] == RTT_ID2) {
                *cb_addr = base + off + 4UL * i;
                return SWD_ACK_OK;
            }
        }
    }

    return RTT_NOT_FOUND;
}

uint8_t rtt_up_count(uint32_t cb, uint32_t *count)
{
    return mem_ap_read_word(cb + 16, count);
}

uint8_t rtt_drain(uint32_t cb, uint8_t index, void (*sink)(char), uint16_t *bytes)
{
    uint32_t desc = cb + RTT_CB_HEADER + (uint32_t)RTT_DESC_SIZE * index;
    uint32_t fields[6];

    *bytes = 0;

    uint8_t ack = mem_ap_read_block(desc, fields, 6);
    if (ack != SWD_ACK_OK)
        return ack;

    uint32_t buffer = fields[1];
    uint32_t size = fields[2];
    uint32_t wr = fields[3];
    uint32_t rd = fields[4];

    if (size == 0 || buffer == 0 || rd == wr)
        return SWD_ACK_OK;

    if (wr >= size || rd >= size)
        return RTT_NOT_FOUND;

    while (rd != wr) {
        /* One pass to the end of the ring, then another from the start. */
        uint32_t end = (wr > rd) ? wr : size;

        while (rd < end) {
            uint8_t byte = 0;
            ack = mem_ap_read8(buffer + rd, &byte);
            if (ack != SWD_ACK_OK)
                return ack;

            sink((char)byte);
            rd++;
            (*bytes)++;
        }

        if (rd >= size)
            rd = 0;
    }

    /* The host owns RdOff; the target watches it to reclaim space. */
    return mem_ap_write_word(desc + 16, rd);
}
