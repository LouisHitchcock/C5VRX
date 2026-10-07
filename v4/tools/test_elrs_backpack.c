/* ELRS VRx backpack link regressions: MSPv2 framing, CRC and channel map.
 * The reference CRC mirrors ExpressLRS Backpack lib/CRC GENERIC_CRC8(0xD5). */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "elrs_backpack.h"
#include "../firmware/elrs_backpack.c"

static uint8_t ref_tab[256];

static void ref_init(void)
{
    for (unsigned i = 0; i < 256; ++i) {
        uint8_t crc = (uint8_t)i;
        for (int j = 0; j < 8; ++j) crc = (uint8_t)((crc << 1) ^ ((crc & 0x80) ? 0xD5 : 0));
        ref_tab[i] = crc;
    }
}

/* Builds a frame exactly like Backpack MSP::sendPacket. */
static size_t frame(uint8_t *out, char type, uint16_t function, const uint8_t *payload, uint16_t size)
{
    size_t n = 0;
    out[n++] = '$'; out[n++] = 'X'; out[n++] = (uint8_t)type;
    uint8_t header[5] = {0, (uint8_t)function, (uint8_t)(function >> 8), (uint8_t)size, (uint8_t)(size >> 8)};
    uint8_t crc = 0;
    for (int i = 0; i < 5; ++i) { out[n++] = header[i]; crc = ref_tab[crc ^ header[i]]; }
    for (uint16_t i = 0; i < size; ++i) { out[n++] = payload[i]; crc = ref_tab[crc ^ payload[i]]; }
    out[n++] = crc;
    return n;
}

static int feed_all(elrs_msp_t *m, const uint8_t *data, size_t n, int *index)
{
    int frames = 0;
    for (size_t i = 0; i < n; ++i)
        if (elrs_msp_feed(m, data[i]) && m->function == ELRS_MSP_SET_CHANNEL_INDEX && m->size >= 1) {
            ++frames;
            *index = m->payload[0];
        }
    return frames;
}

int main(void)
{
    ref_init();
    for (unsigned crc = 0; crc < 256; ++crc)
        for (unsigned b = 0; b < 256; ++b)
            assert(elrs_crc8_dvb_s2((uint8_t)crc, (uint8_t)b) == ref_tab[crc ^ b]);

    uint8_t buf[256], r3 = 34;
    elrs_msp_t m;
    int index = -1;
    elrs_msp_init(&m);
    size_t n = frame(buf, '<', ELRS_MSP_SET_CHANNEL_INDEX, &r3, 1);
    assert(n == 10 && feed_all(&m, buf, n, &index) == 1 && index == 34);

    /* Garbage, a stray '$', a lone 'X' and a GET request before the frame. */
    uint8_t noisy[64] = {0xff, 0x00, '$', '$', 'X', 0x55, 'X', '<'};
    size_t k = 8;
    k += frame(noisy + k, '<', 0x0300u, NULL, 0);
    uint8_t r5 = 36;
    k += frame(noisy + k, '<', ELRS_MSP_SET_CHANNEL_INDEX, &r5, 1);
    index = -1;
    elrs_msp_init(&m);
    assert(feed_all(&m, noisy, k, &index) == 1 && index == 36);

    /* Every single-bit error in a frame is rejected and never yields a wrong
     * index. A corrupted size field may also swallow the next good frame
     * (the sender repeats every 2 s); the one after that always decodes. */
    for (size_t byte = 0; byte < n; ++byte)
        for (int bit = 0; bit < 8; ++bit) {
            uint8_t bad[32];
            memcpy(bad, buf, n);
            bad[byte] ^= (uint8_t)(1u << bit);
            elrs_msp_init(&m);
            index = -1;
            assert(feed_all(&m, bad, n, &index) == 0 && index == -1);
            int good = feed_all(&m, buf, n, &index);
            good += feed_all(&m, buf, n, &index);
            good += feed_all(&m, buf, n, &index);
            assert(good >= 1 && index == 34);
        }

    /* Responses are ignored; oversize payloads are dropped and resync. */
    elrs_msp_init(&m);
    n = frame(buf, '>', ELRS_MSP_SET_CHANNEL_INDEX, &r3, 1);
    assert(feed_all(&m, buf, n, &index) == 0);
    uint8_t big[40] = {0};
    elrs_msp_init(&m);
    n = frame(buf, '<', ELRS_MSP_SET_CHANNEL_INDEX, big, sizeof(big));
    assert(feed_all(&m, buf, n, &index) == 0 && m.oversize == 1);
    n = frame(buf, '<', ELRS_MSP_SET_CHANNEL_INDEX, &r5, 1);
    assert(feed_all(&m, buf, n, &index) == 1 && index == 36);

    /* Byte-at-a-time across reads is the same as one buffer. */
    elrs_msp_init(&m);
    int frames = 0;
    for (size_t i = 0; i < n; ++i) {
        frames += elrs_msp_feed(&m, buf[i]);
    }
    assert(frames == 1 && m.payload[0] == 36);

    /* Channel map: R3 (ELRS 34) is C5 index 2, A1 (ELRS 0) is C5 8, L8 is 47. */
    assert(elrs_backpack_c5_index(34) == 2 && elrs_backpack_mhz(34) == 5732);
    assert(elrs_backpack_c5_index(0) == 8 && elrs_backpack_c5_index(39) == 7);
    assert(elrs_backpack_c5_index(48) == -1 && elrs_backpack_mhz(48) == 0);
    unsigned seen[48] = {0};
    for (uint8_t i = 0; i < 40; ++i) ++seen[elrs_backpack_c5_index(i)];
    for (unsigned i = 0; i < 40; ++i) assert(seen[i] == 1); /* R,A,B,E,F once each */
    for (uint8_t i = 40; i < 48; ++i) assert(elrs_backpack_c5_index(i) == -1); /* ELRS L */
    printf("PASS: ELRS backpack MSPv2 CRC (65536 pairs), framing/resync, bit errors, 40-channel map, ELRS L refused\n");
    return 0;
}
