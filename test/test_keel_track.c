/* C5VRX-5 by Twotoz and the C5VRX contributors: KEEL-1 search harness.
 * The real dg_tick() (main/gain.c) on a moving carrier, with what the
 * plain tracking model (test_gain_track.c) leaves out: every gain row has its
 * own DC, and the live servo removes it only while the gain stays on that row.
 *
 * Receiver model (board 2026-10-19):
 * - noise 1.95 cells per axis at G83;
 * - Gaussian noise peaks (P95 ~ 4.3 x P50);
 * - 4-bit window +-7.5 cells;
 * - each gain write costs WRITE_COST_US of picture (blackbox: ~2.5 ms).
 *
 * DC model:
 * - each row starts with a DC of DC0_CELLS (random direction, 0.5-1.5 x);
 *   C5VRX-4 measured ~1.1 cell on uncorrected rows;
 * - the servo shrinks it by e every DC_TAU_US of settled dwell on that row,
 *   down to DC_FLOOR_CELLS;
 * - the DC adds to the noise (a shifted circle demodulates with a phase
 *   error) and to folding (the carrier plus noise plus DC must stay inside
 *   the window).
 * The DC numbers are not measured per row: the search reports how results
 * move with them.
 *
 * Scenarios: close / mid / edge levels, K 0/3/10, 5-80 Hz Doppler, sustained
 * steps, VTX off and on. Score: the share of time with a usable picture,
 * picture SNR >= 10 dB close in and mid, >= 3 dB at the edge.
 *
 *   cc -O2 -std=c11 -I../main test_keel_track.c ../main/gain.c ../main/arc_phy.c -lm -o t && ./t
 *   (-DKEEL_LATTICE_Q8=1536 for 6 dB home rows; -DDC_TAU_US=..., -DDC0_CELLS=...)
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gain.h"
#include "phase8_gain_lut.h"
#include "adc_fold.h"

unsigned char phy_param[2048];

#define TICK_US 200u
#ifndef WRITE_COST_US
#define WRITE_COST_US 2000u
#endif
#ifndef FOLD_COST_US
#define FOLD_COST_US 4000u   /* a folded moment costs ~a few lines of picture and sync */
#endif
#ifndef DC_TAU_US
#define DC_TAU_US 100000.0
#endif
#ifndef DC0_CELLS
#define DC0_CELLS 1.0
#endif
#ifndef DC_FLOOR_CELLS
#define DC_FLOOR_CELLS 0.1
#endif

static direct_gain_t g;
static arc_gain_table_t table;
static double s_db[DG_STATES], s_dc0[DG_STATES], s_dca[DG_STATES], s_dwell[DG_STATES];
static uint32_t s_seed = 7;
static double urand(void) { s_seed = s_seed * 1664525u + 1013904223u; return (s_seed >> 8) / 16777216.0; }

static void build_model(void)
{
    arc_gain_table_from_bytes(&table, NULL, ARC_5G_GAIN_MAX);
    for (unsigned k = 0; k <= table.max_index; ++k) {
        arc_gain_tuple_t t;
        arc_gain_tuple_decode(&table, (uint8_t)k, &t);
        s_db[k] = 6.0 * t.rf_stage + 6.0 * log2(t.bb_code + 1.0) + 0.83 * (5 - (int)t.fine_code);
    }
}
static void dc_reset(uint32_t seed)
{
    s_seed = seed;
    for (unsigned k = 0; k < DG_STATES; ++k) { s_dc0[k] = DC0_CELLS * (0.5 + urand()); s_dca[k] = 6.2831853 * urand(); s_dwell[k] = 0; }
}
static double dc_cells(unsigned k)
{
    const double d = s_dc0[k] * exp(-s_dwell[k] / DC_TAU_US);
    return d > DC_FLOOR_CELLS ? d : DC_FLOOR_CELLS;
}
static double radius(double level_db, unsigned k) { return level_db <= -200 ? 0 : pow(10.0, (level_db + s_db[k]) / 20.0); }
static double noise_radius(unsigned k) { return 1.95 * sqrt(2.0) * pow(10.0, (s_db[k] - s_db[table.max_index]) / 20.0); }

