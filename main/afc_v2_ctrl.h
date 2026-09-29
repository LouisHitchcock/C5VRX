#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "afc_v2.h"

/*
 * Issue #115 AFC V2 acquisition decision on top of afc2_measure().
 *
 * Only used in AUTO AFC (default OFF) and only outside TRACK. It never uses
 * the scene-biased WBFM slope. A correction requires:
 *  - AFC2_CTRL_SAMPLES burst-confirmed window estimates since the last reset,
 *    all with the same sync polarity and video standard;
 *  - a stable estimate: median absolute deviation <= AFC2_CTRL_MAX_MAD_KHZ;
 *  - |median| > AFC2_CTRL_DEADBAND_KHZ.
 * The step is clamped to +/-AFC2_CTRL_MAX_STEP_KHZ and at most
 * AFC2_CTRL_MAX_CORRECTIONS are made per acquisition context; every write
 * changes the frequency offset, which resets the caller's context so only
 * fresh measurements count afterwards.
 *
 * Reference model: the burst-free porch (blanking) level is treated as the
 * carrier centre. #115 section 9 requires confirming this for the VTX on
 * hardware; AFC2_REF_SYNC_MID offers the sync/porch midpoint instead.
 * Sign: a positive estimate means the signal sits above the receiver centre,
 * corrected by raising the offset (same convention as the legacy estimator).
 */

#define AFC2_CTRL_SAMPLES          16u
#define AFC2_CTRL_MAX_MAD_KHZ      80
#define AFC2_CTRL_DEADBAND_KHZ     50
#define AFC2_CTRL_MAX_STEP_KHZ     250
#define AFC2_CTRL_MAX_CORRECTIONS  4u

typedef enum {
    AFC2_REF_PORCH = 0,
    AFC2_REF_SYNC_MID,
} afc2_ref_t;

typedef struct {
    uint32_t context;
    int32_t est[AFC2_CTRL_SAMPLES];
    uint8_t n;
    int8_t polarity;
    uint8_t standard;
    uint8_t corrections;
    afc2_ref_t ref;
} afc2_ctrl_t;

static inline void afc2_ctrl_reset(afc2_ctrl_t *c, uint32_t context, bool new_acquisition)
{
    c->context = context;
    c->n = 0;
    c->polarity = 0;
    c->standard = 0;
    if (new_acquisition) c->corrections = 0;
}

/* Returns true when the context changed (samples discarded). A context change
 * caused by our own correction keeps the correction count. */
static inline bool afc2_ctrl_sync(afc2_ctrl_t *c, uint32_t context, bool own_write)
{
    if (c->context == context) return false;
    afc2_ctrl_reset(c, context, !own_write);
    return true;
}

static inline void afc2_ctrl_observe(afc2_ctrl_t *c, const afc2_result_t *r)
{
    if (!r || !r->lines || !r->standard || !r->polarity) return;
    if (c->n && (r->polarity != c->polarity || r->standard != c->standard)) c->n = 0;
    c->polarity = r->polarity;
    c->standard = r->standard;
    int32_t v = c->ref == AFC2_REF_SYNC_MID ? (r->sync_khz + r->porch_khz) / 2 : r->porch_khz;
    if (c->n < AFC2_CTRL_SAMPLES) {
        c->est[c->n++] = v;
    } else {
        for (unsigned k = 1; k < AFC2_CTRL_SAMPLES; ++k) c->est[k - 1] = c->est[k];
        c->est[AFC2_CTRL_SAMPLES - 1] = v;
    }
}

static inline int32_t afc2_ctrl_median(const int32_t *v, unsigned n)
{
    int32_t s[AFC2_CTRL_SAMPLES];
    for (unsigned k = 0; k < n; ++k) s[k] = v[k];
    for (unsigned a = 1; a < n; ++a)
        for (unsigned b = a; b > 0 && s[b - 1] > s[b]; --b) {
            int32_t t = s[b]; s[b] = s[b - 1]; s[b - 1] = t;
        }
    return n ? (n & 1u ? s[n / 2] : (s[n / 2 - 1] + s[n / 2]) / 2) : 0;
}

/* Decide one acquisition step. `eligible` is the caller's gate (AUTO AFC,
 * not TRACK, no settle). Returns true with *step_khz when a write is due. */
static inline bool afc2_ctrl_decide(afc2_ctrl_t *c, bool eligible, int32_t *step_khz)
{
    if (!eligible || c->n < AFC2_CTRL_SAMPLES ||
        c->corrections >= AFC2_CTRL_MAX_CORRECTIONS) return false;
    int32_t med = afc2_ctrl_median(c->est, c->n);
    int32_t dev[AFC2_CTRL_SAMPLES];
    for (unsigned k = 0; k < c->n; ++k) dev[k] = c->est[k] > med ? c->est[k] - med : med - c->est[k];
    int32_t mad = afc2_ctrl_median(dev, c->n);
    if (mad > AFC2_CTRL_MAX_MAD_KHZ) return false;
    if (med <= AFC2_CTRL_DEADBAND_KHZ && med >= -AFC2_CTRL_DEADBAND_KHZ) return false;
    int32_t step = med > AFC2_CTRL_MAX_STEP_KHZ ? AFC2_CTRL_MAX_STEP_KHZ :
                   med < -AFC2_CTRL_MAX_STEP_KHZ ? -AFC2_CTRL_MAX_STEP_KHZ : med;
    *step_khz = step;
    ++c->corrections;
    c->n = 0;
    return true;
}
