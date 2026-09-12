#ifndef RTT_H
#define RTT_H

#include <stdint.h>

/*
 * SEGGER RTT. The target firmware leaves a control block in RAM holding ring
 * buffers; the host finds it by its ID string and then moves the read pointer
 * itself. Nothing but ordinary memory access is needed, so no extra pin.
 *
 * Control block: 16 byte ID, then two int32 counts, then the buffer
 * descriptors. Each descriptor is name, pointer, size, write offset, read
 * offset, flags.
 */
#define RTT_CB_HEADER 24
#define RTT_DESC_SIZE 24

#define RTT_NOT_FOUND 0xF5

uint8_t rtt_find(uint32_t base, uint32_t length, uint32_t *cb_addr);
uint8_t rtt_up_count(uint32_t cb, uint32_t *count);

/* Drains one up buffer, handing each byte to the sink. */
uint8_t rtt_drain(uint32_t cb, uint8_t index, void (*sink)(char), uint16_t *bytes);

#endif
