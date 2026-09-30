#pragma once
#include <stdbool.h>
#include <stdint.h>

/*
 * Self-calibrating native AGC level offset. Pure logic, host-tested by
 * tools/test_agc_offset.c.
 *
 * The native packet AGC makes every gain decision in hardware; on a carrier
 * it re-acquires 1-2x per video line and settles with the Q4 vector at P50
 * ~5-7 (radius ~2.5 cells), too small for Phase8. This calibrator only moves
 * the AGC's own level field (rf_agc_offset_set, dB against vendor) once per
 * second so that the median trapped envelope lands in the Q4 band. It never
 * writes or pins a gain.
 *
 *  - measures: median uncentered Q4 P50 over AOC_WINDOW carrier windows,
 *    plus how many of them clipped;
 *  - steps:    one dB per window, only outside the dead band, clip first;
 *  - learns:   the field's polarity (which sign raises the settled level)
 *              from the envelope's response to its own steps;
 *  - bounded:  +-AOC_LIMIT dB around vendor;
 *  - saves:    the caller persists after AOC_SAVE_WINDOWS quiet windows.
 */

#define AOC_WINDOW        20u   /* 50 ms ticks: 1 s */
#define AOC_P50_LOW       10u   /* coarse uncentered power: radius ~3.2 cells */
#define AOC_P50_HIGH      22u   /* radius ~4.7 cells */
#define AOC_CLIP_PM       20u
#define AOC_CLIP_WINDOWS   5u   /* clipping if >= 5 of 20 windows clipped */
#define AOC_LIMIT         12
#define AOC_RUN_STEPS      3u   /* same-direction steps before judging polarity */
#define AOC_SAVE_WINDOWS  60u   /* persist after a quiet minute */

typedef struct {
    bool enabled;
    int8_t db;
    int8_t polarity;        /* +1: +dB raises the settled level */
    uint8_t n;
    uint8_t p50[AOC_WINDOW];
    uint8_t clip_windows;
    int8_t last_step;       /* requested level direction of the last change */
    int8_t run_dir;         /* direction of the current run of steps */
    uint8_t run_len;
    uint8_t run_start_med;  /* median before the run's first step */
    uint16_t quiet;
    bool dirty;
    uint32_t steps, flips;
} agc_offset_cal_t;

static inline void aoc_reset(agc_offset_cal_t *c, bool enabled, int db)
{
    *c = (agc_offset_cal_t){0};
    c->enabled = enabled;
    c->db = (int8_t)(db < -AOC_LIMIT ? -AOC_LIMIT : db > AOC_LIMIT ? AOC_LIMIT : db);
    c->polarity = 1;
}

static inline uint8_t aoc_median(const agc_offset_cal_t *c)
{
    uint8_t s[AOC_WINDOW];
    for (unsigned i = 0; i < c->n; ++i) {
        uint8_t v = c->p50[i];
        unsigned j = i;
        for (; j > 0 && s[j - 1] > v; --j) s[j] = s[j - 1];
        s[j] = v;
    }
    return c->n ? s[c->n / 2u] : 0u;
}

/* One 50 ms control window. Returns true when db changed (caller applies
 * rf_agc_offset_set(c->db)). *save is set when the caller should persist. */
static inline bool aoc_tick(agc_offset_cal_t *c, bool valid, bool carrier,
                            uint8_t p50, uint16_t clip_pm, bool *save)
{
    *save = false;
    if (!c->enabled || !valid || !carrier) return false;
    c->p50[c->n++] = p50;
    if (clip_pm >= AOC_CLIP_PM) ++c->clip_windows;
    if (c->n < AOC_WINDOW) return false;

    uint8_t med = aoc_median(c);
    bool clipping = c->clip_windows >= AOC_CLIP_WINDOWS;
    c->n = 0;
    c->clip_windows = 0;

    /* Learn polarity: after a run of steps in one direction the envelope
     * must have moved that way; if it moved against it, the field is
     * inverted. One dB steps are small, so judge the run, not a step. */
    if (c->last_step && c->run_len >= AOC_RUN_STEPS &&
        ((int)med - (int)c->run_start_med) * c->run_dir < 0) {
        c->polarity = (int8_t)-c->polarity;
        c->run_len = 0;
        ++c->flips;
    }

    int want = 0;                 /* +1: raise the settled level */
    if (clipping || med > AOC_P50_HIGH) want = -1;
    else if (med < AOC_P50_LOW) want = 1;

    int next = c->db + want * c->polarity;
    if (next < -AOC_LIMIT) next = -AOC_LIMIT;
    if (next > AOC_LIMIT) next = AOC_LIMIT;
    if (!want || next == c->db) {
        c->last_step = 0;
        c->run_len = 0;
        if (c->dirty && ++c->quiet >= AOC_SAVE_WINDOWS) {
            c->dirty = false;
            c->quiet = 0;
            *save = true;
        }
        return false;
    }
    if (c->run_len == 0 || c->run_dir != want) {
        c->run_dir = (int8_t)want;
        c->run_start_med = med;
        c->run_len = 0;
    }
    ++c->run_len;
    c->db = (int8_t)next;
    c->last_step = (int8_t)want;
    c->dirty = true;
    c->quiet = 0;
    ++c->steps;
    return true;
}