/* Per-probe statistics from real samples (default): 256 samples of an FM
 * carrier (porch -1 MHz, +-1.5 MHz video at the line rate) plus complex
 * Gaussian receiver noise plus the row's DC, through the ultrafine 4-bit
 * lane (it wraps, it does not clip), measured by the firmware's own
 * dg_measure_gain(). A weak carrier's P95 and folding flicker from probe to
 * probe as on the board; the analytic model (-DANALYTIC_OBS) is smooth and
 * missed the 255 writes a second of blackbox session 28. */
static uint32_t s_nseed = 12345u;
static double s_phi;
static double gauss(void)
{
    double u1, u2;
    do { s_nseed = s_nseed * 1664525u + 1013904223u; u1 = (s_nseed >> 8) / 16777216.0; } while (u1 <= 0.0);
    s_nseed = s_nseed * 1664525u + 1013904223u; u2 = (s_nseed >> 8) / 16777216.0;
    return sqrt(-2.0 * log(u1)) * cos(6.2831853 * u2);
}
/* A gain write is a PHY transient: WRITE_TRANSIENT_US of garbage across
 * the whole 4-bit window after it (the board's overload snapshots were clean
 * a few ms later; the overloads came from the transient). */
static uint64_t s_last_write_us;
static dg_observation_t observe_samples(double level_db, unsigned k, uint64_t t)
{
    uint8_t b[256];
#ifdef WRITE_TRANSIENT_US
    if (s_last_write_us && t - s_last_write_us < WRITE_TRANSIENT_US) {
        for (unsigned n = 0; n < 256u; ++n) {
            s_nseed = s_nseed * 1664525u + 1013904223u;
            b[n] = (uint8_t)(s_nseed >> 24);
        }
        return dg_measure_gain(b, sizeof(b), c5vrx_phase8_gain_lut, t);
    }
#endif
    const double r = radius(level_db, k), sigma = noise_radius(k) / sqrt(2.0), d = dc_cells(k);
    const double di = d * cos(s_dca[k]), dq = d * sin(s_dca[k]);
    for (unsigned n = 0; n < 256u; ++n) {
        const double ts = (double)t * 1e-6 + n / 40e6;
        const double f = -1.0e6 + 1.5e6 * sin(6.2831853 * 15734.0 * ts);
        s_phi += 6.2831853 * f / 40e6;
        const int i = adc_fold4(r * cos(s_phi) + sigma * gauss() + di);
        const int q = adc_fold4(r * sin(s_phi) + sigma * gauss() + dq);
        b[n] = (uint8_t)(((unsigned)i & 15u) << 4 | ((unsigned)q & 15u));
    }
    return dg_measure_gain(b, sizeof(b), c5vrx_phase8_gain_lut, t);
}

static dg_observation_t observe(double level_db, unsigned k, uint64_t t)
{
#ifndef ANALYTIC_OBS
    return observe_samples(level_db, k, t);
#endif
    dg_observation_t o;
    memset(&o, 0, sizeof(o));
    o.observed_us = t;
    const double r = radius(level_db, k), rn = noise_radius(k), d = dc_cells(k);
    const double p50 = r * r + 0.69 * rn * rn, p90 = r * r + 2.3 * rn * rn, p95 = r * r + 3.0 * rn * rn;
    o.p50 = (uint8_t)(p50 > 113 ? 113 : p50);
    o.p90 = (uint8_t)(p90 > 113 ? 113 : p90);
    o.p95 = (uint8_t)(p95 > 113 ? 113 : p95);
    const bool folded = r + rn + d > 7.5;
    const bool dc_touch = !folded && 2.0 * rn + d > 7.5;   /* the cloud pushed against one rail */
    o.clip_pm = (uint16_t)(folded ? fmin(1000.0, 150.0 * (r + rn + d) / 7.5) : dc_touch ? 120 : (r + 2 * rn + d > 7.5 ? 10 : 0));
    o.rails = folded ? 15u : dc_touch ? 5u : 0u;
    const double spread = r > 0.5 ? 0 : rn;
    o.origin_pm = (uint16_t)(r > 2.0 ? 20 : 1000.0 * exp(-(spread * spread + r * r) / 1.5));
    o.coherence = (uint8_t)(folded ? 30 : r > 2.5 ? 95 : r > 1.2 ? 70 : rn > 1.0 ? 45 : 10);
    o.span = 10;
    o.repeat_pm = 50;
    return o;
}

