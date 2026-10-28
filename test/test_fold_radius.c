/* C5VRX by Twotoz and the C5VRX contributors: agc_level_fold_radius() on the
 * exact hardware fold model (adc_fold.h). A folded FM carrier must come back
 * with its own radius; a noise cloud, a DC-pushed cloud or a row too noisy to
 * tell must come back 0, so LEVEL keeps its old reading for those.
 *
 *   cc -O2 -std=c11 -I../main test_fold_radius.c ../main/agc.c -lm -o t && ./t
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "agc.h"
#include "adc_fold.h"

static uint32_t s_seed = 7u;
static double urand(void) { s_seed = s_seed * 1664525u + 1013904223u; return ((s_seed >> 8) + 0.5) / 16777216.0; }
static double gauss(void) { return sqrt(-2.0 * log(urand())) * cos(6.283185307179586 * urand()); }

static uint8_t pack(double i, double q)
{
    return (uint8_t)(((unsigned)adc_fold4(i) & 15u) << 4 | ((unsigned)adc_fold4(q) & 15u));
}

/* An FM carrier at 40 MS/s: -1 MHz offset, a video deviation of a few MHz. */
static void carrier(uint8_t *b, unsigned n, double r, double sigma, double di, double dq)
{
    double phi = 6.283185307179586 * urand();
    const double f0 = -1.0e6 + 6.0e6 * (urand() - 0.5), fm = 0.2 + 1.8 * urand();
    for (unsigned k = 0; k < n; ++k) {
        phi += 6.283185307179586 * (f0 + 1.5e6 * sin(6.283185307179586 * fm * k / n)) / 40e6;
        b[k] = pack(r * cos(phi) + di + sigma * gauss(), r * sin(phi) + dq + sigma * gauss());
    }
}

static void cloud(uint8_t *b, unsigned n, double sigma, double di, double dq)
{
    for (unsigned k = 0; k < n; ++k) b[k] = pack(di + sigma * gauss(), dq + sigma * gauss());
}

static agc_level_t s_a;
static void row_noise(double sigma)   /* per-axis receiver noise of the row in use */
{
    s_a.noise_x16[40] = (uint16_t)lround(2.0 * sigma * sigma * 16.0);
}

int main(void)
{
    memset(&s_a, 0, sizeof(s_a));
    s_a.homes = 1;
    s_a.max_index = 83;
    unsigned fail = 0;
    uint8_t b[256];
    /* Folded carriers: within half a cell, nearly always. */
    const double radii[] = {9, 10, 12, 14, 17, 20, 24};
    const double sigmas[] = {0.3, 0.6};
    const unsigned sizes[] = {64, 256};
    for (unsigned si = 0; si < 2; ++si)
        for (unsigned ni = 0; ni < 2; ++ni) {
            row_noise(sigmas[si]);
            unsigned good = 0, none = 0, total = 0;
            for (unsigned ri = 0; ri < sizeof(radii) / sizeof(radii[0]); ++ri)
                for (unsigned t = 0; t < 100; ++t, ++total) {
                    carrier(b, sizes[ni], radii[ri], sigmas[si], 0.3 * gauss(), 0.3 * gauss());
                    const uint16_t r = agc_level_fold_radius(&s_a, b, sizes[ni], 40);
                    if (!r) ++none;
                    else if (fabs(r / 16.0 - radii[ri]) <= 0.5) ++good;
                }
            const double pct = 100.0 * good / total;
            printf("carriers sigma %.1f n=%3u: %5.1f %% within 0.5 cell, %4.1f %% no estimate\n",
                   sigmas[si], sizes[ni], pct, 100.0 * none / total);
            if (pct < (sizes[ni] == 64u ? 90.0 : 97.0)) { printf("FAIL carrier accuracy\n"); ++fail; }
        }
    /* An unfolded carrier is no evidence of being over the window. */
    row_noise(0.3);
    unsigned small = 0;
    for (unsigned t = 0; t < 200; ++t) { carrier(b, 256, 5.0, 0.3, 0, 0); small += agc_level_fold_radius(&s_a, b, 256, 40) != 0; }
    printf("radius 5 (inside the window): %u of 200 estimated\n", small);
    if (small) { printf("FAIL estimate inside the window\n"); ++fail; }
    /* Noise clouds and a DC-pushed cloud on a quiet row: no circle. */
    const struct { double sigma, di, dq; const char *name; } clouds[] = {
        {4.0, 0, 0, "noise 4 cells"}, {8.0, 0, 0, "noise 8 cells"}, {2.1, 4.1, -3.7, "noise 2.1 + DC 4.1/-3.7"}};
    for (unsigned c = 0; c < 3; ++c) {
        unsigned took = 0;
        for (unsigned t = 0; t < 200; ++t) { cloud(b, 256, clouds[c].sigma, clouds[c].di, clouds[c].dq); took += agc_level_fold_radius(&s_a, b, 256, 40) != 0; }
        printf("%-26s: %3u of 200 taken as a carrier\n", clouds[c].name, took);
        if (took > 4) { printf("FAIL noise taken as a carrier\n"); ++fail; }
    }
    /* A row too noisy to tell: no estimate at all. */
    row_noise(1.5);
    carrier(b, 256, 12.0, 1.5, 0, 0);
    if (agc_level_fold_radius(&s_a, b, 256, 40)) { printf("FAIL estimate on a noisy row\n"); ++fail; }
    printf("%s: fold radius checks\n", fail ? "FAIL" : "PASS");
    return fail != 0;
}
