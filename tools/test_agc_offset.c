#include "agc_offset.h"
#include <assert.h>
#include <stdio.h>

/* Plant: the native AGC settles at P50 = base * 10^(k*db/10) (power), with
 * k = +1 for a field that raises the level and -1 for an inverted field. */
static uint8_t plant(int db, int k, double base)
{
    double p = base;
    for (int i = 0; i < (db * k > 0 ? db * k : -db * k); ++i)
        p = db * k > 0 ? p * 1.2589 : p / 1.2589;
    return (uint8_t)(p > 113.0 ? 113.0 : p + 0.5);
}

static int run(agc_offset_cal_t *c, int k, double base, unsigned seconds, unsigned *saves)
{
    bool save;
    for (unsigned t = 0; t < seconds * AOC_WINDOW; ++t) {
        uint8_t p = plant(c->db, k, base);
        aoc_tick(c, true, true, p, p > 90 ? 50u : 0u, &save);
        if (save && saves) ++*saves;
    }
    return plant(c->db, k, base);
}

int main(void)
{
    agc_offset_cal_t c;
    bool save;

    /* Disabled: never moves. */
    aoc_reset(&c, false, 0);
    assert(run(&c, 1, 6.0, 30, NULL) == 6 && c.db == 0);

    /* Native parks at P50 6: the offset raises it into the band, then stops. */
    aoc_reset(&c, true, 0);
    unsigned saves = 0;
    int p = run(&c, 1, 6.0, 30, &saves);
    assert(p >= (int)AOC_P50_LOW && p <= (int)AOC_P50_HIGH);
    assert(c.db > 0 && c.db <= 5 && c.flips == 0);
    uint32_t steps = c.steps;
    run(&c, 1, 6.0, 120, &saves);
    assert(c.steps == steps);              /* in band: no further changes */
    assert(saves == 1);                    /* persisted once after a quiet minute */

    /* Inverted field: learns the polarity and still converges. */
    aoc_reset(&c, true, 0);
    p = run(&c, -1, 6.0, 60, NULL);
    assert(p >= (int)AOC_P50_LOW && p <= (int)AOC_P50_HIGH);
    assert(c.flips == 1 && c.polarity == -1);

    /* Too strong / clipping: steps down. */
    aoc_reset(&c, true, 0);
    p = run(&c, 1, 100.0, 60, NULL);
    assert(p <= (int)AOC_P50_HIGH && c.db < 0);

    /* Bounded. */
    aoc_reset(&c, true, 0);
    run(&c, 1, 0.5, 60, NULL);
    assert(c.db == AOC_LIMIT);

    /* No carrier or invalid windows: no measurement at all. */
    aoc_reset(&c, true, 3);
    for (unsigned t = 0; t < 400; ++t) {
        assert(!aoc_tick(&c, true, false, 1, 0, &save));
        assert(!aoc_tick(&c, false, true, 1, 0, &save));
    }
    assert(c.db == 3 && c.n == 0);

    printf("agc_offset: all checks passed\n");
    return 0;
}
