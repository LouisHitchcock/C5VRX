/* C5VRX by Twotoz and contributors: pre-demodulation evidence helpers.
 * Pure functions over completed raw Q4/I4 bytes (I high nibble, Q low nibble,
 * signed). Observers only: nothing here touches the 40 MS/s path. */
#pragma once
#include <stddef.h>
#include <stdint.h>

static inline int predemod_i(uint8_t b) { return (int8_t)(b & 0xf0u) >> 4; }
static inline int predemod_q(uint8_t b) { return (int8_t)(uint8_t)(b << 4) >> 4; }
static inline int predemod_abs(int v) { return v < 0 ? -v : v; }

/* One axis of sample n is a glitch when it jumps at least `limit` cells away
 * from both neighbours while those neighbours agree within limit / 2. A read
 * that lands on a MODEM_DIAG transition mixes old and new bits; at a zero
 * crossing that turns -1/0 into -8/+7 (zerowidth PR #3, Logicenios link
 * monitor). A real carrier rotates far less in 25 ns, so the rate compares
 * sampling-phase positions; it is not an absolute error rate. */
static inline unsigned predemod_glitches(const uint8_t *s, size_t n, int limit)
{
    unsigned count = 0;
    for (size_t k = 1; k + 1 < n; ++k) {
        int a[3] = {predemod_i(s[k - 1]), predemod_i(s[k]), predemod_i(s[k + 1])};
        int b[3] = {predemod_q(s[k - 1]), predemod_q(s[k]), predemod_q(s[k + 1])};
        for (unsigned axis = 0; axis < 2; ++axis) {
            const int *x = axis ? b : a;
            if (predemod_abs(x[1] - x[0]) >= limit &&
                predemod_abs(x[1] - x[2]) >= limit &&
                predemod_abs(x[0] - x[2]) <= limit / 2) { ++count; break; }
        }
    }
    return count;
}

/* Cell-centre mean (2v+1)/2 per axis, in milli-cells of the current lane. */
static inline void predemod_dc_mcells(const uint8_t *s, size_t n, int *i, int *q)
{
    int32_t si = 0, sq = 0;
    for (size_t k = 0; k < n; ++k) {
        si += 2 * predemod_i(s[k]) + 1;
        sq += 2 * predemod_q(s[k]) + 1;
    }
    *i = n ? (int)(si * 500 / (int32_t)n) : 0;
    *q = n ? (int)(sq * 500 / (int32_t)n) : 0;
}

/* RX DC calibration point used by the pinned libphy for a 5 GHz channel.
 * phy_set_rx_gain_cal_dc() calibrates these seven frequencies when
 * phy_param[0x2a] != 0, otherwise only 2432 MHz (disassembly, IDF 6.0.2
 * esp-phy-lib 59c1234). FPV channels above 5855 MHz use the 5855 point. */
static inline uint16_t predemod_dc_cal_point(uint16_t mhz, int multi_point)
{
    static const uint16_t points[7] = {5210, 5290, 5530, 5610, 5690, 5775, 5855};
    if (!multi_point) return 2432u;
    uint16_t best = points[0];
    for (unsigned k = 1; k < 7u; ++k)
        if (predemod_abs((int)mhz - (int)points[k]) < predemod_abs((int)mhz - (int)best))
            best = points[k];
    return best;
}

/* Relative RC filter-capacitor step for BBTOP 0x67 registers 6..13: keep the
 * per-chip calibrated value as the base, add an offset in the 6-bit field and
 * saturate at 60, the code phy_11p_set() writes. Upper bits are preserved. */
static inline uint8_t predemod_filter_code(uint8_t calibrated, int offset)
{
    int code = (calibrated & 63) + offset;
    if (code < 0) code = 0;
    if (code > 60) code = 60;
    return (uint8_t)((calibrated & ~63u) | (unsigned)code);
}

/* Closed-loop DC step for two DC DACs with a measured 2x2 response matrix
 * (milli-cells per DAC code). Returns 0 when the matrix is ill-conditioned.
 * Steps are bounded by `limit` codes per axis. */
static inline int predemod_dco_step(const float j[4], float di, float dq,
                                    int limit, int *step_a, int *step_b)
{
    float det = j[0] * j[3] - j[1] * j[2];
    if (det > -1e-3f && det < 1e-3f) return 0;
    float a = (-j[3] * di + j[1] * dq) / det;
    float b = (j[2] * di - j[0] * dq) / det;
    int sa = (int)(a < 0 ? a - 0.5f : a + 0.5f);
    int sb = (int)(b < 0 ? b - 0.5f : b + 0.5f);
    if (sa > limit) sa = limit;
    if (sa < -limit) sa = -limit;
    if (sb > limit) sb = limit;
    if (sb < -limit) sb = -limit;
    *step_a = sa; *step_b = sb;
    return 1;
}

/* Static Phase8 decoder word for one raw IQ byte with the I/Q centre moved
 * by (di, dq) milli-cells of the current lane. Identical to
 * generate_phase8.py at (0, 0): cell centre +31.5/64, half-even rounding.
 * Odd LUT banks hold ((128 + p) & 255) | ((-p & 255) << 8). Digital
 * recentring only moves the decode geometry; it cannot restore samples that
 * folded or clipped before Q4. */
#include <math.h>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
static inline uint8_t predemod_phase8(uint8_t raw, int di, int dq)
{
    double i = predemod_i(raw) + 31.5 / 64 - di / 1000.0;
    double q = predemod_q(raw) + 31.5 / 64 - dq / 1000.0;
    return (uint8_t)((int)rint(atan2(q, i) * 128 / M_PI) & 255);
}
static inline uint16_t predemod_decoder_word(uint8_t raw, int di, int dq)
{
    uint8_t p = predemod_phase8(raw, di, dq);
    return (uint16_t)(((128u + p) & 255u) | (((256u - p) & 255u) << 8));
}

/* Decision for one evaluation of the measured centre (milli-cells). A new
 * centre is applied only after two consecutive evaluations agree within
 * `agree` and it differs from the applied one by at least `step` on an axis;
 * |centre| is clamped to `limit`. Returns 1 and writes *out when applying. */
typedef struct { int last[2]; int have_last; } predemod_dc_filter_t;
static inline int predemod_dc_decide(predemod_dc_filter_t *f, const int measured[2],
                                     const int applied[2], int agree, int step,
                                     int limit, int out[2])
{
    int m[2];
    for (unsigned a = 0; a < 2; ++a)
        m[a] = measured[a] > limit ? limit : measured[a] < -limit ? -limit : measured[a];
    int stable = f->have_last && predemod_abs(m[0] - f->last[0]) <= agree &&
                 predemod_abs(m[1] - f->last[1]) <= agree;
    int target[2] = {(m[0] + f->last[0]) / 2, (m[1] + f->last[1]) / 2};
    f->last[0] = m[0]; f->last[1] = m[1]; f->have_last = 1;
    if (!stable) return 0;
    if (predemod_abs(target[0]) < step && predemod_abs(target[1]) < step) {
        target[0] = target[1] = 0; /* deadband: stay on the pristine table */
    }
    if (target[0] == applied[0] && target[1] == applied[1]) return 0;
    if (predemod_abs(target[0] - applied[0]) < step &&
        predemod_abs(target[1] - applied[1]) < step &&
        (target[0] || target[1])) return 0;
    out[0] = target[0]; out[1] = target[1];
    return 1;
}
