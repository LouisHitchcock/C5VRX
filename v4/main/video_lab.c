/* C5VRX-4: lab responsibilities. */
#include "video_internal.h"

static void lab_observe_11p(const char *stage);
static void lab_dco_observe(const char *stage);
static void lab_filter_observe(const char *stage, int offset);
static void agc_mask_status_print(void);
static void lab_observe_range(const char *stage);
static bool range_lab_pause(const char *tag, analog_agc_mode_t *saved);
static void lab_observe_rf(const char *tag, const char *stage, int arg, unsigned hold_ms);
static void lab_observe_dfilt(const char *stage, int arg);
static void lab_observe_native_hold(const char *stage, unsigned cycle);
#define DFILT_WINDOWS 48u

#define SPHASE_SETTLE_TRIES 12u

#define SPHASE_POSITIONS 9u

#define LAB_BW_SETTLE_MS   800u      /* allow analog filter change before measurement */

typedef struct {
    int p_median;
    int p95;
    int origin_permille;
} centered_q4_metrics_t;

static centered_q4_metrics_t measure_centered_q4(const uint8_t *sample, size_t bytes);

/* Sampling-phase evidence: mid-transition reads per observed sample. */
static volatile uint32_t s_predemod_glitches, s_predemod_samples;


/* While masking: share of samples with the hold flag set (data bit 0),
 * from completed observer windows; ~6-13 % expected from the measured
 * acquisition share. Much more means the picture is mostly held. */
static unsigned s_agc_flag_share_pm;

static unsigned s_native_hold_cycles;

/* PRE-Q4 characterization may intentionally explore the complete vendor-
 * generated table above legacy G62. This bypasses profile clamps but never
 * writes a hand-built PBUS tuple: phy_force_rx_gain() still owns the state. */
void lab_apply_vendor_gain(uint8_t gain)
{
    const arc_gain_table_t *table = rf_get_arc_gain_table();
    if (gain < 2u) gain = 2u;
    if (gain > table->max_index) gain = table->max_index;
    s_current_gain = gain;
    s_shadow_gain = gain;
    s_last_gain_write_us = esp_timer_get_time();
    s_last_phy_write_us = s_last_gain_write_us;
    s_last_phy_write_kind = PHY_WRITE_GAIN;
    rf_set_rx_gain(true, gain);
    ++s_gain_transition_count;
}

void lab_enter_quiet_baseline(void)
{
    if (s_menu_active) {
        printf("C5VRX_LAB_BASELINE_REFUSED reason=menu_active\n");
        return;
    }

    s_agc_mode = ANALOG_AGC_MANUAL;
    s_rf_bw_mode = RF_BW_MODE_BW40;
    if (!s_current_bw40) apply_rf_bandwidth(true);
    s_afc_mode = AFC_MODE_OFF;
    if (rf_get_frequency_offset_khz() != 0) apply_frequency_offset_khz_tracked(0);
    s_lab_quiet = true;

    /* Baseline starts only after control-path setup transients are outside the
     * run. Then all counters/timestamps are cleared in one explicit action. */
    vTaskDelay(pdMS_TO_TICKS(600));
    lab_reset_correlation();

    printf("C5VRX_LAB_BASELINE_READY gain=%u bw=40 afc=off quiet=1 "
           "instruction=do_not_touch_console_until_event\n", s_current_gain);
}

/* Deterministic FFT A/B. Espressif exposes FFT gain separately from AGC gain;
 * this probe answers the only question that matters to C5VRX: does forcing it
 * change the raw MODEM_DIAG Q4/I4 statistics? Production never forces FFT. */

/* Safe first filter experiment: only exercise the already-used
 * phy_wifi_fbw_sel() BW40/BW20 path. Unknown channel-filter ROM calls remain
 * read-only research candidates until their C5 ABI and register effects are
 * proven. */
