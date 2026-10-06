/* C5VRX by Twotoz and contributors: 10-bit band-power / SNR meter helpers.
 * Pure functions over full RF dump words (Q[9:0] bits 0..9, I[9:0] bits
 * 10..19, RX gain index bits 20..26), read from the dump bank after a short
 * MAC-owned window (snr_meter.c). Observers only: nothing here touches the
 * MODEM_DIAG / PARLIO video path.
 *
 * Method (SNR_METER.md): remove the capture mean, Welch PSD with a 64-point
 * Hann window and 50% overlap (1.25 MHz bins at 80 MS/s), then sum bins into
 * bands. Band power is what makes the reading steady on FM video: a
 * single-frequency or 4-bit view follows picture content, a ~20 MHz band holds
 * nearly all of the carrier's energy. Fixed point throughout (no FPU needed);
 * only the final per-bin scaling is floating point. */
#pragma once
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SNR_RING_WORDS   16384u        /* dump bank 0x40830000, 64 KiB */
#define SNR_SENTINEL     0xFFF5A5A5u   /* gain field 127: above any real index */
#define SNR_FFT          64u
#define SNR_FFT_LOG2     6u
#define SNR_HOP          (SNR_FFT / 2u)
#define SNR_MAX_SAMPLES  2048u
#define SNR_MIN_SAMPLES  512u
#define SNR_RATE_HZ      80000000.0
#define SNR_BIN_HZ       (SNR_RATE_HZ / SNR_FFT)
/* In-band: |f| <= 10 MHz (bins -8..8, mean removed, so bin 0 is the
 * carrier's slow content only). Edge: 12.5..20 MHz on both sides. */
#define SNR_IN_BINS      8
#define SNR_EDGE_LO_BIN  10
#define SNR_EDGE_HI_BIN  16
/* x = I + jQ with I from bits 10..19. FPVGateC5MK measured that RF above the
 * LO lands at negative frequency for x = b0 + j*b1 (bits 0..9 as the real
 * part); C5VRX's naming swaps the axes, x = j*conj(that), so here RF above
 * the LO lands at positive frequency. Verify on a new board: '7' with a fine
 * offset (',' / '.') moves the energy towards the matching half. */
#define SNR_SPECTRUM_SIGN (+1)
#define SNR_CLIP_HI      511
#define SNR_CLIP_LO      (-512)

static inline int snr_word_q(uint32_t w) { return (int32_t)(w << 22) >> 22; }
static inline int snr_word_i(uint32_t w) { return (int32_t)(w << 12) >> 22; }
static inline unsigned snr_word_gain(uint32_t w) { return (w >> 20) & 0x7fu; }

/* Longest circular run of non-sentinel words in a ring that was filled with
 * the sentinel before the dump window opened: the words written during that
 * window. Returns its length and start; 0 when the ring was untouched. */
static inline size_t snr_fresh_run(const volatile uint32_t *ring, size_t words,
                                   uint32_t sentinel, size_t *start)
{
    size_t anchor = words;
    for (size_t k = 0; k < words; ++k)
        if (ring[k] == sentinel) { anchor = k; break; }
    if (anchor == words) { *start = 0; return words; } /* fully overwritten */
    size_t best = 0, best_start = 0, run = 0, run_start = 0;
    for (size_t step = 1; step <= words; ++step) {
        size_t k = (anchor + step) % words;
        if (ring[k] != sentinel) {
            if (!run) run_start = k;
            if (++run > best) { best = run; best_start = run_start; }
        } else {
            run = 0;
        }
    }
    *start = best_start;
    return best;
}

typedef struct {
    size_t samples, segments;
    unsigned clips, gain_min, gain_max;
    float dc_i, dc_q;               /* LSB */
    float total, in_band, edge;     /* LSB^2 (complex sample power) */
    float lower, upper;             /* in-band halves, RF below / above LO */
    int peak_bin;                   /* signed, RF orientation */
    float psd[SNR_FFT];             /* LSB^2 per bin, index = FFT bin */
} snr_result_t;

