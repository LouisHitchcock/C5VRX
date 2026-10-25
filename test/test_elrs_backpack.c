/* C5VRX by Twotoz and contributors, GPL-3.0-only. Extends the C5VRX-4
 * backpack tests: independent table CRC, malformed frames, busy/dedupe policy. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "elrs_backpack.h"
#include "../main/elrs_backpack.c"

static uint8_t ref_tab[256];
static size_t frame(uint8_t *out, char type, unsigned flags, unsigned function,
                    const uint8_t *payload, unsigned size)
{
    uint8_t header[] = {(uint8_t)flags, (uint8_t)function, (uint8_t)(function >> 8),
                        (uint8_t)size, (uint8_t)(size >> 8)};
    size_t n = 0;
    out[n++] = '$'; out[n++] = 'X'; out[n++] = (uint8_t)type;
    uint8_t crc = 0;
    for (unsigned i = 0; i < sizeof(header); ++i) { out[n++] = header[i]; crc = ref_tab[crc ^ header[i]]; }
    for (unsigned i = 0; i < size; ++i) { out[n++] = payload[i]; crc = ref_tab[crc ^ payload[i]]; }
    out[n++] = crc;
    return n;
}

static void feed(elrs_backpack_t *m, const uint8_t *data, size_t n)
{
    for (size_t k = 0; k < n; ++k) elrs_backpack_feed(m, data[k]);
}

static void send_index(elrs_backpack_t *m, uint8_t index)
{
    uint8_t bytes[10];
    feed(m, bytes, frame(bytes, '<', 0, ELRS_MSP_SET_CHANNEL_INDEX, &index, 1));
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--map") == 0) {
        for (unsigned i = 0; i < 256; ++i)
            printf("%u %d %u\n", i, elrs_backpack_c5_index((uint8_t)i), elrs_backpack_mhz((uint8_t)i));
        return 0;
    }
    for (unsigned i = 0; i < 256; ++i) {
        uint8_t crc = (uint8_t)i;
        for (unsigned j = 0; j < 8; ++j) crc = (uint8_t)((crc << 1) ^ ((crc & 0x80) ? 0xD5 : 0));
        ref_tab[i] = crc;
    }
    for (unsigned c = 0; c < 256; ++c)
        for (unsigned b = 0; b < 256; ++b)
            assert(elrs_crc8_dvb_s2((uint8_t)c, (uint8_t)b) == ref_tab[c ^ b]);

    elrs_backpack_t m;
    uint8_t index = 255, bytes[100], r3 = 34;
    elrs_backpack_init(&m);
    assert(!elrs_backpack_take(&m, &index));
    size_t n = frame(bytes, '<', 0, ELRS_MSP_SET_CHANNEL_INDEX, &r3, 1);
    for (size_t k = 0; k < n - 1; ++k) {
        elrs_backpack_feed(&m, bytes[k]);
        assert(!elrs_backpack_take(&m, &index));
    }
    elrs_backpack_feed(&m, bytes[n - 1]);
    assert(elrs_backpack_take(&m, &index) && index == 34);
    assert(m.frames == 1 && m.commands == 1);
    /* Repeated radio announcements cannot undo a subsequent local selection. */
    send_index(&m, 34); send_index(&m, 34);
    assert(!elrs_backpack_take(&m, &index));
    /* During calibration only the latest changed request survives. */
    send_index(&m, 35); send_index(&m, 36); send_index(&m, 36);
    assert(elrs_backpack_take(&m, &index) && index == 36);
    assert(!elrs_backpack_take(&m, &index));
    send_index(&m, 34);
    assert(elrs_backpack_take(&m, &index) && index == 34);
    send_index(&m, 35); send_index(&m, 40); /* ELRS L must not become C5 L. */
    assert(!elrs_backpack_take(&m, &index) && m.rejected == 1);
    for (unsigned i = 40; i < 256; ++i) {
        send_index(&m, (uint8_t)i);
        assert(!elrs_backpack_take(&m, &index));
    }

    n = frame(bytes, '<', 0, ELRS_MSP_SET_CHANNEL_INDEX, &r3, 1);
    /* All 80 possible single-bit corruptions fail closed. Size corruption
     * may consume a following frame; repeated complete frames recover. */
    for (size_t k = 0; k < n; ++k) for (unsigned bit = 0; bit < 8; ++bit) {
        uint8_t bad[10]; memcpy(bad, bytes, n); bad[k] ^= (uint8_t)(1u << bit);
        elrs_backpack_init(&m); feed(&m, bad, n);
        assert(!elrs_backpack_take(&m, &index));
        for (unsigned j = 0; j < 3; ++j) feed(&m, bytes, n);
        assert(elrs_backpack_take(&m, &index) && index == 34);
    }
    for (unsigned kind = 0; kind < 5; ++kind) {
        elrs_backpack_init(&m);
        uint8_t payload[32] = {34};
        unsigned size = kind == 2 ? 0 : kind == 3 ? 2 : kind == 4 ? 32 : 1;
        size_t length = frame(bytes, kind == 0 ? '>' : '<', kind == 1 ? 1 : 0,
                              ELRS_MSP_SET_CHANNEL_INDEX, payload, size);
        feed(&m, bytes, length);
        assert(!elrs_backpack_take(&m, &index));
        send_index(&m, 34);
        assert(elrs_backpack_take(&m, &index) && index == 34);
    }
    elrs_backpack_init(&m);
    const uint8_t noise[] = {0xff, '$', '$', 'X', 0x55, 'X', '<'};
    feed(&m, noise, sizeof(noise));
    feed(&m, bytes, frame(bytes, '<', 0, 0x0300, NULL, 0));
    assert(!elrs_backpack_take(&m, &index));
    send_index(&m, 34);
    assert(elrs_backpack_take(&m, &index) && index == 34);
    puts("PASS: ELRS CRC, framing, 80 bit errors, malformed commands, latest-request deferral and repeat suppression");
    return 0;
}