void lab_run_bandwidth_probe(bool vendor_path)
{
    if (s_menu_active) {
        printf("C5VRX_BW_PROBE_REFUSED reason=%s\n",
               "menu_active");
        return;
    }

    bool saved_vendor_bw40=true;
    if (vendor_path && rf_get_vendor_bandwidth_lab(&saved_vendor_bw40) != ESP_OK) {
        printf("PHYLAB vendor_bandwidth_unavailable\n");
        return;
    }
    phy_rx_lab_stock();
    const analog_agc_mode_t saved_agc_mode = s_agc_mode;
    const agc_state_t saved_agc_state = s_agc_state;
    const rf_bw_mode_t saved_bw_mode = s_rf_bw_mode;
    const bool saved_bw40 = s_current_bw40;
    const afc_mode_t saved_afc_mode = s_afc_mode;
    const int saved_offset = rf_get_frequency_offset_khz();
    const bool saved_quiet = s_lab_quiet;

    s_agc_mode = ANALOG_AGC_MANUAL;
    s_afc_mode = AFC_MODE_OFF;
    if (saved_offset != 0) apply_frequency_offset_khz_tracked(0);
    s_lab_quiet = true;
    rf_set_fft_scale_force(false, 0);
    vTaskDelay(pdMS_TO_TICKS(250));
    lab_reset_correlation();

    printf("C5VRX_BW_PROBE_BEGIN gain=%u settle_ms=%u vendor_path=%u order=40,20\n",
           s_current_gain, LAB_BW_SETTLE_MS, vendor_path);

    const bool states[2] = {true, false};
    for (unsigned i = 0; i < 2; ++i) {
        const hw_transport_counters_t base = lab_counter_snapshot();
        if (vendor_path) {
            esp_err_t err=rf_set_vendor_bandwidth_lab(states[i]);
            if (err != ESP_OK) {
                printf("PHYLAB vendor_bw_error=%s\n",esp_err_to_name(err));
                break;
            }
            s_current_bw40=states[i];
        } else apply_rf_bandwidth(states[i]);
        vTaskDelay(pdMS_TO_TICKS(LAB_BW_SETTLE_MS));
        lab_print_row(vendor_path ? "VENDOR_BW_SWEEP" : "BW_SWEEP", &base);
        phy_rx_lab_dump(false);
    }

    if (vendor_path) {
        /* Failed restoration cannot silently resume a mismatched production
         * receiver. ESP_ERROR_CHECK reboots through calibrated startup. */
        ESP_ERROR_CHECK(rf_set_vendor_bandwidth_lab(saved_vendor_bw40));
        s_current_bw40=saved_vendor_bw40;
    }
    s_rf_bw_mode = saved_bw_mode;
    if (s_current_bw40 != saved_bw40) apply_rf_bandwidth(saved_bw40);
    s_afc_mode = saved_afc_mode;
    if (rf_get_frequency_offset_khz() != saved_offset) {
        apply_frequency_offset_khz_tracked(saved_offset);
    }
    s_agc_state = saved_agc_state;
    s_agc_mode = saved_agc_mode;
    s_lab_quiet = saved_quiet;
    printf("C5VRX_BW_PROBE_END restored_agc=%u restored_bw=%u\n",
           (unsigned)saved_agc_mode, saved_bw40 ? 40u : 20u);
}

/* Diagnostic only: the Phase5 decoder places Q4 bins at signed n + 0.5,
 * including 0x00 at (+0.5,+0.5). The production gain thresholds are still
 * calibrated to integer-bin metrics, so measure both during a frozen R sweep
 * before changing any control decision or fast observer stack. */
static centered_q4_metrics_t measure_centered_q4(const uint8_t *sample, size_t bytes)
{
    uint16_t hist[129] = {0};
    unsigned origin = 0;
    for (size_t i = 0; i < bytes; ++i) {
        int q = (int8_t)((sample[i] & 0x0fu) << 4) >> 4;
        int in_val = (int8_t)(sample[i] & 0xf0u) >> 4;
        int ci = 2 * in_val + 1;
        int cq = 2 * q + 1;
        int power = (ci * ci + cq * cq + 2) / 4;
        if (power <= 4) ++origin;
        ++hist[power]; /* Max power is 113, within the 129-bin histogram. */
    }
    centered_q4_metrics_t m = {.origin_permille = bytes ?
        (int)(origin * 1000u / bytes) : 1000};
    unsigned cumulative = 0;
    const unsigned p95_rank = (unsigned)((bytes * 95u + 99u) / 100u);
    bool median_found = false;
    for (unsigned p = 0; p <= 128u; ++p) {
        cumulative += hist[p];
        if (!median_found && cumulative >= (bytes + 1u) / 2u) {
            m.p_median = (int)p;
            median_found = true;
        }
        if (cumulative >= p95_rank) { m.p95 = (int)p; break; }
    }
    return m;
}

/* The controller is paused, so cached LAB rows would be stale. Measure each
 * 11p stage afresh from completed DMA regions; no CPU processing in AV path. */
