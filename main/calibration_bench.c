/* C5VRX-5 by Twotoz and contributors: bench-only measurements of the
 * calibration task (console L lane comparison, self-noise test). */
#include "calibration_internal.h"

/* ---- Bench-only IQ lane comparison (console L) ---------------------------
 * Production keeps the ultrafine lane {9,6,5,4}. This compares it with fine
 * {9,7,6,5} and coarse {9,8,7,6} on the same RF input, gain, filter and
 * tuning, in ABBA order (U F C C F U) so a slow fade does not favour one
 * lane. Raw-IQ statistics only: P64's LUT is made for ultrafine, so
 * this is no demodulation, sensitivity or range result. Only on an
 * explicit request; the status screen owns TX for the ~3 s it takes and
 * the ultrafine lane is always routed back. */
#define LANE_WINDOWS 24u
volatile bool s_lane_requested;
void lane_compare_request(void) { s_lane_requested = true; }

/* Bench (console N, range audit PR #230): does the board's own video output
 * (6-bit DAC and PARLIO TX at 40 MHz, next to the antenna input) raise the
 * receiver's noise floor? Receiver noise at the table maximum with the
 * output running and stopped, alternated four times (drift averages out);
 * the VTX must be off. Video stops ~0.3 s per pair. */
volatile bool s_selfnoise_requested;
void self_noise_request(void) { s_selfnoise_requested = true; }
static bool selfnoise_measure(double *m2_out, double *kurt_out)
{
    uint8_t buf[PROBE_BYTES];
    double m2s = 0.0, m4s = 0.0;
    unsigned got = 0;
    for (unsigned tries = 0; tries < 96u && got < 48u; ++tries) {
        vTaskDelay(1);
        if (!copy_probe_window(buf, NULL)) continue;
        cna_sums_t p = {0};
        for (unsigned k = 0; k < sizeof(buf); ++k) cna_sums_add(&p, buf[k]);
        float mi, mq;
        cna_mean(&p, &mi, &mq);
        double m2, m4;
        cna_moments(&p, mi, mq, &m2, &m4);
        m2s += m2; m4s += m4;
        ++got;
    }
    if (got < 16u) return false;
    *m2_out = m2s / got;
    *kurt_out = (m4s / got) / (*m2_out * *m2_out);
    return true;
}
void self_noise_test(void)
{
    if (!quiet_owner() || !receiver_pause("SELFNOISE")) { printf("SELFNOISE refused=busy\n"); return; }
    const uint8_t restore = s_current_gain;
    receiver_set_gain(rf_gain_table()->max_index);
    vTaskDelay(pdMS_TO_TICKS(40));
    double on_db = 0.0, off_db = 0.0;
    unsigned pairs = 0;
    for (unsigned rep = 0; rep < 4u; ++rep) {
        double m_on, k_on, m_off, k_off;
        const bool a = selfnoise_measure(&m_on, &k_on);
        transport_stop_tx();
        vTaskDelay(pdMS_TO_TICKS(30));
        const bool b = selfnoise_measure(&m_off, &k_off);
        transport_resume_live();
        vTaskDelay(pdMS_TO_TICKS(60));
        if (!a || !b) { printf("SELFNOISE pair=%u probe_failed\n", rep); continue; }
        printf("SELFNOISE pair=%u output_on=%.3f kurt=%.2f output_off=%.3f kurt=%.2f diff_db=%+.2f\n", rep, m_on, k_on,
               m_off, k_off, 10.0 * log10(m_on / m_off));
        on_db += 10.0 * log10(m_on); off_db += 10.0 * log10(m_off);
        ++pairs;
    }
    receiver_set_gain(restore);
    receiver_resume();
    if (pairs)
        printf("SELFNOISE gain=%u pairs=%u mean_diff_db=%+.2f (receiver noise with the video output on minus off; "
               "> 0.3 dB = the board raises its own floor)\n", rf_gain_table()->max_index, pairs,
               (on_db - off_db) / pairs);
}

typedef struct { unsigned windows; long p_median, q_phase, origin_pm, clip_pm, env_x100; } lane_stats_t;

static void lane_measure(lane_stats_t *st)
{
    uint8_t sample[PROBE_BYTES];
    vTaskDelay(pdMS_TO_TICKS(20));      /* the ring refills within ~1 ms */
    for (unsigned tries = 0, got = 0; tries < LANE_WINDOWS * 3u && got < LANE_WINDOWS; ++tries) {
        vTaskDelay(1);
        if (!copy_probe_window(sample, NULL)) continue;
        metrics_t m = measure_window(sample, sizeof(sample));
        st->p_median += m.p_median;
        st->q_phase += m.q_phase;
        st->origin_pm += m.origin_pm;
        st->clip_pm += m.clip_pm;
        st->env_x100 += dsp_envelope_ratio_x100(sample, sizeof(sample));
        ++st->windows;
        ++got;
    }
}

void lane_compare(void)
{
    if (!quiet_owner() || !receiver_pause("LANE_CMP")) { printf("LANE_CMP refused=busy\n"); return; }
    s_scan_active = true;
    status_screen("BENCH LANE TEST", "VIDEO STOPS FOR ABOUT 3 S", "SAME GAIN, FILTER AND CHANNEL", -1);
    static const uint8_t order[] = {RF_LANE_ULTRAFINE, RF_LANE_FINE, RF_LANE_COARSE,
                                    RF_LANE_COARSE, RF_LANE_FINE, RF_LANE_ULTRAFINE};
    lane_stats_t st[RF_LANES];
    memset(st, 0, sizeof(st));
    bool routed = true;
    for (unsigned k = 0; k < sizeof(order) && routed; ++k) {
        routed = rf_set_lane(order[k]) == ESP_OK;
        if (routed) lane_measure(&st[order[k]]);
    }
    esp_err_t back = rf_set_lane(RF_LANE_ULTRAFINE);
    for (unsigned t = 0; back != ESP_OK && t < 3u; ++t) { vTaskDelay(pdMS_TO_TICKS(10)); back = rf_set_lane(RF_LANE_ULTRAFINE); }
    printf("LANE_CMP freq=%u gain=%u filter_code=%d skirt=%u routed=%u restored=%s (raw IQ only; P64 is made for "
           "ultrafine, not a range result)\n", rf_frequency_mhz(), s_current_gain, rf_filter().code, rf_filter().skirt,
           routed, back == ESP_OK && rf_lane() == RF_LANE_ULTRAFINE ? "ultrafine" : "FAILED");
    for (unsigned l = 0; l < RF_LANES; ++l) {
        const long n = st[l].windows ? (long)st[l].windows : 1;
        printf("LANE_CMP lane=%s windows=%u p_median=%ld q_phase=%ld origin_pm=%ld rail_pm=%ld env_x100=%ld\n",
               rf_lane_name(l), st[l].windows, st[l].p_median / n, st[l].q_phase / n, st[l].origin_pm / n,
               st[l].clip_pm / n, st[l].env_x100 / n);
    }
    printf("LANE_CMP note: origin_pm high = signal below the lane's step; rail_pm high = folding risk; compare "
           "q_phase at equal RF input over repeated runs before changing the production lane\n");
    status_screen_end();
    s_scan_active = false;
    receiver_resume();
}
