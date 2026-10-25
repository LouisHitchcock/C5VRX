/* C5VRX by Twotoz and the C5VRX contributors, GPL-3.0-only.
 * Adapted from C5VRX-4 firmware/elrs_backpack.c and include/elrs_backpack.h
 * on range/rapidfire-parity; see docs/ELRS_BACKPACK.md for source revisions.
 * HDZero MSPv2 framing follows ExpressLRS/Backpack 1.5.9, src/hdzero.cpp
 * and lib/MSP (GPL-3.0): https://github.com/ExpressLRS/Backpack */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define ELRS_MSP_SET_CHANNEL_INDEX 0x0301u
#define ELRS_CHANNEL_COUNT 48u
#define ELRS_MSP_MAX_PAYLOAD 16u

typedef struct {
    uint8_t state, crc, header[5], header_pos;
    uint16_t function, size, received;
    uint8_t payload[ELRS_MSP_MAX_PAYLOAD];
    int last_index, pending_index;
    uint32_t frames, crc_errors, oversize, commands, rejected;
} elrs_backpack_t;

void elrs_backpack_init(elrs_backpack_t *m);
void elrs_backpack_feed(elrs_backpack_t *m, uint8_t byte);
/* Only a changed radio channel becomes pending. Repeats leave a local
 * menu/USB selection alone. Busy owners may defer taking the latest request. */
bool elrs_backpack_take(elrs_backpack_t *m, uint8_t *index);
uint8_t elrs_crc8_dvb_s2(uint8_t crc, uint8_t byte);
uint16_t elrs_backpack_mhz(uint8_t index);
/* C5 channel order R,A,B,E,F,L. ELRS L has a different frequency plan. */
int elrs_backpack_c5_index(uint8_t index);