static void lab_observe_11p(const char *stage)
{
    vTaskDelay(pdMS_TO_TICKS(1000));
    uint8_t sample[256];
    if (!rx_probe_copy_completed(sample)) {
        printf("PHY11P stage=%s sample=unavailable\n",stage);
        return;
    }
    control_metrics_t m=analyze_control_window(sample,sizeof(sample),0);
    centered_q4_metrics_t center=measure_centered_q4(sample,sizeof(sample));
    printf("PHY11P stage=%s freq=%u G=%u bw=%u offset=%d samples=%u "
           "P50=%d P50_center=%d P95_center=%d Q_phase=%d outer_permille=%d "
           "origin_permille=%d origin_center_permille=%d video=hardware_pending\n",
           stage,rf_get_frequency_mhz(),s_current_gain,rf_get_analog_bandwidth()?40:20,
           rf_get_frequency_offset_khz(),(unsigned)sizeof(sample),m.p_median,
           center.p_median,center.p95,m.q_phase,m.clip_permille,m.origin_permille,
           center.origin_permille);
}

void lab_run_11p_probe(void)
{
    if (rf_native_agc_active() || s_menu_active ||
        s_pre_q4_probe_active || s_rssi_probe_active) {
        printf("PHY11P refused=other_lab_menu_or_native_owner\n");
        return;
    }
    analog_agc_mode_t saved_mode=s_agc_mode;
    s_rssi_probe_active=true;
    s_agc_mode=ANALOG_AGC_MANUAL;
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_err_t result=phy_rx_lab_run_11p_probe(lab_observe_11p);
    /* A failed exact rollback must not resume control over an unknown PHY. */
    if (result==ESP_FAIL) { printf("PHY11P rollback_failed rebooting\n"); esp_restart(); }
    ++s_profile_generation;
    s_agc_mode=saved_mode;
    s_rssi_probe_active=false;
    printf("PHY11P done status=%d\n",(int)result);
}

void lab_predemod_status(void)
{
    const arc_gain_table_t *table = rf_get_arc_gain_table();
    uint32_t glitches = s_predemod_glitches, samples = s_predemod_samples;
    int dc_i = s_v3_dc_i_mstep, dc_q = s_v3_dc_q_mstep;
    printf("PREDEMOD lanes=%s lane=%u adc_step=%u gain=%u table_max=%u freq=%u "
           "observer_glitch_ppm=%u observer_samples=%lu receiver_dc_lane0_msteps=%d/%d\n",
           c5vrx4_lane_mode_name(), rf_get_iq_lanes(), 64u >> rf_get_iq_lanes(),
           s_current_gain, table ? table->max_index : 0u, rf_get_frequency_mhz(),
           predemod_ppm(glitches, samples), (unsigned long)samples, dc_i, dc_q);
    predemod_correction_print();
    /* Yield between blocks: the no-driver USB console busy-waits on a full
     * FIFO, and a long dump at priority 1 starved IDLE (task WDT 2026-10-07). */
    vTaskDelay(pdMS_TO_TICKS(5));
    phy_rx_lab_predemod_status();
    vTaskDelay(pdMS_TO_TICKS(5));
    bw_status_print();
    agc_mask_status_print();
    vTaskDelay(pdMS_TO_TICKS(5));
    idle_raster_status_print();
    sync_flywheel_status_print();
    vTaskDelay(pdMS_TO_TICKS(5));
    gain_readback_print();
    vTaskDelay(pdMS_TO_TICKS(5));
    printf("RADIUS_BOOST enabled=%u active=%u entries=%lu exits=%lu streak=%u "
           "band_p50=30..46 normal_p50=13..32 gain=%u p50=%d p95=%d clip_pm=%d coherence=%d "
           "hardware_acceptance=pending\n",
           s_direct_gain_v3.boost_enabled, s_direct_gain_v3.boost,
           (unsigned long)s_direct_gain_v3.boost_entries,
           (unsigned long)s_direct_gain_v3.boost_exits, s_direct_gain_v3.boost_streak,
           s_current_gain, s_v3_p50, s_v3_p95, s_v3_clip_pm, s_v3_coherence);
}

bool lab_dco_measure(int dc[2])
{
    predemod_window_t w;
    vTaskDelay(pdMS_TO_TICKS(50));
    if (!predemod_collect(64, &w)) return false;
    dc[0] = w.dc_i;
    dc[1] = w.dc_q;
    return true;
}