/* The detector's own loss on a small signal, beyond the quantisation noise:
 * the bit-exact U85Q chain (research/sim.py, PAL, deviation 1.0) at the same
 * C/N with the total RMS at the quantiser 1.5 / 2.2 / 3.0 / 4.2 cells loses
 * 2.9 / 1.4 / 0.6 / 0 dB of luma SINAD (range audit, PR #230; the picture
 * threshold at 2 dB C/N falls from 0.96 to 0.67 at 1.5 cells). */
static double small_signal_loss_db(double rms)
{
    static const double at[] = {1.5, 2.2, 3.0, 4.2}, loss[] = {2.9, 1.4, 0.6, 0.0};
    if (rms <= at[0]) return loss[0] + (at[0] - rms) * 2.0;
    for (int k = 1; k < 4; ++k)
        if (rms <= at[k]) return loss[k - 1] + (loss[k] - loss[k - 1]) * (rms - at[k - 1]) / (at[k] - at[k - 1]);
    return 0.0;
}

static double snr_db(double level_db, unsigned k)
{
    const double r = radius(level_db, k), rn = noise_radius(k), d = dc_cells(k);
    if (r + rn + d > 7.5) return -99.0;
    return 10.0 * log10(r * r / (rn * rn + 1.0 / 6.0 + d * d) + 1e-9) - small_signal_loss_db(sqrt(r * r + rn * rn));
}

#define PATHS 12
static double s_phase[PATHS], s_cos[PATHS];
static void fading_init(uint32_t seed)
{
    uint32_t keep = s_seed;
    s_seed = seed;
    for (int p = 0; p < PATHS; ++p) { s_phase[p] = 6.2831853 * urand(); s_cos[p] = cos(6.2831853 * urand()); }
    s_seed = keep;
}
static double fading_db(uint64_t t_us, double doppler_hz, double k_factor)
{
    double re = 0, im = 0;
    for (int p = 0; p < PATHS; ++p) {
        const double a = 6.2831853 * doppler_hz * s_cos[p] * (double)t_us * 1e-6 + s_phase[p];
        re += cos(a); im += sin(a);
    }
    re /= sqrt((double)PATHS); im /= sqrt((double)PATHS);
    const double los = sqrt(k_factor);
    const double h2 = ((los + re) * (los + re) + im * im) / (k_factor + 1.0);
    return 10.0 * log10(h2 > 1e-6 ? h2 : 1e-6);
}

typedef struct {
    const char *name;
    double level_db;          /* mean level, as dB over the G83 noise-free unit radius */
    double doppler, k;
    double step_db;           /* a sustained step half way (0 none, -300 VTX off) */
    double threshold_db;      /* picture SNR counted as usable */
    double weight;
} scenario_t;

typedef struct { double usable_pct, writes_s, mean_dc, mean_gain; } result_t;

