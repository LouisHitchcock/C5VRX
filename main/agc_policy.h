#pragma once
#include <stdbool.h>
#include <stdint.h>

/*
 * Native AGC policy profiles (#121 follow-up). Pure logic, host-tested by
 * tools/test_agc_policy.c. The caller ticks it every 50 ms control window and
 * applies the returned command through rf_native_policy_*().
 *
 * Bench facts this is built on (docs/native-agc-v2.md):
 *  - the vendor packet AGC restarts from its start gain (82) 1-4x per ms and
 *    walks down to its operating point, i.e. many gain steps inside every
 *    video line; each step requantizes Q4 and becomes a fake phase delta;
 *  - it parks the Q4 vector at P50 ~5-7 (radius ~2.5 cells), too small for
 *    Phase8 (grain, blue blacks); P50 13..32 is the provisional healthy band;
 *  - pinning the gain (force bit, AGC left enabled) gave a perfectly steady
 *    Q4 circle and zero phase jumps.
 *
 * Profiles, from least to most intervention:
 *  NATIVE  vendor behaviour, zero writes (production default).
 *  INIT    zero gain writes. Once a carrier is stable, the start gain follows
 *          the operating point (+AGCP_INIT_MARGIN), so every restart is a small
 *          step instead of a swing from 82. Carrier loss restores the vendor
 *          start so a new or weak signal is acquired from the top.
 *  TUNED   INIT plus the RF saturation intervention (0x600A705C) switched
 *          off, as ESPARGOS esp-sdr does for stable gain. Zero gain writes.
 *  HOLD    acquire + hold. Native AGC finds the operating point, then the
 *          gain is pinned there and servoed slowly (one index per
 *          AGCP_SERVO_TICKS at most) into the Q4 annulus. Carrier loss
 *          releases the pin so native AGC re-acquires from the top: the
 *          NO_CARRIER -> high-gain survival rule stays intact. This is a slow
 *          CPU gain decision and therefore an explicit, opt-in exception to
 *          the #121 "zero CPU gain writes" rule.
 *
 * Gain-index-to-dB is not linear or known, so HOLD never predicts a
 * destination: it steps and measures.
 */

typedef enum {
    AGC_PROFILE_NATIVE = 0,
    AGC_PROFILE_INIT = 1,
    AGC_PROFILE_TUNED = 2,
    AGC_PROFILE_HOLD = 3,
    AGC_PROFILE_COUNT
} agc_profile_t;

typedef enum {
    AGCP_RELEASED = 0,  /* native AGC runs free */
    AGCP_HOLD,          /* gain pinned (HOLD profile only) */
} agcp_phase_t;

#define AGCP_RING               16u
#define AGCP_INIT_MARGIN         6u   /* start gain above the operating point */
#define AGCP_INIT_MIN           40u
#define AGCP_INIT_HYST           4u   /* ignore smaller operating-point drift */
#define AGCP_INIT_PERIOD_TICKS  20u   /* re-evaluate the start gain every 1 s */
#define AGCP_ACQUIRE_TICKS      10u   /* 0.5 s of carrier before pinning */
#define AGCP_LOSS_TICKS_INIT    20u   /* 1 s without carrier: vendor start */
#define AGCP_LOSS_TICKS_HOLD     6u   /* 0.3 s without carrier: release pin */
#define AGCP_SERVO_TICKS         6u   /* at most one normal step per 0.3 s */
/* Uncentered Q4 power (i*i + q*q), as analyze_control_window() reports.
 * Provisional band; tools/p8env_sweep.py measures the empirical annulus. */
#define AGCP_P50_LOW            20u
#define AGCP_P50_HIGH           36u
#define AGCP_P95_MAX            64u
#define AGCP_CLIP_PM            20u   /* step down at or above this */
#define AGCP_CLIP_UP_PM          5u   /* step up only below this */
#define AGCP_CLIP_SEVERE_PM     80u
/* After a step down for headroom, no step up for 10 s. Without it a VTX that
 * clips before P50 reaches the band makes HOLD hunt +1/-1 forever, one gain
 * write mid-picture every 0.3 s (seen on hardware: 48 writes in 40 s). */
#define AGCP_UP_BLOCK_TICKS    200u

typedef struct {
    bool valid;          /* fresh window, no settle/context change */
    bool carrier;        /* coherent FM carrier present */
    uint8_t native_idx;  /* median native state byte; only while released */
    uint8_t p50, p95;    /* uncentered Q4 power */
    uint16_t clip_pm;
} agcp_obs_t;

typedef struct {
    uint8_t initgain;    /* 0 = vendor start gain */
    bool rfsat_off;
    bool force;
    uint8_t force_idx;
} agcp_cmd_t;

typedef struct {
    agc_profile_t profile;
    agcp_phase_t phase;
    uint8_t min_idx, max_idx;
    uint8_t ring[AGCP_RING];
    uint8_t ring_n, ring_pos;
    uint16_t carrier_ticks, loss_ticks, period_ticks, servo_ticks, up_block_ticks;
    agcp_cmd_t cmd;
    uint32_t writes;     /* commands that changed hardware state */
} agc_policy_t;

static inline void agcp_ring_clear(agc_policy_t *p)
{
    p->ring_n = 0;
    p->ring_pos = 0;
}

static inline void agcp_ring_push(agc_policy_t *p, uint8_t v)
{
    p->ring[p->ring_pos] = v;
    p->ring_pos = (uint8_t)((p->ring_pos + 1u) % AGCP_RING);
    if (p->ring_n < AGCP_RING) ++p->ring_n;
}

