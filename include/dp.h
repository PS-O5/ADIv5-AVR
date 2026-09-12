#ifndef DP_H
#define DP_H

#include <stdint.h>

#define DP_DPIDR    0x0
#define DP_ABORT    0x0
#define DP_CTRLSTAT 0x4
#define DP_SELECT   0x8
#define DP_RDBUFF   0xC

#define CTRLSTAT_CSYSPWRUPACK (1UL << 31)
#define CTRLSTAT_CSYSPWRUPREQ (1UL << 30)
#define CTRLSTAT_CDBGPWRUPACK (1UL << 29)
#define CTRLSTAT_CDBGPWRUPREQ (1UL << 28)

uint8_t dp_read(uint8_t addr, uint32_t *data);
uint8_t dp_write(uint8_t addr, uint32_t data);

uint8_t dp_power_up(void);
uint8_t dp_clear_errors(void);

#endif
