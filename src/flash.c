#include <util/delay.h>
#include "swd.h"
#include "ap.h"
#include "flash.h"

/*
 * Real time, not poll counts. A retry count is a proxy for time that silently
 * changes meaning with the SWD clock, and erases take hundreds of milliseconds:
 * 250 to 400ms for a 16KB sector, longer for a 128KB one.
 */
#define BSY_PROGRAM_MS  200
#define BSY_ERASE_MS   8000

static uint16_t size_kb;
static uint8_t sectors;

/*
 * STM32F4 sectors are not uniform: four of 16KB, one of 64KB, then 128KB for
 * the rest. The part reports its own flash size, so the map can be derived
 * instead of hardcoding one device.
 */
uint8_t flash_probe(void)
{
    uint16_t kb = 0;
    uint8_t ack = mem_ap_read16(FLASH_SIZE_REG, &kb);
    if (ack != SWD_ACK_OK)
        return ack;

    size_kb = kb;

    if (kb <= 64)
        sectors = (uint8_t)(kb / 16);
    else if (kb <= 128)
        sectors = 5;
    else
        sectors = (uint8_t)(5 + (kb - 128) / 128);

    return SWD_ACK_OK;
}

uint16_t flash_size_kb(void)
{
    return size_kb;
}

uint8_t flash_sectors(void)
{
    return sectors;
}

uint32_t flash_sector_size(uint8_t sector)
{
    if (sector < 4)
        return 16UL * 1024;
    if (sector == 4)
        return 64UL * 1024;
    return 128UL * 1024;
}

uint32_t flash_sector_base(uint8_t sector)
{
    if (sector < 4)
        return FLASH_BASE_ADDR + 16UL * 1024 * sector;
    if (sector == 4)
        return FLASH_BASE_ADDR + 64UL * 1024;
    return FLASH_BASE_ADDR + 128UL * 1024 * (sector - 4);
}

uint8_t flash_sector_of(uint32_t addr, uint8_t *sector)
{
    for (uint8_t i = 0; i < sectors; i++) {
        uint32_t base = flash_sector_base(i);
        if (addr >= base && addr < base + flash_sector_size(i)) {
            *sector = i;
            return SWD_ACK_OK;
        }
    }

    return FLASH_ERR;
}

uint8_t flash_read_sr(uint32_t *sr)
{
    return mem_ap_read_word(FLASH_SR, sr);
}

static uint8_t flash_wait_busy_ms(uint32_t *sr, uint16_t timeout_ms)
{
    for (uint16_t ms = 0; ms <= timeout_ms; ms++) {
        uint8_t ack = flash_read_sr(sr);
        if (ack != SWD_ACK_OK)
            return ack;
        if (!(*sr & FLASH_SR_BSY))
            return SWD_ACK_OK;
        _delay_ms(1);
    }

    return SWD_TIMEOUT;
}

