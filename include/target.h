#ifndef TARGET_H
#define TARGET_H

#include <stdint.h>

#define DBGMCU_IDCODE 0xE0042000UL

#define TARGET_NAME_MAX 12

/* Returned by flash operations when the part is not one flash.c understands. */
#define TARGET_UNKNOWN 0xF3

uint8_t target_identify(void);

uint16_t target_dev_id(void);
uint16_t target_rev_id(void);

/* 1 and fills buf when the device id is in the table, 0 otherwise. */
uint8_t target_name(char *buf);

/* Whether flash.c's F4 sector algorithm applies to this part. */
uint8_t target_flash_ok(void);

void target_force(void);
uint8_t target_forced(void);

#endif