static void lab_dco_observe(const char *stage)
{
    vTaskDelay(pdMS_TO_TICKS(1000));
    predemod_window_t w;
    if (!predemod_collect(64, &w)) { printf("DCO stage=%s sample=unavailable\n", stage); return; }
    predemod_print("DCO", stage, 0, &w);
}

void lab_run_dco_probe(void)
{
    analog_agc_mode_t saved_mode;
    if (!predemod_pause("DCO", &saved_mode)) return;
    esp_err_t result = phy_rx_lab_run_dco_probe(lab_dco_measure, lab_dco_observe);
    /* A failed exact rollback must not resume control over an unknown PHY. */
    /* Board 2026-10-06: after work mode the forced DC pair stays until the
     * next table replay, so an exact read-back fails without an unknown PHY. */
    if (result == ESP_FAIL) printf("DCO restore_mismatch (see restore_diff)\n");
    /* Work mode replays the current gain's row only on a gain write; without
     * it the IQ stayed dead (P50 1, origin 1000) after the probe. */
    rf_set_rx_gain(true, s_current_gain);
    predemod_resume(saved_mode);
    printf("DCO done status=%d\n", (int)result);
}

/* '6': DC correction from the last '#' search on/off, then the DC and
 * coherence it leaves (the A/B for the range edge with a weak VTX). */

static void lab_filter_observe(const char *stage, int offset)
{
    vTaskDelay(pdMS_TO_TICKS(1000));
    predemod_window_t w;
    if (!predemod_collect(48, &w)) { printf("FILTER stage=%s sample=unavailable\n", stage); return; }
    predemod_print("FILTER", stage, offset, &w);
}

void lab_run_filter_sweep(void)
{
    analog_agc_mode_t saved_mode;
    if (!predemod_pause("FILTER", &saved_mode)) return;
    esp_err_t result = phy_rx_lab_run_filter_sweep(lab_filter_observe);
    if (result == ESP_FAIL) { printf("FILTER rollback_failed rebooting\n"); esp_restart(); }
    predemod_resume(saved_mode);
    printf("FILTER done status=%d\n", (int)result);
}

void agc_mask_observe(void)
{
    if (!c5vrx4_agc_mask_active()) return;
    uint8_t sample[RX_PROBE_REGIONS * RX_PROBE_REGION_BYTES];
    if (!rx_probe_copy_completed(sample)) return;
    unsigned set = 0;
    for (unsigned k = 0; k < sizeof(sample); ++k) set += sample[k] & 1u;
    unsigned pm = set * 1000u / sizeof(sample);
    s_agc_flag_share_pm = (7u * s_agc_flag_share_pm + pm) / 8u;
}

static void agc_mask_status_print(void)
{
    uint8_t flag = c5vrx4_agc_flag();
    printf("AGC_MASK native=%u enabled=%u active=%u flag=%s%d%s calibrations=%u last=%s "
           "separation_pm=%u lead_samples=%u lag_samples=%u live_flag_share_pm=%u lane_cost=Q_LSB "
           "pacing=%s hardware_acceptance=pending\n",
           rf_native_agc_active(), c5vrx4_agc_mask_enabled(), c5vrx4_agc_mask_active(),
           flag == C5VRX4_AGC_FLAG_UNKNOWN ? "none" : "DIAG", flag == C5VRX4_AGC_FLAG_UNKNOWN ? -1 :
           28 + (flag & 3), (flag != C5VRX4_AGC_FLAG_UNKNOWN && (flag & 0x80u)) ? "_inverted" : "",
           s_witness_runs, s_witness_result, s_witness_last.separation_pm,
           s_witness_last.lead_samples, s_witness_last.lag_samples, s_agc_flag_share_pm,
           c5vrx4_agc_mask_active() ? "off_mask_needs_every_acquisition" : "unchanged");
}

/* ---- Range labs (2026-10-04, PREDEMOD_LAB.md) ----------------------------
 * ''' : sigRSSI mode A/B. In native mode the interesting question is whether
 *       the sigRSSI configuration stops the ~25-50 us packet re-acquisitions
 *       (watch Q rows and, while masking, the live flag share).
 * '"' : phy_param_track_tot(1,0) A/B (temperature-tracked RX recalibration). */
