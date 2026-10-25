/* C5VRX by Twotoz and the C5VRX contributors, GPL-3.0-only.
 * Extends the C5VRX-4 MSPv2 parser; provenance in elrs_backpack.h. */
#include "elrs_backpack.h"

enum { WAIT_DOLLAR, WAIT_X, WAIT_TYPE, HEADER, PAYLOAD, CHECKSUM };
static const uint16_t s_elrs_mhz[ELRS_CHANNEL_COUNT] = {
    5865, 5845, 5825, 5805, 5785, 5765, 5745, 5725, /* A */
    5733, 5752, 5771, 5790, 5809, 5828, 5847, 5866, /* B */
    5705, 5685, 5665, 5645, 5885, 5905, 5925, 5945, /* E */
    5740, 5760, 5780, 5800, 5820, 5840, 5860, 5880, /* F */
    5658, 5695, 5732, 5769, 5806, 5843, 5880, 5917, /* R */
    5333, 5373, 5413, 5453, 5493, 5533, 5573, 5613, /* L */
};
static const uint8_t s_c5_band[5] = {1, 2, 3, 4, 0};

uint16_t elrs_backpack_mhz(uint8_t index)
{
    return index < ELRS_CHANNEL_COUNT ? s_elrs_mhz[index] : 0;
}

int elrs_backpack_c5_index(uint8_t index)
{
    if (index >= 40) return -1;
    return s_c5_band[index / 8u] * 8 + (int)(index % 8u);
}

uint8_t elrs_crc8_dvb_s2(uint8_t crc, uint8_t byte)
{
    crc ^= byte;
    for (unsigned bit = 0; bit < 8; ++bit)
        crc = (crc & 0x80u) ? (uint8_t)((crc << 1) ^ 0xD5u) : (uint8_t)(crc << 1);
    return crc;
}

void elrs_backpack_init(elrs_backpack_t *m)
{
    *m = (elrs_backpack_t){.last_index = -1, .pending_index = -1};
}

void elrs_backpack_feed(elrs_backpack_t *m, uint8_t byte)
{
    switch (m->state) {
    case WAIT_DOLLAR:
        if (byte == '$') m->state = WAIT_X;
        return;
    case WAIT_X:
        m->state = byte == 'X' ? WAIT_TYPE : byte == '$' ? WAIT_X : WAIT_DOLLAR;
        return;
    case WAIT_TYPE:
        if (byte == '<') { m->state = HEADER; m->header_pos = 0; m->crc = 0; }
        else m->state = byte == '$' ? WAIT_X : WAIT_DOLLAR;
        return;
    case HEADER:
        m->header[m->header_pos++] = byte;
        m->crc = elrs_crc8_dvb_s2(m->crc, byte);
        if (m->header_pos < sizeof(m->header)) return;
        m->function = (uint16_t)(m->header[1] | m->header[2] << 8);
        m->size = (uint16_t)(m->header[3] | m->header[4] << 8);
        m->received = 0;
        if (m->size > ELRS_MSP_MAX_PAYLOAD) { ++m->oversize; m->state = WAIT_DOLLAR; return; }
        m->state = m->size ? PAYLOAD : CHECKSUM;
        return;
    case PAYLOAD:
        m->payload[m->received++] = byte;
        m->crc = elrs_crc8_dvb_s2(m->crc, byte);
        if (m->received == m->size) m->state = CHECKSUM;
        return;
    default:
        m->state = WAIT_DOLLAR;
        if (byte != m->crc) { ++m->crc_errors; return; }
        ++m->frames;
        if (m->function != ELRS_MSP_SET_CHANNEL_INDEX) return;
        if (m->header[0] != 0 || m->size != 1) { ++m->rejected; return; }
        ++m->commands;
        int index = m->payload[0];
        if (index == m->last_index) return;
        m->last_index = index;
        /* A newer unsupported request cancels an older deferred one. */
        m->pending_index = -1;
        if (elrs_backpack_c5_index((uint8_t)index) < 0) { ++m->rejected; return; }
        m->pending_index = index;
        return;
    }
}

bool elrs_backpack_take(elrs_backpack_t *m, uint8_t *index)
{
    if (m->pending_index < 0) return false;
    *index = (uint8_t)m->pending_index;
    m->pending_index = -1;
    return true;
}
