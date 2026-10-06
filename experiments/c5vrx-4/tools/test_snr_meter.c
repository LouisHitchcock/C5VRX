/* C5VRX by Twotoz and contributors: 10-bit band-power / SNR meter regressions. */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "snr_meter.h"

static uint32_t s_ring[SNR_RING_WORDS];

static uint32_t word(int i, int q, unsigned gain)
{
    if (i > 511) i = 511;
    if (i < -512) i = -512;
    if (q > 511) q = 511;
    if (q < -512) q = -512;
    return ((uint32_t)q & 0x3ffu) | (((uint32_t)i & 0x3ffu) << 10) | ((gain & 0x7fu) << 20);
}

/* Deterministic Gaussian noise (Box-Muller over a small LCG). */
static uint32_t s_seed = 12345u;
static double uniform(void)
{
    s_seed = s_seed * 1664525u + 1013904223u;
    return ((s_seed >> 8) + 0.5) / 16777216.0;
}
static double gauss(void)
{
    return sqrt(-2.0 * log(uniform())) * cos(2.0 * 3.14159265358979 * uniform());
}

static void fill(size_t start, size_t n, double amp, double f_hz, double sigma, double dc, unsigned gain)
{
    for (size_t k = 0; k < n; ++k) {
        double ph = 2.0 * 3.14159265358979 * f_hz * (double)k / SNR_RATE_HZ;
        double i = amp * cos(ph) + sigma * gauss() + dc;
        double q = amp * sin(ph) + sigma * gauss() - dc;
        s_ring[(start + k) % SNR_RING_WORDS] = word((int)lround(i), (int)lround(q), gain);
    }
}

static void clear(void)
{
    for (size_t k = 0; k < SNR_RING_WORDS; ++k) s_ring[k] = SNR_SENTINEL;
}

int main(void)
{
    /* Word decode: sign extension of both 10-bit fields and the gain field. */
    uint32_t w = word(-512, 511, 89);
    assert(snr_word_i(w) == -512 && snr_word_q(w) == 511 && snr_word_gain(w) == 89);
    w = word(-1, -2, 0);
    assert(snr_word_i(w) == -1 && snr_word_q(w) == -2 && snr_word_gain(w) == 0);
    assert(snr_word_gain(SNR_SENTINEL) > 89);

    /* Fresh run: found across the ring wrap, and untouched ring is empty. */
    clear();
    size_t start;
    assert(snr_fresh_run(s_ring, SNR_RING_WORDS, SNR_SENTINEL, &start) == 0);
    fill(SNR_RING_WORDS - 1000, 3000, 0, 0, 2.0, 0, 60);
    size_t n = snr_fresh_run(s_ring, SNR_RING_WORDS, SNR_SENTINEL, &start);
    assert(n == 3000 && start == SNR_RING_WORDS - 1000);

    /* A complex tone at +5 MHz (bin +4), amplitude 100: in-band power is the
     * tone's A^2 = 40 dB, all of it in the upper half, peak at bin 4. */
    snr_result_t r;
    clear();
    fill(100, 3000, 100.0, 5e6, 0.0, 0.0, 60);
    n = snr_fresh_run(s_ring, SNR_RING_WORDS, SNR_SENTINEL, &start);
    assert(snr_analyze(s_ring, SNR_RING_WORDS, start, n, &r));
    assert(r.samples == SNR_MAX_SAMPLES && r.segments == 63);
    assert(fabsf(snr_db(r.in_band) - 40.0f) < 0.3f);
    assert(r.peak_bin == 4 * SNR_SPECTRUM_SIGN);
    assert(snr_db(SNR_SPECTRUM_SIGN > 0 ? r.upper : r.lower) - snr_db(SNR_SPECTRUM_SIGN > 0 ? r.lower : r.upper) > 40.0f);
    assert(r.clips == 0 && r.gain_min == 60 && r.gain_max == 60);
    assert(fabsf(r.total - r.in_band) / r.total < 0.01f);

    /* A tone at -15 MHz lands in the edge band only. */
    clear();
    fill(0, 2048, 100.0, -15e6, 0.0, 0.0, 60);
    assert(snr_analyze(s_ring, SNR_RING_WORDS, 0, 2048, &r));
    assert(snr_db(r.edge) > 39.5f && snr_db(r.in_band) < 10.0f);

    /* DC is reported and removed: a 40-LSB offset alone reads as no power. */
    clear();
    fill(0, 2048, 0.0, 0, 0.0, 40.0, 60);
    assert(snr_analyze(s_ring, SNR_RING_WORDS, 0, 2048, &r));
    assert(fabsf(r.dc_i - 40.0f) < 0.01f && fabsf(r.dc_q + 40.0f) < 0.01f);
    assert(r.total < 1e-3f);

    /* White noise, sigma 3 LSB per axis: total 2*sigma^2 = 12.6 dB, the
     * in-band share 17/64 of it, the edge share 14/64. */
    clear();
    fill(0, 2048, 0.0, 0, 3.0, 0.0, 81);
    assert(snr_analyze(s_ring, SNR_RING_WORDS, 0, 2048, &r));
    float total_db = snr_db(r.total);
    assert(fabsf(total_db - 10.0f * log10f(18.0f)) < 0.4f);
    assert(fabsf(snr_db(r.in_band) - snr_db(r.total * 17.0f / 64.0f)) < 0.6f);
    assert(fabsf(snr_db(r.edge) - snr_db(r.total * 14.0f / 64.0f)) < 0.6f);
    float floor_in = r.in_band;

    /* Tone plus that noise: the SNR estimate recovers tone power over the
     * in-band noise. A = 6 -> 36 LSB^2 vs 18 * 17/64 = 4.8 LSB^2 = 8.8 dB. */
    clear();
    fill(0, 2048, 6.0, 3e6, 3.0, 0.0, 81);
    assert(snr_analyze(s_ring, SNR_RING_WORDS, 0, 2048, &r));
    float snr = snr_estimate_db(r.in_band, floor_in);
    assert(fabsf(snr - 10.0f * log10f(36.0f / (18.0f * 17.0f / 64.0f))) < 0.8f);
    assert(isnan(snr_estimate_db(r.in_band, 0.0f)));
    assert(snr_estimate_db(floor_in * 0.5f, floor_in) == -30.0f);

    /* Clipping and mixed gain are counted. */
    clear();
    fill(0, 1024, 600.0, 1e6, 0.0, 0.0, 40);
    fill(1024, 1024, 10.0, 1e6, 0.0, 0.0, 42);
    assert(snr_analyze(s_ring, SNR_RING_WORDS, 0, 2048, &r));
    assert(r.clips > 100 && r.gain_min == 40 && r.gain_max == 42);

    /* Too short a window is refused. */
    assert(!snr_analyze(s_ring, SNR_RING_WORDS, 0, SNR_MIN_SAMPLES - 1, &r));

    /* Filter: a single +27 dB spike is removed, a real step is followed. */
    snr_filter_t f;
    snr_filter_reset(&f);
    for (int k = 0; k < 10; ++k) snr_filter_push(&f, 10.0f);
    assert(fabsf(snr_filter_push(&f, 37.0f) - 10.0f) < 1e-4f);
    assert(fabsf(snr_filter_push(&f, 10.0f) - 10.0f) < 1e-4f);
    float v = 0.0f;
    for (int k = 0; k < 12; ++k) v = snr_filter_push(&f, 20.0f);
    assert(v > 19.5f);

    puts("PASS: snr_meter (decode, fresh run, band power, SNR, filter)");
    return 0;
}
