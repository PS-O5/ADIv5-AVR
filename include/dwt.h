#ifndef DWT_H
#define DWT_H

#include <stdint.h>

#define DWT_CTRL      0xE0001000UL
#define DWT_COMP_BASE 0xE0001020UL

/* Each comparator is a 16 byte block: COMP, MASK, FUNCTION, reserved. */
#define DWT_COMP(n)     (DWT_COMP_BASE + 16UL * (n))
#define DWT_MASK(n)     (DWT_COMP_BASE + 16UL * (n) + 4)
#define DWT_FUNCTION(n) (DWT_COMP_BASE + 16UL * (n) + 8)

#define DWT_FUNC_DISABLED 0x0
#define DWT_FUNC_READ     0x5
#define DWT_FUNC_WRITE    0x6
#define DWT_FUNC_RW       0x7

#define DWT_FUNC_MATCHED (1UL << 24)

#define DWT_NO_SLOT 0xF7
#define DWT_BAD_MASK 0xF6

uint8_t dwt_init(void);
uint8_t dwt_slots(void);

uint8_t dwt_set(uint8_t slot, uint32_t addr, uint8_t function, uint8_t mask);
uint8_t dwt_clear(uint8_t slot);
uint8_t dwt_get(uint8_t slot, uint32_t *addr, uint8_t *function, uint8_t *matched);

#endif