static void lab_observe_range(const char *stage)
{
    vTaskDelay(pdMS_TO_TICKS(500));
    if (c5vrx4_agc_mask_active()) for (unsigned k = 0; k < 8u; ++k) { agc_mask_observe(); vTaskDelay(pdMS_TO_TICKS(10)); }
    uint8_t sample[256];
    if (!rx_probe_copy_completed(sample)) { printf("RANGELAB stage=%s sample=unavailable\n", stage); return; }
    control_metrics_t m = analyze_control_window(sample, sizeof(sample), 0);
    centered_q4_metrics_t center = measure_centered_q4(sample, sizeof(sample));
    printf("RANGELAB stage=%s freq=%u native=%u G=%u P50=%d P95_center=%d Q_phase=%d "
           "outer_permille=%d origin_permille=%d agc_flag_share_pm=%u video=hardware_pending\n",
           stage, rf_get_frequency_mhz(), rf_native_agc_active(), s_current_gain, m.p_median,
           center.p95, m.q_phase, m.clip_permille, m.origin_permille,
           c5vrx4_agc_mask_active() ? s_agc_flag_share_pm : 0u);
}

/* Controllers paused for the A/B (native keeps its own hardware AGC). */
static bool range_lab_pause(const char *tag, analog_agc_mode_t *saved)
{
    if ((s_menu_active && !IDLE_RASTER_ACTIVE()) || s_pre_q4_probe_active || s_rssi_probe_active) {
        printf("%s refused=other_lab_or_menu\n", tag);
        return false;
    }
    *saved = s_agc_mode;
    s_rssi_probe_active = true;
    if (!rf_native_agc_active()) s_agc_mode = ANALOG_AGC_MANUAL;
    vTaskDelay(pdMS_TO_TICKS(100));
    return true;
}

void lab_run_sigrssi(void)
{
    analog_agc_mode_t saved;
    if (!range_lab_pause("SIGRSSI", &saved)) return;
    phy_rx_lab_rssi_stats_t st = {0};
    esp_err_t result = phy_rx_lab_run_sigrssi_probe(lab_observe_range, &st);
    if (result == ESP_FAIL) { printf("SIGRSSI rollback_failed rebooting\n"); esp_restart(); }
    if (result == ESP_OK)
        printf("SIGRSSI freq=%u native=%u samples=%u min_dbm=%d p10_dbm=%d p50_dbm=%d p90_dbm=%d "
               "max_dbm=%d mean_dbm=%d.%d source=phy_get_sigrssi\n",
               rf_get_frequency_mhz(), rf_native_agc_active(), st.samples, st.min_dbm, st.p10_dbm,
               st.p50_dbm, st.p90_dbm, st.max_dbm, st.mean_dbm_x10 / 10,
               (st.mean_dbm_x10 < 0 ? -st.mean_dbm_x10 : st.mean_dbm_x10) % 10);
    ++s_profile_generation;
    s_agc_mode = saved;
    s_rssi_probe_active = false;
    printf("SIGRSSI done status=%d\n", (int)result);
}

void lab_run_phy_track(void)
{
    analog_agc_mode_t saved;
    if (!range_lab_pause("PHYTRACK", &saved)) return;
    esp_err_t result = phy_rx_lab_run_track_probe(lab_observe_range);
    ++s_profile_generation;
    s_agc_mode = saved;
    s_rssi_probe_active = false;
    printf("PHYTRACK done status=%d\n", (int)result);
}

/* '/' : digital RX filter / ADC-rate lab (PREDEMOD_LAB.md). With the VTX off
 * the noise width shows whether a digital filter sits ahead of the tap; with
 * a steady weak VTX, winding (clicks) and Q_phase show what it does to the
 * picture. Direct Gain only, fixed gain. */

