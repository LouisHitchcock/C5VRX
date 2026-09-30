#include "agc_policy.h"
#include <assert.h>
#include <stdio.h>

static agcp_obs_t obs(bool carrier, uint8_t idx, uint8_t p50, uint8_t p95, uint16_t clip)
{
    agcp_obs_t o = {0};
    o.valid = true;
    o.carrier = carrier;
    o.native_idx = idx;
    o.p50 = p50;
    o.p95 = p95;
    o.clip_pm = clip;
    return o;
}

static agcp_cmd_t tick(agc_policy_t *p, agcp_obs_t o, bool *changed)
{
    return agc_policy_tick(p, &o, changed);
}

/* Toy plant: pinned gain g gives P50 = 6 at the native point 50 and roughly
 * doubles every 4 indices, the way a steady carrier would respond. */
static uint8_t plant_p50(uint8_t g)
{
    int p = 6;
    for (int k = 50; k < (int)g; k += 4) p *= 2;
    for (int k = 50; k > (int)g; k -= 4) p /= 2;
    return (uint8_t)(p > 113 ? 113 : p);
}

int main(void)
{
    agc_policy_t p;
    bool changed;
    agcp_cmd_t c;

    /* NATIVE: never writes, whatever it sees. */
    agc_policy_reset(&p, AGC_PROFILE_NATIVE, 2, 83);
    for (unsigned k = 0; k < 200; ++k) {
        c = tick(&p, obs(k & 1, 50, 6, 20, 500), &changed);
        assert(!changed && !c.force && !c.initgain && !c.rfsat_off);
    }
    assert(p.writes == 0);

    /* INIT: start gain follows the operating point + margin, with hysteresis. */
    agc_policy_reset(&p, AGC_PROFILE_INIT, 2, 83);
    assert(!p.cmd.rfsat_off);
    unsigned writes = 0;
    for (unsigned k = 0; k < 40; ++k) {
        c = tick(&p, obs(true, 50, 6, 20, 0), &changed);
        writes += changed;
        assert(!c.force);
    }
    assert(c.initgain == 50 + AGCP_INIT_MARGIN && writes == 1);
    for (unsigned k = 0; k < 60; ++k) c = tick(&p, obs(true, 52, 6, 20, 0), &changed);
    assert(c.initgain == 56);                        /* drift of 2: ignored */
    for (unsigned k = 0; k < 60; ++k) c = tick(&p, obs(true, 60, 6, 20, 0), &changed);
    assert(c.initgain == 66);                        /* fade: start follows up */
    for (unsigned k = 0; k < AGCP_LOSS_TICKS_INIT; ++k)
        c = tick(&p, obs(false, 82, 1, 4, 0), &changed);
    assert(c.initgain == 0);                         /* carrier lost: vendor top */
    /* Clamp to the minimum start gain for a very strong carrier. */
    for (unsigned k = 0; k < 60; ++k) c = tick(&p, obs(true, 20, 6, 20, 0), &changed);
    assert(c.initgain == AGCP_INIT_MIN);
    /* Invalid windows are ignored completely. */
    agcp_obs_t bad = obs(false, 82, 0, 0, 0);
    bad.valid = false;
    for (unsigned k = 0; k < 100; ++k) c = agc_policy_tick(&p, &bad, &changed);
    assert(c.initgain == AGCP_INIT_MIN && !changed);

    /* TUNED: INIT plus RF saturation intervention off from the start. */
    agc_policy_reset(&p, AGC_PROFILE_TUNED, 2, 83);
    assert(p.cmd.rfsat_off && !p.cmd.force);

    /* HOLD: acquire, pin at the native point, servo up into the annulus. */
    agc_policy_reset(&p, AGC_PROFILE_HOLD, 2, 83);
    for (unsigned k = 0; k + 1 < AGCP_ACQUIRE_TICKS; ++k) {
        c = tick(&p, obs(true, 50, 6, 20, 0), &changed);
        assert(!c.force);
    }
    c = tick(&p, obs(true, 50, 6, 20, 0), &changed);
    assert(changed && c.force && c.force_idx == 50 && p.phase == AGCP_HOLD);

    unsigned last_step = 0, steps = 0;
    for (unsigned k = 1; k <= 400; ++k) {
        uint8_t before = c.force_idx;
        uint8_t p50 = plant_p50(before);
        c = tick(&p, obs(true, 0, p50, (uint8_t)(p50 * 2 > 113 ? 113 : p50 * 2), 0), &changed);
        if (c.force_idx != before) {
            assert(c.force_idx == before + 1);                 /* one index */
            assert(last_step == 0 || k - last_step >= AGCP_SERVO_TICKS);
            last_step = k;
            ++steps;
        }
    }
    uint8_t settled = plant_p50(c.force_idx);
    assert(settled >= AGCP_P50_LOW && settled <= AGCP_P50_HIGH);
    assert(steps > 0 && steps < 12);
    /* Settled: no further writes while in band. */
    uint32_t w = p.writes;
    for (unsigned k = 0; k < 200; ++k) {
        uint8_t p50 = plant_p50(c.force_idx);
        c = tick(&p, obs(true, 0, p50, p50 * 2, 0), &changed);
    }
    assert(p.writes == w);

    /* Overload: severe clipping steps down by two quickly. */
    uint8_t held = c.force_idx;
    c = tick(&p, obs(true, 0, 60, 113, 300), &changed);
    c = tick(&p, obs(true, 0, 60, 113, 300), &changed);
    assert(c.force_idx == held - 2);

    /* Short dropout (< release time) keeps the pin. */
    for (unsigned k = 0; k + 1 < AGCP_LOSS_TICKS_HOLD; ++k)
        c = tick(&p, obs(false, 0, 2, 6, 0), &changed);
    assert(c.force);
    c = tick(&p, obs(true, 0, 20, 40, 0), &changed);
    assert(c.force);
    /* Real carrier loss: release so native AGC climbs to high gain. */
    for (unsigned k = 0; k < AGCP_LOSS_TICKS_HOLD; ++k)
        c = tick(&p, obs(false, 0, 2, 6, 0), &changed);
    assert(!c.force && p.phase == AGCP_RELEASED);

    /* A VTX that clips before P50 reaches the band must not make HOLD hunt:
     * clip grows with gain; at most one step down, then it stays put. */
    agc_policy_reset(&p, AGC_PROFILE_HOLD, 2, 83);
    for (unsigned k = 0; k < AGCP_ACQUIRE_TICKS; ++k)
        c = tick(&p, obs(true, 35, 6, 20, 0), &changed);
    assert(c.force && c.force_idx == 35);
    uint32_t start_writes = p.writes;
    for (unsigned k = 0; k < 2000; ++k) {       /* 100 s */
        int g = c.force_idx;
        uint16_t clip = g >= 43 ? 30 : g >= 41 ? 10 : 0;   /* clips at P50 ~13 */
        c = tick(&p, obs(true, 0, (uint8_t)(g - 30), (uint8_t)(g - 10), clip), &changed);
    }
    assert(c.force_idx >= 40 && c.force_idx <= 42);
    assert(p.writes - start_writes <= 14);

    /* The pin never leaves the table. */
    agc_policy_reset(&p, AGC_PROFILE_HOLD, 2, 83);
    for (unsigned k = 0; k < AGCP_ACQUIRE_TICKS; ++k)
        c = tick(&p, obs(true, 83, 1, 4, 0), &changed);
    for (unsigned k = 0; k < 100; ++k) c = tick(&p, obs(true, 0, 1, 4, 0), &changed);
    assert(c.force && c.force_idx == 83);

    printf("agc_policy: all checks passed\n");
    return 0;
}