static result_t run(const scenario_t *sc, uint32_t seed)
{
    dc_reset(seed);
    fading_init(seed * 31u + 7u);
    dg_reset(&g, &table, table.max_index);
    g.learn_enabled = true;
    uint64_t t = 1000, last_write = 0, last_fold = 0;
    unsigned applied = g.current_gain;
    const unsigned settle_ticks = 5000u, ticks = 25000u;   /* 1 s settle, 5 s measured */
    unsigned ok = 0;
    uint32_t w0 = 0;
    double dc_sum = 0, gain_sum = 0;
    for (unsigned n = 0; n < settle_ticks + ticks; ++n, t += TICK_US) {
        if (n == settle_ticks) w0 = g.writes;
        double lv = sc->level_db + fading_db(t, sc->doppler, sc->k);
        if (sc->step_db != 0 && n >= settle_ticks + ticks / 2u) lv = sc->step_db <= -300 ? -300 : lv + sc->step_db;
        dg_observation_t o = observe(lv, applied, t);
#ifdef AFC_RESET_US
        /* An AFC step every AFC_RESET_US: the firmware re-binds the core
         * (same receiver, offset changed). */
        if (n && n % (AFC_RESET_US / TICK_US) == 0) dg_reset_context(&g, &table, (uint8_t)applied, NULL);
#endif
        const uint8_t target = dg_tick(&g, &o);
        if (target != applied) { applied = target; dg_sync_applied(&g, target, t); last_write = t; s_last_write_us = t; }
        else if (!last_write || t - last_write >= 400u) s_dwell[applied] += TICK_US;   /* the servo learns settled rows */
        /* A fold tears the picture for longer than its own samples: lines and
         * sync go (board, session 34: "why does the video almost drop out"). */
        if (snr_db(lv, applied) < -90.0) last_fold = t;
        if (n >= settle_ticks) {
            const bool fresh = (!last_write || t - last_write >= WRITE_COST_US) &&
                               (!last_fold || t - last_fold >= FOLD_COST_US);
            ok += fresh && snr_db(lv, applied) >= sc->threshold_db;
            dc_sum += dc_cells(applied);
            gain_sum += applied;
        }
    }
    return (result_t){100.0 * ok / ticks, (g.writes - w0) / 5.0, dc_sum / ticks, gain_sum / ticks};
}