static void lab_observe_rf(const char *tag, const char *stage, int arg, unsigned hold_ms)
{
    vTaskDelay(pdMS_TO_TICKS(hold_ms));
    float psd[PREDEMOD_FFT_N] = {0};
    uint8_t sample[RX_PROBE_REGIONS * RX_PROBE_REGION_BYTES];
    uint32_t glitches = 0, samples = 0;
    unsigned got = 0;
    int q_sum = 0, p_sum = 0, wind_sum = 0, strong_sum = 0, clip_max = 0;
    for (unsigned tries = 0; tries < DFILT_WINDOWS * 3u && got < DFILT_WINDOWS; ++tries) {
        vTaskDelay(1);
        if (!rx_probe_copy_completed(sample)) continue;
        for (unsigned r = 0; r < RX_PROBE_REGIONS; ++r) {
            predemod_psd_accumulate(sample + r * RX_PROBE_REGION_BYTES, psd);
            glitches += predemod_glitches(sample + r * RX_PROBE_REGION_BYTES, RX_PROBE_REGION_BYTES, 6);
        }
        samples += RX_PROBE_REGIONS * (RX_PROBE_REGION_BYTES - 2u);
        control_metrics_t m = analyze_control_window(sample, sizeof(sample), 0);
        q_sum += m.q_phase;
        p_sum += m.p_median;
        wind_sum += m.winding_permille;
        strong_sum += m.strong_winding_permille;
        if (m.clip_permille > clip_max) clip_max = m.clip_permille;
        ++got;
    }
    if (!got) { printf("%s stage=%s arg=%d sample=unavailable\n", tag, stage, arg); return; }
    printf("%s stage=%s arg=%d freq=%u G=%u lane=%u windows=%u width_khz=%u nbw_khz=%u glitch_ppm=%u "
           "P50=%d Q_phase=%d wind_pm=%d strong_wind_pm=%d clip_pm=%d video=hardware_pending\n",
           tag, stage, arg, rf_get_frequency_mhz(), s_current_gain, rf_get_iq_lanes(), got,
           predemod_psd_width_khz(psd), predemod_psd_nbw_khz(psd), predemod_ppm(glitches, samples), p_sum / (int)got,
           q_sum / (int)got, wind_sum / (int)got, strong_sum / (int)got, clip_max);
}

static void lab_observe_dfilt(const char *stage, int arg) { lab_observe_rf("DFILT", stage, arg, 20); }

/* ';' : BW20 channel setup with the analog filter wide open (2026-10-04).
 * esp-sdr (ESPARGOS, rx_bandwidth.h) measured on the same 80 MS/s dump that
 * PHY channel mode 0 (BW20) tops out at ~23 MHz even at RX0 code 0, while
 * mode 1 (BW40) reaches 48 MHz, and notes that the curves include the
 * digital-filter response; the vendor BW20 path also selects digital filter
 * mode 4. If that ~23 MHz edge is a steep digital filter ahead of the tap,
 * BW20 + wide analog removes the folded skirt that an RC filter leaves
 * (compare nbw_khz). C5VRX-3 rejected BW20 only with the calibrated, much
 * narrower analog codes. Public API for the width change; every stage holds
 * 1 s for a picture comparison; ends with the normal BW40 retune, which
 * re-applies the stored fixed-BW code. Direct Gain, current gain. */
void lab_run_bw20_wide(void)
{
    bool vendor40 = false;
    if (rf_get_vendor_bandwidth_lab(&vendor40) != ESP_OK || !vendor40 || !s_current_bw40) {
        printf("BW20WIDE refused=not_in_bw40\n");
        return;
    }
    analog_agc_mode_t saved;
    if (!predemod_pause("BW20WIDE", &saved)) return;
    const afc_mode_t saved_afc = s_afc_mode;
    const int saved_offset = rf_get_frequency_offset_khz();
    s_afc_mode = AFC_MODE_OFF;
    if (saved_offset) apply_frequency_offset_khz_tracked(0);
    printf("BW20WIDE begin freq=%u G=%u fixed_bw=%u code=%d skirt=%d reference=ESPARGOS_esp-sdr\n",
           rf_get_frequency_mhz(), s_current_gain, c5vrx4_fixed_bw_enabled(),
           phy_rx_lab_filter_code(), phy_rx_lab_filter_skirt());
    lab_observe_rf("BW20WIDE", "BW40_CURRENT", phy_rx_lab_filter_code(), 1000);
    esp_err_t err = rf_set_vendor_bandwidth_lab(false);
    if (err == ESP_OK) {
        s_current_bw40 = false;
        lab_observe_rf("BW20WIDE", "BW20_AFTER_RESTORE", phy_rx_lab_filter_code(), 1000);
        static const int codes[] = {0, 8, 16};
        for (unsigned k = 0; k < sizeof(codes) / sizeof(codes[0]); ++k) {
            if (phy_rx_lab_filter_poke_live(codes[k])) lab_observe_rf("BW20WIDE", "BW20_CODE", codes[k], 1000);
            else printf("BW20WIDE code=%d refused=filter_write\n", codes[k]);
        }
    } else printf("BW20WIDE vendor_bw20_error=%s\n", esp_err_to_name(err));
    /* A failed return to BW40 must not resume a mixed receiver. */
    ESP_ERROR_CHECK(rf_set_vendor_bandwidth_lab(true));
    s_current_bw40 = true;
    s_afc_mode = saved_afc;
    if (saved_offset) apply_frequency_offset_khz_tracked(saved_offset);
    lab_observe_rf("BW20WIDE", "RESTORED", phy_rx_lab_filter_code(), 200);
    predemod_resume(saved);
    printf("BW20WIDE done\n");
}

