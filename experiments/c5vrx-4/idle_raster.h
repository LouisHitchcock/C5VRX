/* C5VRX by Twotoz and contributors: no-carrier idle raster decision.
 *
 * Strict digital-path goggles (HDZero: TP2825 decoder, deinterlacer, scaler)
 * show a green screen and may switch PAL/NTSC when their decoder sees noise.
 * The HDZero firmware polls TP2825 register 0x01 every ~100 ms: a vertical
 * lock in the other standard on two consecutive polls switches the decoder
 * and display timing, and a fresh lock takes ten polls to count as locked
 * (hd-zero/hdzero-goggle src/driver/hardware-goggle*.c, AV_in_detect).
 * Demodulated receiver noise therefore costs a standard re-detection and a
 * relock when the transmitter appears. Analog modules usually give the
 * goggles a valid raster (their menu) instead.
 *
 * C5VRX-4 emits a clean black raster in the last live standard after a
 * sustained absence of any carrier, and returns to live video as soon as a
 * carrier or a sync is seen. The live path itself is unchanged: this never
 * repairs or regenerates a received picture, and a weak carrier with sync
 * fragments stays live. Pure function, host-tested. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define IDLE_RASTER_ENTER_TICKS 40u /* 2 s of 50-ms control ticks */
#define IDLE_RASTER_QUIET_Q     25  /* carrier coherence below: receiver noise */
#define IDLE_RASTER_CARRIER_Q   40  /* at or above: a carrier, return to live */
#define IDLE_RASTER_SYNC_TICKS  40u /* no sync for at least 2 s before entry */

typedef struct {
    bool active;
    uint16_t quiet_ticks;
    uint32_t entries, exits;
} idle_raster_t;

typedef struct {
    bool enabled;        /* NVS opt-out */
    bool owner_free;     /* live TX, no menu/lab/sweep/probe/pending calibration */
    bool survival_gain;  /* gain owner at its no-carrier (maximum) state */
    bool settling;       /* gain or context still settling */
    int q_phase;         /* carrier coherence 0..100 of the control window */
    bool fresh_sync;     /* a valid sync in this window */
    unsigned sync_age_ticks;
} idle_raster_obs_t;

typedef enum { IDLE_RASTER_STAY, IDLE_RASTER_ENTER, IDLE_RASTER_EXIT } idle_raster_action_t;

static inline idle_raster_action_t idle_raster_step(idle_raster_t *s, const idle_raster_obs_t *o)
{
    if (s->active) {
        /* Leave at the first sign of a transmitter: a weak picture is
         * always preferred over the idle raster. */
        if (!o->enabled || o->fresh_sync || o->q_phase >= IDLE_RASTER_CARRIER_Q) {
            s->active = false;
            s->quiet_ticks = 0;
            ++s->exits;
            return IDLE_RASTER_EXIT;
        }
        return IDLE_RASTER_STAY;
    }
    bool quiet = o->enabled && o->owner_free && o->survival_gain && !o->settling &&
                 !o->fresh_sync && o->sync_age_ticks >= IDLE_RASTER_SYNC_TICKS &&
                 o->q_phase < IDLE_RASTER_QUIET_Q;
    if (!quiet) { s->quiet_ticks = 0; return IDLE_RASTER_STAY; }
    if (++s->quiet_ticks < IDLE_RASTER_ENTER_TICKS) return IDLE_RASTER_STAY;
    s->active = true;
    s->quiet_ticks = 0;
    ++s->entries;
    return IDLE_RASTER_ENTER;
}

/* The firmware could not take over TX (menu memory) or a user/menu action
 * took it: forget the idle state without counting an exit to live. */
static inline void idle_raster_abandon(idle_raster_t *s)
{
    s->active = false;
    s->quiet_ticks = 0;
}
