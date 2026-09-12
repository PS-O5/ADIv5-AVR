#ifndef FLASH_H
#define FLASH_H

#include <stdint.h>

#define FLASH_BASE 0x40023C00UL
#define FLASH_KEYR (FLASH_BASE + 0x04)
#define FLASH_SR   (FLASH_BASE + 0x0C)
#define FLASH_CR   (FLASH_BASE + 0x10)

#define FLASH_OPTKEYR (FLASH_BASE + 0x08)
#define FLASH_OPTCR   (FLASH_BASE + 0x14)

#define FLASH_KEY1 0x45670123UL
#define FLASH_KEY2 0xCDEF89ABUL

#define FLASH_OPTKEY1 0x08192A3BUL
#define FLASH_OPTKEY2 0x4C5D6E7FUL

#define FLASH_OPTCR_OPTLOCK (1UL << 0)
#define FLASH_OPTCR_OPTSTRT (1UL << 1)
#define FLASH_OPTCR_RDP_SHIFT 8

#define RDP_LEVEL0 0xAA  /* no protection */
#define RDP_LEVEL2 0xCC  /* debug disabled, irreversible */

#define FLASH_SR_EOP    (1UL << 0)
#define FLASH_SR_OPERR  (1UL << 1)
#define FLASH_SR_WRPERR (1UL << 4)
#define FLASH_SR_PGAERR (1UL << 5)
#define FLASH_SR_PGPERR (1UL << 6)
#define FLASH_SR_PGSERR (1UL << 7)
#define FLASH_SR_BSY    (1UL << 16)

#define FLASH_SR_ERRORS (FLASH_SR_OPERR | FLASH_SR_WRPERR | FLASH_SR_PGAERR \
                       | FLASH_SR_PGPERR | FLASH_SR_PGSERR)

#define FLASH_CR_PG        (1UL << 0)
#define FLASH_CR_SER       (1UL << 1)
#define FLASH_CR_MER       (1UL << 2)
#define FLASH_CR_SNB_SHIFT 3
#define FLASH_CR_PSIZE_X32 (0x2UL << 8)
#define FLASH_CR_STRT      (1UL << 16)
#define FLASH_CR_LOCK      (1UL << 31)

/* Reported when FLASH_SR flags an error rather than the transport failing. */
#define FLASH_ERR    0xFD
/* Reported when flash reads back as something other than what was written. */
#define FLASH_VERIFY 0xF6
/* Reported when FLASH_CR is still locked, which would otherwise fail silently. */
#define FLASH_LOCKED 0xFC

#define FLASH_BASE_ADDR 0x08000000UL
#define FLASH_SIZE_REG  0x1FFF7A22UL

/* Reads the size register and derives the sector map. Call after connecting. */
uint8_t flash_probe(void);
uint16_t flash_size_kb(void);
uint8_t flash_sectors(void);
uint32_t flash_sector_base(uint8_t sector);
uint32_t flash_sector_size(uint8_t sector);
uint8_t flash_sector_of(uint32_t addr, uint8_t *sector);

uint8_t flash_unlock(void);
uint8_t flash_lock(void);
uint8_t flash_read_sr(uint32_t *sr);
uint8_t flash_program_word(uint32_t addr, uint32_t value);
uint8_t flash_write(uint32_t addr, const uint32_t *words, uint16_t count);
uint8_t flash_verify(uint32_t addr, const uint32_t *words, uint16_t count);
uint8_t flash_erase_sector(uint8_t sector);
uint8_t flash_mass_erase(void);

uint8_t flash_read_optcr(uint32_t *optcr);
uint8_t flash_rdp_level(uint8_t *level);

/*
 * Restores RDP to level 0 only. Writing an arbitrary RDP value is deliberately
 * not offered: 0xCC is level 2, which disables debug access permanently with no
 * way back, and a value that cannot be typed cannot be typed by mistake.
 */
uint8_t flash_remove_readout_protection(void);

#endif
