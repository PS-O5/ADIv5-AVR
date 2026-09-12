#include "swd.h"
#include "ap.h"
#include "flash.h"

#define BSY_RETRIES 200

uint8_t flash_read_sr(uint32_t *sr)
{
    return mem_ap_read_word(FLASH_SR, sr);
}

static uint8_t flash_wait_busy(uint32_t *sr)
{
    for (uint16_t i = 0; i < BSY_RETRIES; i++) {
        uint8_t ack = flash_read_sr(sr);
        if (ack != SWD_ACK_OK)
            return ack;
        if (!(*sr & FLASH_SR_BSY))
            return SWD_ACK_OK;
    }

    return SWD_TIMEOUT;
}

/* Clears the sticky status flags, which are write-1-to-clear. */
static uint8_t flash_clear_sr(void)
{
    return mem_ap_write_word(FLASH_SR, FLASH_SR_EOP | FLASH_SR_ERRORS);
}

/*
 * Writes to FLASH_CR are ignored while LOCK is set, so an operation started on
 * a locked controller never runs, never sets BSY and never reports an error.
 * Check first rather than succeeding at nothing.
 */
static uint8_t flash_check_unlocked(void)
{
    uint32_t cr = 0;
    uint8_t ack = mem_ap_read_word(FLASH_CR, &cr);
    if (ack != SWD_ACK_OK)
        return ack;

    return (cr & FLASH_CR_LOCK) ? FLASH_LOCKED : SWD_ACK_OK;
}

uint8_t flash_unlock(void)
{
    uint8_t ack = mem_ap_write_word(FLASH_KEYR, FLASH_KEY1);
    if (ack != SWD_ACK_OK)
        return ack;

    ack = mem_ap_write_word(FLASH_KEYR, FLASH_KEY2);
    if (ack != SWD_ACK_OK)
        return ack;

    return flash_check_unlocked();
}

uint8_t flash_lock(void)
{
    return mem_ap_write_word(FLASH_CR, FLASH_CR_LOCK);
}

/* Drops PG/SER and reports whatever went wrong first. */
static uint8_t flash_finish(uint8_t ack, uint32_t sr)
{
    uint32_t final_sr = sr;
    uint8_t wait_ack = flash_wait_busy(&final_sr);
    uint8_t cr_ack = mem_ap_write_word(FLASH_CR, 0);

    if (ack != SWD_ACK_OK)
        return ack;
    if (wait_ack != SWD_ACK_OK)
        return wait_ack;
    if (cr_ack != SWD_ACK_OK)
        return cr_ack;

    return (final_sr & FLASH_SR_ERRORS) ? FLASH_ERR : SWD_ACK_OK;
}

uint8_t flash_program_word(uint32_t addr, uint32_t value)
{
    uint32_t sr = 0;
    uint8_t ack = flash_wait_busy(&sr);
    if (ack != SWD_ACK_OK)
        return ack;

    ack = flash_check_unlocked();
    if (ack != SWD_ACK_OK)
        return ack;

    ack = flash_clear_sr();
    if (ack != SWD_ACK_OK)
        return ack;

    ack = mem_ap_write_word(FLASH_CR, FLASH_CR_PSIZE_X32 | FLASH_CR_PG);
    if (ack != SWD_ACK_OK)
        return ack;

    /* The controller intercepts this bus write and programs the word. */
    ack = mem_ap_write_word(addr, value);

    return flash_finish(ack, sr);
}

/* TAR auto-increment is only guaranteed across the bottom 10 address bits. */
#define TAR_BOUNDARY 1024UL

uint8_t flash_write(uint32_t addr, const uint32_t *words, uint16_t count)
{
    uint32_t sr = 0;
    uint8_t ack = flash_wait_busy(&sr);
    if (ack != SWD_ACK_OK)
        return ack;

    ack = flash_check_unlocked();
    if (ack != SWD_ACK_OK)
        return ack;

    ack = flash_clear_sr();
    if (ack != SWD_ACK_OK)
        return ack;

    ack = mem_ap_write_word(FLASH_CR, FLASH_CR_PSIZE_X32 | FLASH_CR_PG);
    if (ack != SWD_ACK_OK)
        return ack;

    ack = ap_select(0, 0x0);

    for (uint16_t i = 0; i < count && ack == SWD_ACK_OK; i++) {
        uint32_t target = addr + 4UL * i;

        if (i == 0 || (target & (TAR_BOUNDARY - 1)) == 0)
            ack = ap_write(AP_TAR, target);

        if (ack == SWD_ACK_OK)
            ack = ap_write(AP_DRW, words[i]);
    }

    return flash_finish(ack, sr);
}

uint8_t flash_erase_sector(uint8_t sector)
{
    uint32_t sr = 0;
    uint8_t ack = flash_wait_busy(&sr);
    if (ack != SWD_ACK_OK)
        return ack;

    ack = flash_check_unlocked();
    if (ack != SWD_ACK_OK)
        return ack;

    ack = flash_clear_sr();
    if (ack != SWD_ACK_OK)
        return ack;

    uint32_t cr = FLASH_CR_PSIZE_X32 | FLASH_CR_SER
                | ((uint32_t)sector << FLASH_CR_SNB_SHIFT);

    ack = mem_ap_write_word(FLASH_CR, cr);
    if (ack != SWD_ACK_OK)
        return ack;

    ack = mem_ap_write_word(FLASH_CR, cr | FLASH_CR_STRT);

    return flash_finish(ack, sr);
}
