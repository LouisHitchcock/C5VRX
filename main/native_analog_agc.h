#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Research policy, NOT a decoded amplitude target/hysteresis control.
 * Pinned bb_agc_reg_update writes 0x180 to 8020. The existing field lab
 * explores its low ten bits. Keep the first trial small and reversible. */
#define ANALOG_AGC_MASK 0x3ffu
#define ANALOG_AGC_VENDOR 384u
#define ANALOG_AGC_TRIAL_US INT64_C(20000000)

typedef struct {
    bool active;
    unsigned profile; /* 1 = raw +32, 2 = raw -32; neither direction is calibrated */
    uint32_t baseline, applied;
    int64_t deadline_us;
} native_analog_agc_t;

static inline bool native_analog_agc_begin(native_analog_agc_t *s, unsigned profile,
                                         uint32_t reg, int64_t now_us, bool eligible,
                                         uint32_t *write)
{
    if (!eligible || s->active || profile < 1u || profile > 2u ||
        (reg & ANALOG_AGC_MASK) != ANALOG_AGC_VENDOR) return false;
    s->baseline = reg & ANALOG_AGC_MASK;
    s->applied = profile == 1u ? s->baseline + 32u : s->baseline - 32u;
    s->profile = profile;
    s->deadline_us = now_us + ANALOG_AGC_TRIAL_US;
    s->active = true;
    *write = (reg & ~ANALOG_AGC_MASK) | s->applied;
    return true;
}

static inline bool native_analog_agc_end(native_analog_agc_t *s, uint32_t reg,
                                       uint32_t *write)
{
    if (!s->active) return false;
    s->active = false;
    /* Another owner may already have restored/reinitialized the PHY. Do not
     * overwrite its new field value; unrelated bits always use current MMIO. */
    if ((reg & ANALOG_AGC_MASK) != s->applied) return false;
    *write = (reg & ~ANALOG_AGC_MASK) | s->baseline;
    return true;
}

static inline bool native_analog_agc_poll(native_analog_agc_t *s, uint32_t reg,
                                        int64_t now_us, bool eligible, uint32_t *write)
{
    if (!s->active) return false;
    if (!eligible || now_us >= s->deadline_us ||
        (reg & ANALOG_AGC_MASK) != s->applied)
        return native_analog_agc_end(s, reg, write);
    return false; /* No tracking writes, gain decisions, or policy reassertion. */
}
