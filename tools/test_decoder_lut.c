/* C5VRX by Twotoz and contributors: C5VRX-3 DC recentring regressions.
 * gcc -std=c11 -Wall -Wextra -Werror -DDECODER_LUT_HOST -Imain \
 *     tools/test_decoder_lut.c main/decoder_lut.c -lm */
#define _DEFAULT_SOURCE
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "decoder_lut.h"
#include "predemod.h"

static uint16_t lut[1024];
static bool width16 = true, stuck;
static unsigned host_writes;
uint16_t lut_read(unsigned index) { return lut[index]; }
void lut_write(unsigned index, uint16_t value) { ++host_writes; if (!stuck) lut[index] = value; }
bool lut_width16(void) { return width16; }

static uint8_t iq(int i, int q) { return (uint8_t)(((i & 15) << 4) | (q & 15)); }

/* The checked-in live program's LUT, so the C formula is tied to it. */
static unsigned load_program_lut(uint16_t out[1024])
{
    FILE *f = fopen("main/fm_phase8_hr_live.bsasm", "r");
    assert(f);
    static char line[1 << 16];
    unsigned n = 0;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "lut ", 4)) continue;
        for (char *t = strtok(line + 4, " \n"); t && n < 1024; t = strtok(NULL, " \n"))
            out[n++] = (uint16_t)strtoul(t, NULL, 10);
    }
    fclose(f);
    return n;
}

int main(void)
{
    uint16_t program[1024];
    assert(load_program_lut(program) == 1024u);
    for (unsigned i = 0; i < 1024u; ++i)
        assert(program[i] == predemod_hr_live_word((uint8_t)(i & 255u), 0, 0));

    /* Centre moved +1 cell on I: raw (2,0) is ~18 deg (Phase8 13), raw (0,0)
     * ~136 deg (Phase8 97). */
    assert(predemod_phase8(iq(2, 0), 1000, 0) == 13);
    assert(predemod_phase8(iq(0, 0), 1000, 0) == 97);
    assert(predemod_lane0_to_mcells(300, 2) == 1200 && predemod_mcells_to_lane0(1200, 2) == 300);

    /* Not Phase8 FULL, wrong width, foreign table: refused, table untouched. */
    memcpy(lut, program, sizeof(lut));
    assert(decoder_lut_prepare(false, 0) && !decoder_lut_ready());
    width16 = false;
    assert(decoder_lut_prepare(true, 0) && !decoder_lut_ready());
    width16 = true;
    lut[700] ^= 4u;
    assert(decoder_lut_prepare(true, 0) && !decoder_lut_ready());
    lut[700] ^= 4u;
    host_writes = 0;

    /* Pristine table: probe passes and leaves it pristine. */
    assert(decoder_lut_prepare(true, 0) && decoder_lut_ready());
    assert(!memcmp(lut, program, sizeof(lut)) && host_writes == 2u);

    /* Recentre on lane 1 by 300 lane-0 milli-steps = 600 milli-cells. */
    assert(decoder_lut_set(300, -150, 1));
    for (unsigned i = 0; i < 1024u; ++i)
        assert(lut[i] == predemod_hr_live_word((uint8_t)(i & 255u), 600, -300));
    int applied[2];
    unsigned lane;
    decoder_lut_applied(applied, &lane);
    assert(applied[0] == 300 && applied[1] == -150 && lane == 1u);

    /* Lane change: same ADC-code offset, rewritten at the new lane's scale. */
    assert(decoder_lut_set(300, -150, 2));
    for (unsigned i = 0; i < 1024u; ++i)
        assert(lut[i] == predemod_hr_live_word((uint8_t)(i & 255u), 1200, -600));

    /* Program reload (menu exit): pristine table, offset re-applied while
     * the engine is stopped. */
    decoder_lut_stop();
    assert(!decoder_lut_set(0, 0, 2));
    memcpy(lut, program, sizeof(lut));
    assert(decoder_lut_prepare(true, 2) && decoder_lut_ready());
    for (unsigned i = 0; i < 1024u; ++i)
        assert(lut[i] == predemod_hr_live_word((uint8_t)(i & 255u), 1200, -600));

    /* Clamp at 3 cells on the current lane. */
    assert(decoder_lut_set(2000, 0, 2));
    for (unsigned i = 0; i < 1024u; ++i)
        assert(lut[i] == predemod_hr_live_word((uint8_t)(i & 255u), 3000, 0));

    /* (0, 0) restores the pristine table exactly. */
    assert(decoder_lut_set(0, 0, 2));
    assert(!memcmp(lut, program, sizeof(lut)));

    /* A write that does not read back blocks every further write. */
    stuck = true;
    assert(!decoder_lut_set(400, 0, 0) && !decoder_lut_ready());
    stuck = false;
    memcpy(lut, program, sizeof(lut));
    assert(decoder_lut_prepare(true, 0) && !decoder_lut_ready());
    decoder_lut_print();

    /* Glitch metric and DC estimate. */
    uint8_t mixed[] = {iq(-1, 3), iq(-8, 3), iq(0, 3), iq(1, 3)};
    assert(predemod_glitches(mixed, 4, 6) == 1);
    uint8_t step[] = {iq(-6, 0), iq(1, 0), iq(5, 0)};
    assert(predemod_glitches(step, 3, 6) == 0);
    int di, dq;
    uint8_t centred[] = {iq(0, -1), iq(-1, 0)};
    predemod_dc_mcells(centred, 2, &di, &dq);
    assert(di == 0 && dq == 0);
    uint8_t offset[] = {iq(2, 0), iq(2, 0)};
    predemod_dc_mcells(offset, 2, &di, &dq);
    assert(di == 2500 && dq == 500);
    assert(predemod_dc_cal_point(5917, 1) == 5855 && predemod_dc_cal_point(5865, 0) == 2432);

    /* Decision: two agreeing looks, averaged; jumps wait; clamp; deadband. */
    predemod_dc_filter_t f = {0};
    int app[2] = {0, 0}, out[2];
    int m1[2] = {900, -300}, m2[2] = {950, -280}, far[2] = {9000, 0};
    assert(!predemod_dc_decide(&f, m1, app, 150, 120, 3000, out));
    assert(predemod_dc_decide(&f, m2, app, 150, 120, 3000, out) && out[0] == 925 && out[1] == -290);
    app[0] = out[0]; app[1] = out[1];
    assert(!predemod_dc_decide(&f, m2, app, 150, 120, 3000, out));
    assert(!predemod_dc_decide(&f, far, app, 150, 120, 3000, out));
    assert(predemod_dc_decide(&f, far, app, 150, 120, 3000, out) && out[0] == 3000);
    predemod_dc_filter_t g = {0};
    int small[2] = {50, -40};
    app[0] = 400; app[1] = 0;
    assert(!predemod_dc_decide(&g, small, app, 150, 120, 3000, out));
    assert(predemod_dc_decide(&g, small, app, 150, 120, 3000, out) && !out[0] && !out[1]);

    puts("C5VRX-3 DC recentring: program-exact Phase8 table, probe, lane rescale, "
         "reload re-apply, clamp, restore, blocked read-back, glitch/DC/decision OK");
    return 0;
}
