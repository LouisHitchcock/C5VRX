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
