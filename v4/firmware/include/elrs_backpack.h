#pragma once
/* ELRS VRx backpack link: HDZero-protocol MSPv2 on one UART wire.
 *
 * Receiver side of the ExpressLRS Backpack HDZero VRx target
 * (https://github.com/ExpressLRS/Backpack, GPL-3.0, src/hdzero.cpp and
 * lib/MSP at release 1.5.9): "$X<" + flags + function16 + size16 + payload
 * + crc8_dvb_s2, little endian. Only MSP_ELRS_BACKPACK_SET_CHANNEL_INDEX is
 * acted on; the C5 has no return wire, so GET requests are never answered.
 * Pure C, no ESP-IDF: host-tested by tools/test_elrs_backpack.c. */
#include <stdbool.h>
#include <stdint.h>

#define ELRS_MSP_SET_CHANNEL_INDEX 0x0301u
#define ELRS_CHANNEL_COUNT 48u
#define ELRS_MSP_MAX_PAYLOAD 16u

typedef struct {
    uint8_t state, crc, header[5], header_pos;
    uint16_t function, size, received;
    uint8_t payload[ELRS_MSP_MAX_PAYLOAD];
    uint32_t frames, crc_errors, oversize;
} elrs_msp_t;

void elrs_msp_init(elrs_msp_t *m);
/* Feed one received byte. True when a complete, CRC-valid command frame is
 * in m->function / m->payload / m->size (valid until the next feed). */
bool elrs_msp_feed(elrs_msp_t *m, uint8_t byte);
uint8_t elrs_crc8_dvb_s2(uint8_t crc, uint8_t byte);

/* ELRS channel index (bands A,B,E,F,R,L x 8) to the C5VRX rf_set_channel
 * index (bands R,A,B,E,F x 8, rf.h fpv_band_t); -1 when out of range or
 * ELRS L band, whose frequencies differ from the C5VRX L band. */
int elrs_backpack_c5_index(uint8_t elrs_index);
/* ELRS frequency for an index (Backpack src/rx5808.h table), 0 if invalid. */
uint16_t elrs_backpack_mhz(uint8_t elrs_index);