void lab_run_dfilt(void)
{
    analog_agc_mode_t saved;
    if (!predemod_pause("DFILT", &saved)) return;
    esp_err_t result = phy_rx_lab_run_dfilt_probe(lab_observe_dfilt);
    if (result == ESP_FAIL) { printf("DFILT rollback_failed rebooting\n"); fflush(stdout); esp_restart(); }
    predemod_resume(saved);
    printf("DFILT done status=%d\n", (int)result);
}

static void lab_observe_native_hold(const char *stage, unsigned cycle)
{
    volatile uint32_t *proxy_a=(volatile uint32_t *)0x600A706Cu;
    volatile uint32_t *proxy_b=(volatile uint32_t *)0x600A7078u;
    uint32_t before_a=*proxy_a, before_b=*proxy_b;
    /* One second for visual A/B; cycle stress uses >=2 PAL/NTSC fields. */
    vTaskDelay(pdMS_TO_TICKS(s_native_hold_cycles==1 ? 1000 : 40));
    uint8_t sample[256];
    if (!rx_probe_copy_completed(sample)) {
        printf("NATIVEHOLD stage=%s cycle=%u sample=unavailable\n",stage,cycle);
        return;
    }
    control_metrics_t m=analyze_control_window(sample,sizeof(sample),0);
    centered_q4_metrics_t center=measure_centered_q4(sample,sizeof(sample));
    printf("NATIVEHOLD stage=%s cycle=%u freq=%u bw=%u offset=%d "
           "proxyA=%08lx/%08lx proxyB=%08lx/%08lx samples=%u "
           "bb_ctrl=%08lx force_ctrl=%08lx rf_ctrl=%08lx "
           "P50=%d P50_center=%d P95_center=%d Q_phase=%d outer_pm=%d origin_pm=%d "
           "live_gain=unknown video=hardware_pending\n",
           stage,cycle,rf_get_frequency_mhz(),rf_get_analog_bandwidth()?40:20,
           rf_get_frequency_offset_khz(),(unsigned long)before_a,(unsigned long)*proxy_a,
           (unsigned long)before_b,(unsigned long)*proxy_b,(unsigned)sizeof(sample),
           (unsigned long)*(volatile uint32_t *)0x600A7030u,
           (unsigned long)*(volatile uint32_t *)0x600A702Cu,
           (unsigned long)*(volatile uint32_t *)0x600A705Cu,
           m.p_median,center.p_median,center.p95,m.q_phase,m.clip_permille,m.origin_permille);
}

void lab_run_native_hold(unsigned cycles)
{
#ifndef C5VRX_PHY_RX_LAB_PINNED
    (void)cycles;
    printf("NATIVEHOLD refused=unverified_PHY_binary\n");
    return;
#endif
    if (!rf_native_agc_active() || s_menu_active ||
        s_pre_q4_probe_active || s_rssi_probe_active) {
        printf("NATIVEHOLD refused=busy_or_not_native hint=N_native_on_next_boot\n");
        return;
    }
    analog_agc_mode_t saved_mode=s_agc_mode;
    s_rssi_probe_active=true;
    s_agc_mode=ANALOG_AGC_MANUAL;
    vTaskDelay(pdMS_TO_TICKS(100));
    /* Stop the native pacing ISR before touching the same BB gate. Resuming
     * afterwards restores the operator's paced/continuous selection. */
    c5vrx4_suspend();
    s_native_hold_cycles=cycles;
    esp_err_t result=phy_rx_lab_run_native_hold(cycles,lab_observe_native_hold);
    if (result==ESP_FAIL) { printf("NATIVEHOLD restore_failed rebooting\n"); esp_restart(); }
    c5vrx4_resume();
    ++s_profile_generation;
    s_agc_mode=saved_mode;
    s_rssi_probe_active=false;
    printf("NATIVEHOLD done status=%d\n",(int)result);
}

