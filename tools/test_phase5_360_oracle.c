#include <assert.h>
#include <stdio.h>
#include "phase5_360_oracle.h"

static int independent_wrap(int delta)
{
    while (delta < -16) delta += 32;
    while (delta > 15) delta -= 32;
    return delta;
}

int main(void)
{
    unsigned no_winding = 0, winding = 0, changed = 0, sync_flips = 0;
    for (unsigned i = 0; i < 1024; ++i)
        assert(s_phase5_360_golden_dac[i] == s_phase5_360_live_dac[i]);
    for (unsigned p = 0; p < 32; ++p) {
        for (unsigned m = 0; m < 32; ++m) {
            for (unsigned c = 0; c < 32; ++c) {
                int adjacent = independent_wrap((int)m - (int)p) +
                               independent_wrap((int)c - (int)m);
                int endpoint = independent_wrap((int)c - (int)p);
                int expected_winding = (adjacent - endpoint) / 32;
                /* Production P20/G2 is ~6 codes per Phase5 bin. */
                int linear = 20 + 6 * adjacent;
                if (linear < 0) linear = 0;
                if (linear > 63) linear = 63;
                uint8_t golden = s_phase5_360_golden_dac[(p << 5) | c];
                uint8_t expected = expected_winding ? (uint8_t)linear : golden;
                uint8_t actual = phase5_360_adjacent_dac(p, m, c);
                uint8_t live = phase5_360_live_dac(p, c);
                assert(phase5_360_winding(p, m, c) == expected_winding);
                assert(actual == expected);
                if (!expected_winding) {
                    ++no_winding;
                    assert(actual == golden);
                } else {
                    ++winding;
                }
                if (actual != live) ++changed;
                if ((actual <= 8) != (live <= 8)) ++sync_flips;
            }
        }
    }
    assert(no_winding == 24576 && winding == 8192);
    assert(phase5_360_adjacent_dac(0, 10, 20) !=
           phase5_360_adjacent_dac(0, 20, 20));
    assert(changed > 0 && sync_flips > 0);
    printf("Phase5-360 exact triplets: 32768; winding: %u; live DAC changes: %u; sync-tip flips: %u\n",
           winding, changed, sync_flips);
    return 0;
}
