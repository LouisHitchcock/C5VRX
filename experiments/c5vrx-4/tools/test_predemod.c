/* C5VRX by Twotoz and contributors: pre-demodulation helper regressions. */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include "predemod.h"

static uint8_t iq(int i, int q) { return (uint8_t)(((i & 15) << 4) | (q & 15)); }

int main(void)
{
    /* A smooth carrier at radius 5 cells, 30 degrees per sample: no glitches. */
    uint8_t ring[64];
    for (unsigned k = 0; k < 64; ++k) {
        double a = k * 3.14159265358979 / 6.0;
        ring[k] = iq((int)lround(5 * cos(a) - 0.5), (int)lround(5 * sin(a) - 0.5));
    }
    assert(predemod_glitches(ring, 64, 6) == 0);
    /* A mid-transition read at a zero crossing: -1 -> -8 -> 0 on I. */
    uint8_t mixed[] = {iq(-1, 3), iq(-8, 3), iq(0, 3), iq(1, 3)};
    assert(predemod_glitches(mixed, 4, 6) == 1);
    /* A genuine step (neighbours disagree) is not a glitch. */
    uint8_t step[] = {iq(-6, 0), iq(1, 0), iq(5, 0)};
    assert(predemod_glitches(step, 3, 6) == 0);

    int di, dq;
    uint8_t centred[] = {iq(0, -1), iq(-1, 0)};
    predemod_dc_mcells(centred, 2, &di, &dq);
    assert(di == 0 && dq == 0);
    uint8_t offset[] = {iq(2, 0), iq(2, 0)};
    predemod_dc_mcells(offset, 2, &di, &dq);
    assert(di == 2500 && dq == 500);

    assert(predemod_dc_cal_point(5865, 1) == 5855);
    assert(predemod_dc_cal_point(5917, 1) == 5855);
    assert(predemod_dc_cal_point(5740, 1) == 5775);
    assert(predemod_dc_cal_point(5865, 0) == 2432);

    assert(predemod_filter_code(0xC0 | 20, 8) == (0xC0 | 28));
    assert(predemod_filter_code(55, 16) == 60);
    assert(predemod_filter_code(4, -8) == 0);

    /* Identity response: the step cancels the error, bounded per axis. */
    float j[4] = {1000.f, 0.f, 0.f, 1000.f};
    int a, b;
    assert(predemod_dco_step(j, 3000.f, -2000.f, 32, &a, &b) && a == -3 && b == 2);
    assert(predemod_dco_step(j, 90000.f, 0.f, 32, &a, &b) && a == -32 && b == 0);
    /* Cross-coupled response with swapped mapping still converges. */
    float swapped[4] = {0.f, -800.f, 1200.f, 0.f};
    assert(predemod_dco_step(swapped, 1600.f, 2400.f, 32, &a, &b));
    assert(fabsf(swapped[0] * a + swapped[1] * b + 1600.f) < 500.f);
    assert(fabsf(swapped[2] * a + swapped[3] * b + 2400.f) < 700.f);
    float singular[4] = {1.f, 2.f, 2.f, 4.f};
    assert(!predemod_dco_step(singular, 1.f, 1.f, 32, &a, &b));
    puts("PASS: glitch metric, DC centre, DC-cal point, relative filter code and DCO solver");
    return 0;
}
