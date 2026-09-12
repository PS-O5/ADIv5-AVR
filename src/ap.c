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

/* Tracked so the common word path does not rewrite CSW on every access. */
static uint8_t current_size = 0xFF;

uint8_t mem_ap_init(void)
{
    current_size = 0xFF;

    uint8_t ack = ap_select(0, 0x0);
    if (ack != SWD_ACK_OK)
        return ack;

    uint32_t csw = CSW_PROT_DEBUG | CSW_SIZE_WORD | CSW_ADDRINC_SINGLE;
    ack = ap_write(AP_CSW, csw);
    if (ack == SWD_ACK_OK)
        current_size = CSW_SIZE_WORD;

    return ack;
}

/*
 * Size is allowed to be read-only, in which case an unsupported value simply
 * does not take. Read it back rather than trusting the write.
 */
static uint8_t ensure_size(uint8_t size)
{
    if (current_size == size)
        return SWD_ACK_OK;

    uint8_t ack = ap_write(AP_CSW, CSW_PROT_DEBUG | CSW_ADDRINC_SINGLE | size);
    if (ack != SWD_ACK_OK)
        return ack;

    uint32_t got = 0;
    ack = ap_read(AP_CSW, &got);
    if (ack != SWD_ACK_OK)
        return ack;

    if ((got & 0x7) != size)
        return MEM_AP_BAD_SIZE;

    current_size = size;
    return SWD_ACK_OK;
}

static uint8_t read_word_once(uint32_t addr, uint32_t *value)
{
    uint8_t ack = ap_select(0, 0x0);
    if (ack != SWD_ACK_OK)
        return ack;

    ack = ensure_size(CSW_SIZE_WORD);
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

    ack = ensure_size(CSW_SIZE_WORD);
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

/*
 * Sub-word data rides in the DRW lane matching the address, so a byte at
 * ...03 arrives in bits [31:24] rather than at the bottom. Little-endian
 * target, per the byte lane tables in ADIv5.
 */
static uint8_t sub_access(uint32_t addr, uint8_t size, uint32_t *value, uint8_t write)
{
    uint8_t ack = ap_select(0, 0x0);
    if (ack != SWD_ACK_OK)
        return ack;

    ack = ensure_size(size);
    if (ack != SWD_ACK_OK)
        return ack;

    ack = ap_write(AP_TAR, addr);
    if (ack != SWD_ACK_OK)
        return ack;

    ack = write ? ap_write(AP_DRW, *value) : ap_read(AP_DRW, value);

    if (ack == SWD_ACK_FAULT)
        dp_clear_errors();

    return ack;
}

uint8_t mem_ap_read8(uint32_t addr, uint8_t *value)
{
    uint32_t raw = 0;
    uint8_t ack = sub_access(addr, CSW_SIZE_BYTE, &raw, 0);
    if (ack != SWD_ACK_OK)
        return ack;

    *value = (uint8_t)(raw >> (8 * (addr & 3)));
    return ack;
}

uint8_t mem_ap_write8(uint32_t addr, uint8_t value)
{
    uint32_t raw = (uint32_t)value << (8 * (addr & 3));
    return sub_access(addr, CSW_SIZE_BYTE, &raw, 1);
}

uint8_t mem_ap_read16(uint32_t addr, uint16_t *value)
{
    uint32_t raw = 0;
    uint8_t ack = sub_access(addr, CSW_SIZE_HALF, &raw, 0);
    if (ack != SWD_ACK_OK)
        return ack;

    *value = (uint16_t)(raw >> (8 * (addr & 2)));
    return ack;
}

uint8_t mem_ap_write16(uint32_t addr, uint16_t value)
{
    uint32_t raw = (uint32_t)value << (8 * (addr & 2));
    return sub_access(addr, CSW_SIZE_HALF, &raw, 1);
}
