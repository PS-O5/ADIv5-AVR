#ifndef CORTEX_H
#define CORTEX_H

#include <stdint.h>

#define DHCSR 0xE000EDF0UL
#define DCRSR 0xE000EDF4UL
#define DCRDR 0xE000EDF8UL
#define DEMCR 0xE000EDFCUL
#define AIRCR 0xE000ED0CUL

#define DCRSR_REGWNR (1UL << 16)

/* Selector values. 0 to 12 are R0 to R12. */
#define REG_SP   13
#define REG_LR   14
#define REG_PC   15
#define REG_XPSR 16
#define REG_LAST REG_XPSR

#define DHCSR_DBGKEY    0xA05F0000UL
#define DHCSR_C_DEBUGEN  (1UL << 0)
#define DHCSR_C_HALT     (1UL << 1)
#define DHCSR_C_STEP     (1UL << 2)
#define DHCSR_C_MASKINTS (1UL << 3)
#define DHCSR_S_REGRDY  (1UL << 16)
#define DHCSR_S_HALT    (1UL << 17)
#define DHCSR_S_LOCKUP  (1UL << 19)

#define DEMCR_VC_CORERESET (1UL << 0)

#define AIRCR_VECTKEY     0x05FA0000UL
#define AIRCR_SYSRESETREQ (1UL << 2)

uint8_t cortex_halt(void);
uint8_t cortex_resume(void);
uint8_t cortex_read_dhcsr(uint32_t *value);
uint8_t cortex_reset_halt(void);

/* Returned when an operation needs a halted core and the core is running. */
#define CORTEX_NOT_HALTED 0xFB

uint8_t cortex_read_reg(uint8_t reg, uint32_t *value);
uint8_t cortex_write_reg(uint8_t reg, uint32_t value);
uint8_t cortex_step(uint8_t mask_interrupts);

#endif
