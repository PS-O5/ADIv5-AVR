#ifndef SWD_H
#define SWD_H

#include <stdint.h>

void swd_init(void);
void swd_set_speed(uint8_t loops);

void swd_write_bit(uint8_t bit);
uint8_t swd_read_bit(void);

void swd_turnaround_to_host(void);
void swd_idle(void);

void swd_connect(void);

#define SWD_ACK_OK       1
#define SWD_ACK_WAIT     2
#define SWD_ACK_FAULT    4
#define SWD_PARITY_ERROR 0xFF
#define SWD_TIMEOUT      0xFE

uint8_t swd_transfer(uint8_t ap_ndp, uint8_t read, uint8_t addr, uint32_t *data);
uint8_t swd_transfer_retry(uint8_t ap_ndp, uint8_t read, uint8_t addr, uint32_t *data);

#endif
