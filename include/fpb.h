#ifndef FPB_H
#define FPB_H

#include <stdint.h>

#define FP_CTRL  0xE0002000UL
#define FP_REMAP 0xE0002004UL
#define FP_COMP0 0xE0002008UL

#define FP_CTRL_ENABLE (1UL << 0)
#define FP_CTRL_KEY    (1UL << 1)

/* Returned when a v1 comparator is asked for an address it cannot match. */
#define FPB_OUT_OF_RANGE 0xFA
#define FPB_NO_SLOT      0xF9

uint8_t fpb_init(void);
uint8_t fpb_slots(void);
uint8_t fpb_revision(void);

uint8_t fpb_set(uint8_t slot, uint32_t addr);
uint8_t fpb_clear(uint8_t slot);
uint8_t fpb_get(uint8_t slot, uint32_t *comp, uint32_t *addr, uint8_t *enabled);

#endif
