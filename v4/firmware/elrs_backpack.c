#include "elrs_backpack.h"
#include <stddef.h>

enum { WAIT_DOLLAR, WAIT_X, WAIT_TYPE, HEADER, PAYLOAD, CHECKSUM };

/* ExpressLRS Backpack frequency table (src/rx5808.h), ELRS band order. */
static const uint16_t s_elrs_mhz[ELRS_CHANNEL_COUNT] = {
    5865, 5845, 5825, 5805, 5785, 5765, 5745, 5725, /* A */
    5733, 5752, 5771, 5790, 5809, 5828, 5847, 5866, /* B */
    5705, 5685, 5665, 5645, 5885, 5905, 5925, 5945, /* E */
    5740, 5760, 5780, 5800, 5820, 5840, 5860, 5880, /* F */
    5658, 5695, 5732, 5769, 5806, 5843, 5880, 5917, /* R */
    5333, 5373, 5413, 5453, 5493, 5533, 5573, 5613, /* L */
};
/* ELRS band A,B,E,F,R,L -> rf.h FPV_BAND_A=1,B=2,E=3,F=4,R=0. ELRS L
 * (5333..5613 MHz, 40 MHz steps) is not the C5VRX L band (5362..5621 MHz,
 * 37 MHz steps), so it is refused instead of tuning up to 29 MHz off. */
#define NO_BAND 0xFFu
static const uint8_t s_c5_band[6] = {1, 2, 3, 4, 0, NO_BAND};

uint8_t elrs_crc8_dvb_s2(uint8_t crc, uint8_t byte)
{
    crc ^= byte;
    for (int bit = 0; bit < 8; ++bit)
        crc = (crc & 0x80u) ? (uint8_t)((crc << 1) ^ 0xD5u) : (uint8_t)(crc << 1);
    return crc;
}

void elrs_msp_init(elrs_msp_t *m)
{
    *m = (elrs_msp_t){0};
}

bool elrs_msp_feed(elrs_msp_t *m, uint8_t byte)
{
    switch (m->state) {
    case WAIT_DOLLAR:
        if (byte == '$') m->state = WAIT_X;
        return false;
    case WAIT_X:
        m->state = byte == 'X' ? WAIT_TYPE : byte == '$' ? WAIT_X : WAIT_DOLLAR;
        return false;
    case WAIT_TYPE:
        /* Commands only; '>' responses and '!' errors are not for us. */
        if (byte == '<') { m->state = HEADER; m->header_pos = 0; m->crc = 0; }
        else m->state = byte == '$' ? WAIT_X : WAIT_DOLLAR;
        return false;
    case HEADER:
        m->header[m->header_pos++] = byte;
        m->crc = elrs_crc8_dvb_s2(m->crc, byte);
        if (m->header_pos < sizeof(m->header)) return false;
        m->function = (uint16_t)(m->header[1] | m->header[2] << 8);
        m->size = (uint16_t)(m->header[3] | m->header[4] << 8);
        m->received = 0;
        if (m->size > ELRS_MSP_MAX_PAYLOAD) { ++m->oversize; m->state = WAIT_DOLLAR; return false; }
        m->state = m->size ? PAYLOAD : CHECKSUM;
        return false;
    case PAYLOAD:
        m->payload[m->received++] = byte;
        m->crc = elrs_crc8_dvb_s2(m->crc, byte);
        if (m->received == m->size) m->state = CHECKSUM;
        return false;
    default: /* CHECKSUM */
        m->state = WAIT_DOLLAR;
        if (byte != m->crc) { ++m->crc_errors; return false; }
        ++m->frames;
        return true;
    }
}

int elrs_backpack_c5_index(uint8_t elrs_index)
{
    if (elrs_index >= ELRS_CHANNEL_COUNT || s_c5_band[elrs_index / 8u] == NO_BAND) return -1;
    return s_c5_band[elrs_index / 8u] * 8 + elrs_index % 8u;
}

uint16_t elrs_backpack_mhz(uint8_t elrs_index)
{
    return elrs_index < ELRS_CHANNEL_COUNT ? s_elrs_mhz[elrs_index] : 0u;
}
