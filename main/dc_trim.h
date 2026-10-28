/* C5VRX by Twotoz/contributors: bounded live-carrier DC trial decisions.
 * The actual caller is calibration_dc.c drift_service; no per-sample CPU path. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
/* The receiver DC wanders about half a cell by itself (board 2026-10-19);
 * 300 made the trim hunt (2 of 3 trials rejected, each one resetting the
 * gain epoch). 600 mcells is ~0.3 of the noise per axis at the range edge,
 * where research/sensitivity_model.py finds no measurable cost. */
#define DC_TRIM_DEADBAND_MCELLS 600
#define DC_TRIM_SETTLE_US 300u
/* Trial check (settle, a fresh window, the sums): 2 ms expired on every
 * trial on the board (2026-10-19, deadlines == trials). */
#define DC_TRIM_DEADLINE_US 8000u
#define DC_TRIM_PERIOD_US 2000000u
typedef struct {
    uint32_t gain, gain_epoch, control, phy, table, frequency, offset,
             filter_code, filter_skirt, bw20, lane, scale, packed_gain;
} dc_trim_context_t;
typedef struct {
    dc_trim_context_t context;
    int pair[2], center[2], direction[2];
    bool have;
} dc_trim_t;
static inline bool dc_trim_same(const dc_trim_context_t *a, const dc_trim_context_t *b)
{
    return !memcmp(a, b, sizeof(*a));
}
static inline uint64_t dc_trim_cost(const int center[2])
{
    return (uint64_t)((int64_t)center[0] * center[0]) + (uint64_t)((int64_t)center[1] * center[1]);
}
static inline bool dc_trim_improves(const int before[2], const int after[2])
{
    uint64_t a = dc_trim_cost(before), b = dc_trim_cost(after);
    return a >= b + 20000u && b * 10u <= a * 9u;
}
static inline int dc_trim_abs(int x) { return x < 0 ? -x : x; }