static inline uint8_t agcp_ring_median(const agc_policy_t *p)
{
    uint8_t s[AGCP_RING];
    unsigned n = p->ring_n;
    for (unsigned i = 0; i < n; ++i) {
        uint8_t v = p->ring[i];
        unsigned j = i;
        for (; j > 0 && s[j - 1] > v; --j) s[j] = s[j - 1];
        s[j] = v;
    }
    return n ? s[n / 2u] : 0u;
}

static inline void agc_policy_reset(agc_policy_t *p, agc_profile_t profile,
                                    uint8_t min_idx, uint8_t max_idx)
{
    *p = (agc_policy_t){0};
    p->profile = profile < AGC_PROFILE_COUNT ? profile : AGC_PROFILE_NATIVE;
    p->min_idx = min_idx;
    p->max_idx = max_idx;
    p->cmd.rfsat_off = p->profile == AGC_PROFILE_TUNED;
}

static inline uint8_t agcp_clamp(int v, int lo, int hi)
{
    return (uint8_t)(v < lo ? lo : v > hi ? hi : v);
}

static inline bool agcp_cmd_equal(const agcp_cmd_t *a, const agcp_cmd_t *b)
{
    return a->initgain == b->initgain && a->rfsat_off == b->rfsat_off &&
           a->force == b->force && (!a->force || a->force_idx == b->force_idx);
}

/* Returns the command to apply; *changed tells whether it differs from the
 * previous one (the caller then writes hardware and should settle). */
static inline agcp_cmd_t agc_policy_tick(agc_policy_t *p, const agcp_obs_t *o,
                                         bool *changed)
{
    const agcp_cmd_t before = p->cmd;
    *changed = false;
    if (p->profile == AGC_PROFILE_NATIVE || !o->valid) return p->cmd;

    if (o->carrier) {
        if (p->carrier_ticks < 0xFFFFu) ++p->carrier_ticks;
        p->loss_ticks = 0;
    } else {
        if (p->loss_ticks < 0xFFFFu) ++p->loss_ticks;
        p->carrier_ticks = 0;
    }

    if (p->profile == AGC_PROFILE_INIT || p->profile == AGC_PROFILE_TUNED) {
        if (o->carrier) agcp_ring_push(p, o->native_idx);
        if (p->loss_ticks >= AGCP_LOSS_TICKS_INIT) {
            p->cmd.initgain = 0;
            agcp_ring_clear(p);
            p->period_ticks = 0;
        } else if (++p->period_ticks >= AGCP_INIT_PERIOD_TICKS) {
            p->period_ticks = 0;
            if (p->ring_n == AGCP_RING && p->carrier_ticks >= AGCP_RING) {
                uint8_t target = agcp_clamp(agcp_ring_median(p) + (int)AGCP_INIT_MARGIN,
                                            AGCP_INIT_MIN, p->max_idx);
                int diff = (int)target - (int)p->cmd.initgain;
                if (p->cmd.initgain == 0 || diff >= (int)AGCP_INIT_HYST ||
                    diff <= -(int)AGCP_INIT_HYST)
                    p->cmd.initgain = target;
            }
        }
    } else if (p->phase == AGCP_RELEASED) {
        /* HOLD profile, acquiring: learn where native AGC settles. */
        if (o->carrier) agcp_ring_push(p, o->native_idx);
        else agcp_ring_clear(p);
        if (p->carrier_ticks >= AGCP_ACQUIRE_TICKS && p->ring_n >= AGCP_ACQUIRE_TICKS) {
            p->phase = AGCP_HOLD;
            p->cmd.force = true;
            p->cmd.force_idx = agcp_clamp(agcp_ring_median(p), p->min_idx, p->max_idx);
            p->servo_ticks = 0;
        }
    } else {
        /* HOLD profile, pinned. Carrier loss hands control back to native
         * AGC, which climbs to high gain by itself. */
        if (p->loss_ticks >= AGCP_LOSS_TICKS_HOLD) {
            p->phase = AGCP_RELEASED;
            p->cmd.force = false;
            agcp_ring_clear(p);
        } else if (o->carrier) {
            ++p->servo_ticks;
            if (p->up_block_ticks) --p->up_block_ticks;
            int step = 0;
            if (o->clip_pm >= AGCP_CLIP_SEVERE_PM) {
                if (p->servo_ticks >= 2u) step = -2;       /* overload: fast */
            } else if (p->servo_ticks >= AGCP_SERVO_TICKS) {
                if (o->clip_pm >= AGCP_CLIP_PM || o->p95 > AGCP_P95_MAX ||
                    o->p50 > AGCP_P50_HIGH) step = -1;
                else if (o->p50 < AGCP_P50_LOW && o->clip_pm < AGCP_CLIP_UP_PM &&
                         !p->up_block_ticks) step = 1;
                else p->servo_ticks = 0;                   /* hold */
            }
            if (step < 0) p->up_block_ticks = AGCP_UP_BLOCK_TICKS;
            if (step) {
                uint8_t next = agcp_clamp((int)p->cmd.force_idx + step,
                                          p->min_idx, p->max_idx);
                p->servo_ticks = 0;
                p->cmd.force_idx = next;
            }
        }
    }

    *changed = !agcp_cmd_equal(&before, &p->cmd);
    if (*changed) ++p->writes;
    return p->cmd;
}

static inline const char *agc_profile_name(agc_profile_t profile)
{
    switch (profile) {
    case AGC_PROFILE_INIT:  return "INIT";
    case AGC_PROFILE_TUNED: return "TUNED";
    case AGC_PROFILE_HOLD:  return "HOLD";
    case AGC_PROFILE_NATIVE:
    default:                return "NATIVE";
    }
}
