#ifndef FLASH_H
#define FLASH_H

#include <stdint.h>

#define FLASH_BASE 0x40023C00UL
#define FLASH_KEYR (FLASH_BASE + 0x04)
#define FLASH_SR   (FLASH_BASE + 0x0C)
#define FLASH_CR   (FLASH_BASE + 0x10)

#define FLASH_KEY1 0x45670123UL
#define FLASH_KEY2 0xCDEF89ABUL

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
#define FLASH_CR_SNB_SHIFT 3
#define FLASH_CR_PSIZE_X32 (0x2UL << 8)
#define FLASH_CR_STRT      (1UL << 16)
#define FLASH_CR_LOCK      (1UL << 31)

/* Reported when FLASH_SR flags an error rather than the transport failing. */
#define FLASH_ERR 0xFD

uint8_t flash_unlock(void);
uint8_t flash_lock(void);
uint8_t flash_read_sr(uint32_t *sr);
uint8_t flash_program_word(uint32_t addr, uint32_t value);
uint8_t flash_erase_sector(uint8_t sector);

#endif
