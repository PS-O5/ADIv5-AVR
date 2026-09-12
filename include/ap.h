#ifndef AP_H
#define AP_H

#include <stdint.h>

#define AP_CSW 0x00
#define AP_TAR 0x04
#define AP_DRW 0x0C
#define AP_IDR 0x0C

#define CSW_SIZE_WORD    0x2
#define CSW_ADDRINC_SINGLE (0x1 << 4)

uint8_t ap_select(uint8_t ap_index, uint8_t bank);
uint8_t ap_read(uint8_t addr, uint32_t *data);
uint8_t ap_write(uint8_t addr, uint32_t data);

uint8_t mem_ap_init(void);

#endif
