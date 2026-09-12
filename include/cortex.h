#ifndef CORTEX_H
#define CORTEX_H

#include <stdint.h>

#define DHCSR 0xE000EDF0UL
#define DEMCR 0xE000EDFCUL
#define AIRCR 0xE000ED0CUL

#define DHCSR_DBGKEY    0xA05F0000UL
#define DHCSR_C_DEBUGEN (1UL << 0)
#define DHCSR_C_HALT    (1UL << 1)
#define DHCSR_S_HALT    (1UL << 17)
#define DHCSR_S_LOCKUP  (1UL << 19)

#define DEMCR_VC_CORERESET (1UL << 0)

#define AIRCR_VECTKEY     0x05FA0000UL
#define AIRCR_SYSRESETREQ (1UL << 2)

uint8_t cortex_halt(void);
uint8_t cortex_resume(void);
uint8_t cortex_read_dhcsr(uint32_t *value);
uint8_t cortex_reset_halt(void);

#endif
