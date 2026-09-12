#ifndef AP_H
#define AP_H

#include <stdint.h>

#define AP_CSW 0x00
#define AP_TAR 0x04
#define AP_DRW 0x0C
#define AP_IDR 0x0C

#define CSW_SIZE_BYTE      0x0
#define CSW_SIZE_HALF      0x1
#define CSW_SIZE_WORD      0x2
#define CSW_ADDRINC_SINGLE (0x1 << 4)

/* Returned when the MEM-AP does not implement the requested access size. */
#define MEM_AP_BAD_SIZE 0xF8

/*
 * CSW Prot bits [30:24] are IMPLEMENTATION DEFINED in ADIv5. For a Cortex-M
 * AHB-AP they carry MasterType=Debug (29) and HPROT[1]=privileged (25); bit 24
 * is set by convention. Without them the AHB transaction is rejected and the
 * DP latches STICKYERR.
 */
#define CSW_PROT_DEBUG     0x23000000UL

uint8_t ap_select(uint8_t ap_index, uint8_t bank);
uint8_t ap_read(uint8_t addr, uint32_t *data);
uint8_t ap_write(uint8_t addr, uint32_t data);

uint8_t mem_ap_init(void);

/* TAR auto-increment is only guaranteed across the bottom 10 address bits. */
#define TAR_BOUNDARY 1024UL

uint8_t mem_ap_read_word(uint32_t addr, uint32_t *value);
uint8_t mem_ap_write_word(uint32_t addr, uint32_t value);
uint8_t mem_ap_read_block(uint32_t addr, uint32_t *out, uint16_t count);

uint8_t mem_ap_read8(uint32_t addr, uint8_t *value);
uint8_t mem_ap_write8(uint32_t addr, uint8_t value);
uint8_t mem_ap_read16(uint32_t addr, uint16_t *value);
uint8_t mem_ap_write16(uint32_t addr, uint16_t value);

#endif