int main(void)
{
    build_model();
    const double top = s_db[table.max_index];
    const double close = 20.0 * log10(4.5) - s_db[28], mid = 20.0 * log10(4.5) - s_db[60];
    const double edge0 = 20.0 * log10(2.8) - top, edge3 = 20.0 * log10(3.9) - top, edge6 = 20.0 * log10(5.5) - top;
    const double edge10 = 20.0 * log10(8.7) - top, edge18 = 20.0 * log10(21.9) - top;
    const scenario_t sc[] = {
        {"close, walking (K 10, 5 Hz)", close, 5, 10, 0, 10, 1},
        {"close, moving (K 3, 30 Hz)", close, 30, 3, 0, 10, 1},
        {"close, Rayleigh 30 Hz", close, 30, 0, 0, 10, 1},
        {"close, fast (K 3, 80 Hz)", close, 80, 3, 0, 10, 1},
        {"mid, moving (K 3, 30 Hz)", mid, 30, 3, 0, 10, 1},
        {"mid, Rayleigh 30 Hz", mid, 30, 0, 0, 10, 1},
        {"mid, -12 dB behind an obstacle", mid, 30, 3, -12, 10, 1},
        {"edge 6 dB C/N (K 3, 30 Hz)", edge6, 30, 3, 0, 3, 2},
        {"edge 3 dB C/N (K 3, 30 Hz)", edge3, 30, 3, 0, 3, 2},
        {"edge 3 dB C/N, Rayleigh", edge3, 30, 0, 0, 3, 2},
        {"edge 0 dB C/N (K 3, 30 Hz)", edge0, 30, 3, 0, 3, 2},
        {"edge 3 dB C/N, walking (K 10, 5 Hz)", edge3, 5, 10, 0, 3, 2},
        /* Blackbox session 28, walking out: -76..-88 dBm, C/N 8-12 dB, where
         * the carrier peaks fold at G83 and KEEL-1 wrote 255 times a second. */
        {"walk-out 10 dB C/N (K 3, 10 Hz)", edge10, 10, 3, 0, 3, 2},
        {"walk-out 10 dB C/N, Rayleigh 10 Hz", edge10, 10, 0, 0, 3, 2},
        /* Blackbox session 29 (sentinel fixed): -80..-70 dBm, C/N ~15-20 dB
         * at G83, still 255 writes down and up in a second. */
        {"walk-out 18 dB C/N (K 3, 10 Hz)", edge18, 10, 3, 0, 10, 2},
        {"walk-out 18 dB C/N, Rayleigh 10 Hz", edge18, 10, 0, 0, 10, 2},
        /* Blackbox session 34 (walking): attacks were real folds a median
         * 41 ms after a release; walking fades peak every 100-500 ms. */
        {"walking 18 dB C/N (K 3, 2 Hz)", edge18, 2, 3, 0, 10, 2},
        {"walking 18 dB C/N, Rayleigh 5 Hz", edge18, 5, 0, 0, 10, 2},
        {"walking 6 dB C/N (K 3, 2 Hz)", edge6, 2, 3, 0, 3, 2},
        {"close, walking Rayleigh 2 Hz", close, 2, 0, 0, 10, 1},
        /* A drone at 100-200 km/h: Doppler v / lambda = 540-1070 Hz at
         * 5.8 GHz, a fade every 1-2 ms (operator, 2026-10-20). No gain loop
         * can follow that; KEEL must ride the envelope, not hunt it. */
        {"fast drone 100 km/h, mid (K 3, 540 Hz)", mid, 540, 3, 0, 10, 1},
        {"fast drone 100 km/h, edge 6 dB (K 3)", edge6, 540, 3, 0, 3, 2},
        {"fast drone 200 km/h, edge 3 dB (K 3)", edge3, 1070, 3, 0, 3, 2},
        {"fast drone 100 km/h, walk-out 18 dB Rayleigh", edge18, 540, 0, 0, 10, 2},
    };
    const unsigned n = sizeof(sc) / sizeof(sc[0]);
    double score = 0, weights = 0, writes = 0, edge_writes = 0, edge3_usable = 0;
    for (unsigned k = 0; k < n; ++k) {
        result_t a = {0}, b;
        for (uint32_t seed = 1; seed <= 3; ++seed) {   /* three DC/fading draws */
            b = run(&sc[k], seed);
            a.usable_pct += b.usable_pct / 3; a.writes_s += b.writes_s / 3; a.mean_dc += b.mean_dc / 3;
            a.mean_gain += b.mean_gain / 3;
        }
        printf("     %-38s usable %5.1f%%  writes %5.1f/s  DC %.2f cells  G%.1f\n", sc[k].name, a.usable_pct, a.writes_s, a.mean_dc,
               a.mean_gain);
        printf("S,%u,%.2f,%.2f,%.3f\n", k, a.usable_pct, a.writes_s, a.mean_dc);
        score += sc[k].weight * a.usable_pct; weights += sc[k].weight; writes += a.writes_s / n;
        if (k >= 8u && k < 12u) edge_writes += a.writes_s / 4.0;   /* the 3 dB and 0 dB edge scenarios */
        if (k >= 12u) printf("     (walk-out writes %.1f/s)\n", a.writes_s);
        if (k == 8u) edge3_usable = a.usable_pct;
    }
    printf("SCORE %.2f (weighted usable %%; edge counts twice), writes %.1f/s\n", score / weights, writes);
#ifndef NO_CHECKS
    /* KEEL-1 against the sample-based model. Since PR #230 the model counts
     * the detector's small-signal loss (small_signal_loss_db), which lowers
     * every edge number, and the fast-drone scenarios (540-1070 Hz) count:
     * KEEL-1 as merged in #228 scores 60.4, edge 3 dB 29.3 %; target P95
     * 38 / release 34 / heat 1.5 dB 61.9 and 30.6 % (38-39.6 % edge without
     * the small-signal loss; V7 62.3, V6 58.8 on the older list). */
    unsigned fail = 0;
    if (score / weights < 61.5) { printf("FAIL score %.2f < 61.5 (#228: 60.4)\n", score / weights); ++fail; }
    if (edge_writes > 3.0) { printf("FAIL edge writes %.1f/s > 3\n", edge_writes); ++fail; }
    if (edge3_usable < 28.0) { printf("FAIL edge 3 dB usable %.1f < 28 %% (#228: 29.3)\n", edge3_usable); ++fail; }
    printf("%s: KEEL-1 checks\n", fail ? "FAIL" : "PASS");
    return fail != 0;
#else
    (void)edge_writes; (void)edge3_usable;
    return 0;
#endif
}