static inline float snr_db(float power)
{
    return 10.0f * log10f(power > 1e-6f ? power : 1e-6f);
}

/* Bin k (0..63) to a signed bin in RF orientation. */
static inline int snr_rf_bin(unsigned k)
{
    int f = k < SNR_FFT / 2u ? (int)k : (int)k - (int)SNR_FFT;
    return SNR_SPECTRUM_SIGN * f;
}

typedef struct { int32_t re, im; } snr_cplx_t;

static int16_t s_snr_hann[SNR_FFT];      /* Q14 */
static int16_t s_snr_cos[SNR_FFT / 2u];  /* Q14 twiddles e^{-j2pi k/N} */
static int16_t s_snr_sin[SNR_FFT / 2u];
static bool s_snr_tables;

static inline void snr_tables_init(void)
{
    if (s_snr_tables) return;
    const double pi = 3.14159265358979323846;
    for (unsigned n = 0; n < SNR_FFT; ++n)
        s_snr_hann[n] = (int16_t)lround(16384.0 * 0.5 * (1.0 - cos(2.0 * pi * n / SNR_FFT)));
    for (unsigned k = 0; k < SNR_FFT / 2u; ++k) {
        s_snr_cos[k] = (int16_t)lround(16384.0 * cos(2.0 * pi * k / SNR_FFT));
        s_snr_sin[k] = (int16_t)lround(-16384.0 * sin(2.0 * pi * k / SNR_FFT));
    }
    s_snr_tables = true;
}

/* In-place radix-2 DIT FFT. Inputs up to 2^23 grow by at most 2^6, so int32
 * holds every stage; twiddle products use int64. */
static inline void snr_fft64(snr_cplx_t *x)
{
    for (unsigned i = 1, j = 0; i < SNR_FFT; ++i) {
        unsigned bit = SNR_FFT >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j |= bit;
        if (i < j) { snr_cplx_t t = x[i]; x[i] = x[j]; x[j] = t; }
    }
    for (unsigned len = 2; len <= SNR_FFT; len <<= 1) {
        const unsigned half = len >> 1, stride = SNR_FFT / len;
        for (unsigned base = 0; base < SNR_FFT; base += len) {
            for (unsigned k = 0; k < half; ++k) {
                const int64_t c = s_snr_cos[k * stride], s = s_snr_sin[k * stride];
                snr_cplx_t *a = &x[base + k], *b = &x[base + k + half];
                const int32_t tr = (int32_t)((b->re * c - b->im * s) >> 14);
                const int32_t ti = (int32_t)((b->re * s + b->im * c) >> 14);
                b->re = a->re - tr; b->im = a->im - ti;
                a->re += tr;        a->im += ti;
            }
        }
    }
}

/* Analyse n words starting at ring[start] (circular). Uses the most recent
 * whole segments; returns false below SNR_MIN_SAMPLES. */
