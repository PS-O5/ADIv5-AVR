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

#define REGRDY_RETRIES 32

/*
 * The core registers are not memory mapped. Writing a selector to DCRSR moves
 * the value between the register file and DCRDR, and S_REGRDY says when the
 * transfer has happened. The core has to be halted; reading while it runs is
 * UNPREDICTABLE.
 */
static uint8_t wait_regrdy(void)
{
    for (uint8_t i = 0; i < REGRDY_RETRIES; i++) {
        uint32_t dhcsr = 0;
        uint8_t ack = mem_ap_read_word(DHCSR, &dhcsr);
        if (ack != SWD_ACK_OK)
            return ack;
        if (dhcsr & DHCSR_S_REGRDY)
            return SWD_ACK_OK;
    }

    return SWD_TIMEOUT;
}

uint8_t cortex_read_reg(uint8_t reg, uint32_t *value)
{
    uint8_t ack = mem_ap_write_word(DCRSR, reg & 0x7F);
    if (ack != SWD_ACK_OK)
        return ack;

    ack = wait_regrdy();
    if (ack != SWD_ACK_OK)
        return ack;

    return mem_ap_read_word(DCRDR, value);
}

uint8_t cortex_write_reg(uint8_t reg, uint32_t value)
{
    /* Same rule as reading: the transfer is UNPREDICTABLE on a running core. */
    uint32_t dhcsr = 0;
    uint8_t ack = cortex_read_dhcsr(&dhcsr);
    if (ack != SWD_ACK_OK)
        return ack;

    if (!(dhcsr & DHCSR_S_HALT))
        return CORTEX_NOT_HALTED;

    ack = mem_ap_write_word(DCRDR, value);
    if (ack != SWD_ACK_OK)
        return ack;

    ack = mem_ap_write_word(DCRSR, (reg & 0x7F) | DCRSR_REGWNR);
    if (ack != SWD_ACK_OK)
        return ack;

    return wait_regrdy();
}

#define STEP_RETRIES 64

/*
 * C_HALT has to be clear for the step to happen: the core leaves debug state,
 * retires one instruction, and halts again. Masking interrupts keeps a pending
 * exception from stealing the step and landing in a handler instead.
 */
uint8_t cortex_step(uint8_t mask_interrupts)
{
    uint32_t dhcsr = 0;
    uint8_t ack = cortex_read_dhcsr(&dhcsr);
    if (ack != SWD_ACK_OK)
        return ack;

    if (!(dhcsr & DHCSR_S_HALT))
        return CORTEX_NOT_HALTED;

    uint32_t cmd = DHCSR_DBGKEY | DHCSR_C_DEBUGEN | DHCSR_C_STEP;
    if (mask_interrupts)
        cmd |= DHCSR_C_MASKINTS;

    ack = mem_ap_write_word(DHCSR, cmd);
    if (ack != SWD_ACK_OK)
        return ack;

    for (uint8_t i = 0; i < STEP_RETRIES; i++) {
        ack = cortex_read_dhcsr(&dhcsr);
        if (ack != SWD_ACK_OK)
            return ack;
        if (dhcsr & DHCSR_S_HALT)
            return SWD_ACK_OK;
    }

    return SWD_TIMEOUT;
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