/* Request a fresh vendor PHY calibration on the next boot. The live receiver
 * is never recalibrated in place: only the stored PHY calibration namespace is
 * erased, then C5VRX reboots immediately. */
void lab_request_fresh_phy_calibration(void)
{
    if (s_menu_active || s_pre_q4_probe_active) {
        printf("C5VRX_PREQ4_FULLCAL_REFUSED reason=%s\n",
               s_menu_active ? "menu_active" : "preq4_busy");
        return;
    }

    esp_err_t err = rf_prepare_fresh_phy_calibration();
    if (err != ESP_OK) {
        printf("C5VRX_PREQ4_FULLCAL_ERROR err=%s\n", esp_err_to_name(err));
        return;
    }

    printf("C5VRX_PREQ4_FULLCAL_ARMED action=reboot next_boot=fresh_vendor_phy_calibration\n");
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(120));
    esp_restart();
}

/* Native hardware AGC is an opt-in per-boot choice (Direct Gain V4 is the
 * default) because the vendor loop cannot be restored after C5VRX disables
 * it. 'N' and the RF page profile cycle flip it and reboot; rf_start()
 * decides ownership before PHY use. */
void lab_toggle_native_agc_boot(void)
{
    /* The no-carrier idle raster is not the user menu: switching the gain
     * owner (a reboot) is exactly what a stuck native receiver may need. */
    const bool menu = s_menu_active && !IDLE_RASTER_ACTIVE();
    if (menu || s_pre_q4_probe_active) {
        printf("C5VRX_NATIVE_AGC_REFUSED reason=%s\n",
               menu ? "menu_active" : "preq4_busy");
        return;
    }
    bool enable = !rf_native_agc_active();
    esp_err_t err = rf_request_native_agc_boot(enable);
    if (err != ESP_OK) {
        printf("C5VRX_NATIVE_AGC_ERROR err=%s\n", esp_err_to_name(err));
        return;
    }
    printf("C5VRX_NATIVE_AGC_ARMED next_boot=%s action=reboot\n",
           enable ? "native_hw_agc" : "firmware_gain_control");
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(120));
    esp_restart();
}

void lab_dco_quiet(const char *stage) { (void)stage; }

void lab_run_sigrssi_ladder(void)
{
    static const uint8_t gains[] = {83, 76, 70, 62, 54, 47, 40, 34, 30, 25};
    if (rf_native_agc_active()) { printf("SIGLADDER refused=native_owner\n"); return; }
    analog_agc_mode_t saved;
    if (!range_lab_pause("SIGLADDER", &saved)) return;
    const uint8_t saved_gain = s_current_gain;
    esp_err_t result = ESP_OK;
    for (unsigned k = 0; k < sizeof(gains) && result == ESP_OK; ++k) {
        lab_apply_vendor_gain(gains[k]);
        vTaskDelay(pdMS_TO_TICKS(30));
        uint8_t sample[256];
        control_metrics_t m = {0};
        bool have = false;
        for (unsigned t = 0; t < 20u && !have; ++t) {
            vTaskDelay(1);
            have = rx_probe_copy_completed(sample);
        }
        if (have) m = analyze_control_window(sample, sizeof(sample), 0);
        int wide_dbm = -127;
        bool wide = rf_try_get_wideband_rssi_dbm(&wide_dbm);
        phy_rx_lab_rssi_stats_t st = {0};
        result = phy_rx_lab_run_sigrssi_probe_forced(lab_observe_range, &st);
        if (result != ESP_OK) break;
        printf("SIGLADDER freq=%u G=%u P50=%d Q=%d clip_pm=%d origin_pm=%d sig_p10=%d sig_p50=%d "
               "sig_p90=%d sig_mean=%d.%d phy_rssi=%d phy_rssi_valid=%u\n",
               rf_get_frequency_mhz(), gains[k], m.p_median, m.q_phase, m.clip_permille,
               m.origin_permille, st.p10_dbm, st.p50_dbm, st.p90_dbm, st.mean_dbm_x10 / 10,
               abs(st.mean_dbm_x10 % 10), wide_dbm, wide);
    }
    if (result == ESP_FAIL) { printf("SIGLADDER rollback_failed rebooting\n"); esp_restart(); }
    lab_apply_vendor_gain(saved_gain);
    ++s_profile_generation;
    s_agc_mode = saved;
    s_rssi_probe_active = false;
    printf("SIGLADDER done status=%d restored_gain=%u\n", (int)result, saved_gain);
}