static uint8_t flash_wait_busy(uint32_t *sr)
{
    return flash_wait_busy_ms(sr, BSY_PROGRAM_MS);
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
static uint8_t flash_finish_ms(uint8_t ack, uint32_t sr, uint16_t timeout_ms)
{
    uint32_t final_sr = sr;
    uint8_t wait_ack = flash_wait_busy_ms(&final_sr, timeout_ms);
    uint8_t cr_ack = mem_ap_write_word(FLASH_CR, 0);

    if (ack != SWD_ACK_OK)
        return ack;
    if (wait_ack != SWD_ACK_OK)
        return wait_ack;
    if (cr_ack != SWD_ACK_OK)
        return cr_ack;

    return (final_sr & FLASH_SR_ERRORS) ? FLASH_ERR : SWD_ACK_OK;
}

static uint8_t flash_finish(uint8_t ack, uint32_t sr)
{
    return flash_finish_ms(ack, sr, BSY_PROGRAM_MS);
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

/*
 * Reads back in small chunks so this costs stack rather than a second buffer
 * the size of a transfer block.
 */
uint8_t flash_verify(uint32_t addr, const uint32_t *words, uint16_t count)
{
    uint32_t chunk[8];
    uint16_t done = 0;

    while (done < count) {
        uint16_t n = count - done;
        if (n > 8)
            n = 8;

        uint8_t ack = mem_ap_read_block(addr + 4UL * done, chunk, n);
        if (ack != SWD_ACK_OK)
            return ack;

        for (uint16_t i = 0; i < n; i++)
            if (chunk[i] != words[done + i])
                return FLASH_VERIFY;

        done += n;
    }

    return SWD_ACK_OK;
}

uint8_t flash_erase_sector(uint8_t sector)
{
    if (sectors && sector >= sectors)
        return FLASH_ERR;

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

    return flash_finish_ms(ack, sr, BSY_ERASE_MS);
}

/* A whole-device erase takes seconds, not milliseconds. */
#define BSY_MASS_ERASE_MS 30000

uint8_t flash_mass_erase(void)
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

    uint32_t cr = FLASH_CR_PSIZE_X32 | FLASH_CR_MER;

    ack = mem_ap_write_word(FLASH_CR, cr);
    if (ack != SWD_ACK_OK)
        return ack;

    ack = mem_ap_write_word(FLASH_CR, cr | FLASH_CR_STRT);

    return flash_finish_ms(ack, sr, BSY_MASS_ERASE_MS);
}

uint8_t flash_read_optcr(uint32_t *optcr)
{
    return mem_ap_read_word(FLASH_OPTCR, optcr);
}

uint8_t flash_rdp_level(uint8_t *level)
{
    uint32_t optcr = 0;
    uint8_t ack = flash_read_optcr(&optcr);
    if (ack != SWD_ACK_OK)
        return ack;

    uint8_t rdp = (uint8_t)(optcr >> FLASH_OPTCR_RDP_SHIFT);

    if (rdp == RDP_LEVEL0)
        *level = 0;
    else if (rdp == RDP_LEVEL2)
        *level = 2;
    else
        *level = 1;

    return SWD_ACK_OK;
}

static uint8_t flash_option_unlock(void)
{
    uint8_t ack = mem_ap_write_word(FLASH_OPTKEYR, FLASH_OPTKEY1);
    if (ack != SWD_ACK_OK)
        return ack;

    ack = mem_ap_write_word(FLASH_OPTKEYR, FLASH_OPTKEY2);
    if (ack != SWD_ACK_OK)
        return ack;

    uint32_t optcr = 0;
    ack = flash_read_optcr(&optcr);
    if (ack != SWD_ACK_OK)
        return ack;

    return (optcr & FLASH_OPTCR_OPTLOCK) ? FLASH_LOCKED : SWD_ACK_OK;
}

/*
 * Going from level 1 back to level 0 mass erases the flash, by design: the
 * contents cannot survive protection being dropped.
 */
uint8_t flash_remove_readout_protection(void)
{
    uint8_t level = 0;
    uint8_t ack = flash_rdp_level(&level);
    if (ack != SWD_ACK_OK)
        return ack;

    if (level == 2)
        return FLASH_LOCKED;

    if (level == 0)
        return SWD_ACK_OK;

    ack = flash_option_unlock();
    if (ack != SWD_ACK_OK)
        return ack;

    uint32_t optcr = 0;
    ack = flash_read_optcr(&optcr);
    if (ack != SWD_ACK_OK)
        return ack;

    optcr &= ~(0xFFUL << FLASH_OPTCR_RDP_SHIFT);
    optcr |= (uint32_t)RDP_LEVEL0 << FLASH_OPTCR_RDP_SHIFT;

    ack = mem_ap_write_word(FLASH_OPTCR, optcr);
    if (ack != SWD_ACK_OK)
        return ack;

    ack = mem_ap_write_word(FLASH_OPTCR, optcr | FLASH_OPTCR_OPTSTRT);
    if (ack != SWD_ACK_OK)
        return ack;

    uint32_t sr = 0;
    return flash_wait_busy_ms(&sr, BSY_MASS_ERASE_MS);
}
