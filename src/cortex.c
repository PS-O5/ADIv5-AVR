#include "swd.h"
#include "dp.h"
#include "ap.h"
#include "cortex.h"

uint8_t cortex_halt(void)
{
    return mem_ap_write_word(DHCSR, DHCSR_DBGKEY | DHCSR_C_DEBUGEN | DHCSR_C_HALT);
}

uint8_t cortex_resume(void)
{
    return mem_ap_write_word(DHCSR, DHCSR_DBGKEY | DHCSR_C_DEBUGEN);
}

uint8_t cortex_read_dhcsr(uint32_t *value)
{
    return mem_ap_read_word(DHCSR, value);
}

#define HALT_RETRIES 64

uint8_t cortex_reset_halt(void)
{
    uint8_t ack = mem_ap_write_word(DHCSR, DHCSR_DBGKEY | DHCSR_C_DEBUGEN);
    if (ack != SWD_ACK_OK)
        return ack;

    ack = mem_ap_write_word(DEMCR, DEMCR_VC_CORERESET);
    if (ack != SWD_ACK_OK)
        return ack;

    /* The reset takes effect mid-transaction, so this access may not complete. */
    mem_ap_write_word(AIRCR, AIRCR_VECTKEY | AIRCR_SYSRESETREQ);
    dp_clear_errors();

    for (uint8_t i = 0; i < HALT_RETRIES; i++) {
        uint32_t dhcsr = 0;
        if (cortex_read_dhcsr(&dhcsr) == SWD_ACK_OK && (dhcsr & DHCSR_S_HALT))
            return SWD_ACK_OK;
        dp_clear_errors();
    }

    return SWD_TIMEOUT;
}