static inline bool snr_analyze(const volatile uint32_t *ring, size_t ring_words,
                               size_t start, size_t n, snr_result_t *r)
{
    if (n > SNR_MAX_SAMPLES) { start = (start + n - SNR_MAX_SAMPLES) % ring_words; n = SNR_MAX_SAMPLES; }
    if (n < SNR_MIN_SAMPLES) return false;
    snr_tables_init();
    *r = (snr_result_t){0};
    r->samples = n;
    r->gain_min = 0x7fu;
    int64_t sum_i = 0, sum_q = 0;
    for (size_t k = 0; k < n; ++k) {
        const uint32_t w = ring[(start + k) % ring_words];
        const int i = snr_word_i(w), q = snr_word_q(w);
        const unsigned g = snr_word_gain(w);
        sum_i += i; sum_q += q;
        r->clips += (i >= SNR_CLIP_HI || i <= SNR_CLIP_LO || q >= SNR_CLIP_HI || q <= SNR_CLIP_LO);
        if (g < r->gain_min) r->gain_min = g;
        if (g > r->gain_max) r->gain_max = g;
    }
    r->dc_i = (float)sum_i / (float)n;
    r->dc_q = (float)sum_q / (float)n;
    /* Mean in Q4 so the subtraction leaves < 1/16 LSB of DC. */
    const int32_t mi16 = (int32_t)((sum_i * 16) / (int64_t)n);
    const int32_t mq16 = (int32_t)((sum_q * 16) / (int64_t)n);

    uint64_t acc[SNR_FFT] = {0};
    snr_cplx_t x[SNR_FFT];
    for (size_t seg = 0; seg + SNR_FFT <= n; seg += SNR_HOP) {
        for (unsigned m = 0; m < SNR_FFT; ++m) {
            const uint32_t w = ring[(start + seg + m) % ring_words];
            /* (16x - mean16) * hann_q14 >> 4 = x * hann_q14, |.| < 2^24. */
            x[m].re = ((snr_word_i(w) * 16 - mi16) * s_snr_hann[m]) >> 4;
            x[m].im = ((snr_word_q(w) * 16 - mq16) * s_snr_hann[m]) >> 4;
        }
        snr_fft64(x);
        for (unsigned k = 0; k < SNR_FFT; ++k)
            acc[k] += ((uint64_t)((int64_t)x[k].re * x[k].re) +
                       (uint64_t)((int64_t)x[k].im * x[k].im)) >> 8;
        ++r->segments;
    }
    /* Per-bin power: |X|^2 / (N * sum(w^2)) with the window in its Q14
     * integer form on both sides, and the >> 8 above undone. Sum over bins =
     * window-weighted mean |x|^2. */
    double w2 = 0.0;
    for (unsigned m = 0; m < SNR_FFT; ++m) w2 += (double)s_snr_hann[m] * s_snr_hann[m];
    const double scale = 256.0 / ((double)SNR_FFT * w2 * (double)r->segments);
    float peak = -1.0f;
    for (unsigned k = 0; k < SNR_FFT; ++k) {
        const float p = (float)((double)acc[k] * scale);
        const int f = snr_rf_bin(k), af = f < 0 ? -f : f;
        r->psd[k] = p;
        r->total += p;
        if (af <= SNR_IN_BINS) {
            r->in_band += p;
            if (f < 0) r->lower += p;
            if (f > 0) r->upper += p;
            if (p > peak) { peak = p; r->peak_bin = f; }
        } else if (af >= SNR_EDGE_LO_BIN && af <= SNR_EDGE_HI_BIN) {
            r->edge += p;
        }
    }
    return true;
}

/* Signal-to-noise estimate from in-band power against the no-carrier floor
 * measured at the same gain index: (P - N) / N in dB, clamped at -30 dB. */
static inline float snr_estimate_db(float in_band, float floor_in_band)
{
    if (floor_in_band <= 0.0f) return NAN;
    float excess = in_band / floor_in_band - 1.0f;
    return 10.0f * log10f(excess > 1e-3f ? excess : 1e-3f);
}

/* Median of the last three, then EMA alpha 0.3. One-capture spikes (5 GHz
 * Wi-Fi bursts, a hop) vanish without slowing a real fade by more than one
 * reading. */
typedef struct { float hist[3]; unsigned count; float ema; } snr_filter_t;

static inline void snr_filter_reset(snr_filter_t *f) { f->count = 0; }

static inline float snr_filter_push(snr_filter_t *f, float v)
{
    f->hist[f->count % 3u] = v;
    ++f->count;
    float m = v;
    if (f->count >= 3u) {
        float a = f->hist[0], b = f->hist[1], c = f->hist[2];
        m = a > b ? (b > c ? b : (a > c ? c : a)) : (a > c ? a : (b > c ? c : b));
    }
    f->ema = f->count == 1u ? m : f->ema + 0.3f * (m - f->ema);
    return f->ema;
}
