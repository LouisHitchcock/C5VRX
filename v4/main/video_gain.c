/* C5VRX-4: gain responsibilities. */
#include "video_internal.h"

static void direct_gain_v3_apply_target(uint8_t target, uint32_t profile, uint32_t phy);
static void direct_gain_v5_dc_observe(const uint8_t *sample, size_t bytes,
                                      const dg3_observation_t *o);
static bool bw_in_narrow_gear(void);
static void direct_gain_v5_bw_gear(const dg3_observation_t *o);
#define V5_BW_REENTRY_US 5000000u

#define V5_BW_CLEAR_HEADROOM 8u

#define V5_BW_DWELL_US 1000000u

#include "phase8_gain_lut.h"

direct_gain_v3_t s_direct_gain_v3;

volatile int s_v3_p50, s_v3_p90, s_v3_p95, s_v3_origin_pm;

/* Receiver DC at maximum gain (quiet windows), milli-steps of lane 0. */
volatile int s_v3_dc_i_mstep, s_v3_dc_q_mstep;

volatile uint32_t s_v3_bw_switches;

volatile int s_v3_clip_pm, s_v3_coherence;

TaskHandle_t s_v3_observer_task_handle;

TaskHandle_t s_v3_sentinel_task_handle;

static volatile uint32_t s_v3_fast_overload_state;

static dg3_observation_t s_v3_fast_overload_observation;

static rx_control_epoch_t s_v3_fast_overload_epoch;

static void direct_gain_v3_apply_target(uint8_t target, uint32_t profile, uint32_t phy)
{
    if (phy != phy_rx_lab_generation() || phy_rx_lab_busy() ||
        profile != s_profile_generation ||
        s_agc_mode != ANALOG_AGC_ACTIVE ||
        s_rx_profile != RX_PROFILE_DIRECT_GAIN) return;
    if (!phy_rx_lab_try_actuator(phy)) return;
    s_shadow_gain = target;
    s_agc_state = s_direct_gain_v3.state == DG3_HOLD ?
                  AGC_STATE_TRACK : AGC_STATE_LEARN;
    s_last_direct_gain_state = s_direct_gain_v3.state == DG3_HOLD ?
                               DG3_HOLD : DG3_ACQUIRE;
    s_last_direct_gain_target = target;
    s_last_direct_gain_delta = (int)target - (int)s_current_gain;
    s_last_direct_gain_total_writes = s_direct_gain_v3.writes;
    s_last_direct_gain_hold_cycles = s_direct_gain_v3.holds;
    if (s_direct_gain_v3.lane != rf_get_iq_lanes()) {
        /* Range lane switch: bump the epoch so no in-flight measurement
         * mixes samples from both lane sets. */
        rf_set_iq_lanes(s_direct_gain_v3.lane);
        ++s_gain_transition_count;
    }
    if (target != s_current_gain) {
        __sync_synchronize();
        uint8_t applied = apply_rx_gain_for_generation(target, phy);
        direct_gain_v3_sync_applied(&s_direct_gain_v3, applied,
                                    (uint64_t)esp_timer_get_time());
        __sync_synchronize();
    }
    phy_rx_lab_end_actuator();
}

/* ESP timer callback does no DMA or sample processing. It only wakes the
 * sentinel task every 200 us; the sentinel requests full control work only
 * when a fresh sample block shows a clipped Q4 envelope. */
void direct_gain_v3_sentinel_timer_cb(void *arg)
{
    (void)arg;
    /* The flywheel shares the 200 us tick (board 2026-10-06: at 100 us and
     * observer priority it starved IDLE and the console, task WDT every
     * 5 s, and delayed V5). It runs below the observer and does twice the
     * work per wake. */
    if (s_sfw_task_handle) xTaskNotifyGive(s_sfw_task_handle);
    if (s_v3_sentinel_task_handle)
        xTaskNotifyGive(s_v3_sentinel_task_handle);
    /* V5: the observer runs on the same 200 us cadence; a new RX
     * descriptor completes every ~102 us. (A 1 kHz observer against the
     * task watchdog cut range in flight on another board, PR #177 report;
     * the CPU budget is measured instead: DG3_OBS obs_us_avg/max.) */
    if (s_v3_observer_task_handle)
        xTaskNotifyGive(s_v3_observer_task_handle);
}

void direct_gain_v3_sentinel_task(void *arg)
{
    (void)arg;
    uint8_t sample[64];
    for (;;) {
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        bool active = s_rx_profile == RX_PROFILE_DIRECT_GAIN &&
                      s_agc_mode == ANALOG_AGC_ACTIVE && !s_menu_active &&
                      !phy_rx_lab_busy() && !s_pre_q4_probe_active && !s_rssi_probe_active;
        if (!active || s_v3_fast_overload_state != 0u ||
            s_rx_dma_ch < 0 || s_rx_dma_ch >= 3 || s_rx_dscr_count < 2)
            continue;

        rx_control_epoch_t epoch = {s_profile_generation, phy_rx_lab_generation(),
                                    s_gain_transition_count};
        uint32_t gain_epoch = epoch.gain;
        uint32_t active_addr = AHB_DMA.channel[s_rx_dma_ch].in.in_dscr_bf0.val;
        int active_idx = find_dscr_index(s_rx_dscr_nodes, s_rx_dscr_count,
                                         active_addr);
        if (active_idx < 0) continue;
        int sample_idx = (active_idx - 1 + s_rx_dscr_count) % s_rx_dscr_count;
        uint8_t *src = s_rx_dscr_nodes[sample_idx].buffer;
        const size_t offset = 1984u;
        if (!src || s_rx_dscr_nodes[sample_idx].length < 4092u ||
            src < s_raw_ring || src + offset + sizeof(sample) >
                                  s_raw_ring + sizeof(s_raw_ring)) continue;
        sync_dma_m2c(src + offset, sizeof(sample));
        memcpy(sample, src + offset, sizeof(sample));
        active_addr = AHB_DMA.channel[s_rx_dma_ch].in.in_dscr_bf0.val;
        if (find_dscr_index(s_rx_dscr_nodes, s_rx_dscr_count,
                            active_addr) == sample_idx ||
            gain_epoch != s_gain_transition_count) continue;

        dg3_observation_t observation = direct_gain_v3_measure(
            sample, sizeof(sample), c5vrx_phase8_gain_lut,
            (uint64_t)esp_timer_get_time());
        if (observation.clip_pm < 125u && observation.p95 < 95u) continue;
        rx_control_epoch_t current = {s_profile_generation, phy_rx_lab_generation(),
                                      s_gain_transition_count};
        if (!rx_control_epoch_equal(epoch, current) || phy_rx_lab_busy()) continue;
        if (!__sync_bool_compare_and_swap(&s_v3_fast_overload_state, 0u, 1u))
            continue;
        s_v3_fast_overload_observation = observation;
        s_v3_fast_overload_epoch = epoch;
        __sync_synchronize();
        __sync_lock_test_and_set(&s_v3_fast_overload_state, 2u);
        if (s_v3_observer_task_handle)
            xTaskNotifyGive(s_v3_observer_task_handle);
    }
}

/* Receiver DC at maximum gain, from quiet (no-carrier) windows: mean signed
 * nibble relative to the bucket centre, rescaled to lane-0 milli-steps. At
 * ~0.6 step of noise a DC of a few tenths of a step biases the quantizer
 * cells; this measures whether that matters before any LUT compensation. */
static void direct_gain_v5_dc_observe(const uint8_t *sample, size_t bytes,
                                      const dg3_observation_t *o)
{
    const direct_gain_v3_t *v3 = &s_direct_gain_v3;
    if (v3->current_gain != v3->table.max_index || !v3->lane ||
        o->clip_pm || o->coherence >= 45u || o->p95 >= 40u || !bytes) return;
    int32_t si = 0, sq = 0;
    for (size_t n = 0; n < bytes; ++n) {
        si += 2 * ((int8_t)(sample[n] & 0xF0u) >> 4) + 1;
        sq += 2 * ((int8_t)(uint8_t)(sample[n] << 4) >> 4) + 1;
    }
    /* mean(2x+1)/2 steps -> milli-steps of this lane -> lane-0 units. */
    int di = (int)(si * 500 / (int32_t)bytes) >> v3->lane;
    int dq = (int)(sq * 500 / (int32_t)bytes) >> v3->lane;
    s_v3_dc_i_mstep = (7 * s_v3_dc_i_mstep + di) / 8;
    s_v3_dc_q_mstep = (7 * s_v3_dc_q_mstep + dq) / 8;
}

/* V5 bandwidth gear (RF BW mode AUTO). A narrower filter lowers the noise
 * bandwidth (pre-detection CNR and, because span75 resamples at 13.33 MS/s
 * without an anti-alias filter, post-detection aliasing) but trims
 * wideband-FM detail/chroma, so it is the last gear: only at maximum analog
 * gain, on the noise-referenced lane cap, with a present but starved or
 * incoherent carrier for 1 s. It returns after 1 s of clear recovery. Each
 * switch is a rare PHY write; the gain epoch is bumped so no measurement
 * straddles it.
 * With the fixed analog BW calibrated, the gear switches between the normal
 * code and the measured edge profile (c5vrx4_bw_edge_code: analog code,
 * optionally the digital BW20 filter), and stays off when calibration found
 * no setting >= 0.5 dB better. Before calibration it is the original digital
 * BW20/BW40 gear. */

/* Anti-hunt (review 2026-10-06): the narrow filter lowers the noise, so V5
 * steps one index below maximum and the old "clear" (any gain below max)
 * fired after 1 s; wide again, V5 back at max, narrow again - a full PHY
 * restore (a glitch) every 1-2 s exactly at the range edge, worst with a
 * fixed lane (lane >= cap always true). Leave only with real headroom, and
 * hold off re-entry after an exit. */

static bool bw_in_narrow_gear(void)
{
    return bw_fixed_calibrated() ? rf_fixed_bw_edge_active() : !s_current_bw40;
}

static void direct_gain_v5_bw_gear(const dg3_observation_t *o)
{
    static uint64_t weak_since, strong_since, exit_us;
    const direct_gain_v3_t *v3 = &s_direct_gain_v3;
    const bool fixed = bw_fixed_calibrated();
    if (s_rf_bw_mode != RF_BW_MODE_AUTO || !c5vrx4_edge_gear_enabled() ||
        (fixed && c5vrx4_bw_edge_code() == C5VRX4_BW_UNCALIBRATED)) {
        if (fixed && rf_fixed_bw_edge_active()) bw_set_edge(false);
        weak_since = strong_since = 0;
        return;
    }
    uint64_t now = o->observed_us;
    bool at_max = v3->current_gain == v3->table.max_index;
    bool present = o->origin_pm < 650u && o->coherence >= 30u;
    bool edge = at_max && v3->lane >= v3->lane_cap && present &&
                (o->p50 < 13u || o->coherence < 70u);
    bool headroom = (unsigned)v3->current_gain + V5_BW_CLEAR_HEADROOM <=
                    (unsigned)v3->table.max_index;
    bool clear = headroom && o->p50 >= 13u && o->coherence >= 85u;
    if (!bw_in_narrow_gear()) {
        strong_since = 0;
        if (!edge || (exit_us && now - exit_us < V5_BW_REENTRY_US)) { weak_since = 0; return; }
        if (!weak_since) weak_since = now;
        if (now - weak_since < V5_BW_DWELL_US) return;
        if (fixed) bw_set_edge(true);
        else apply_rf_bandwidth(false);
    } else {
        weak_since = 0;
        if (!clear) { strong_since = 0; return; }
        if (!strong_since) strong_since = now;
        if (now - strong_since < V5_BW_DWELL_US) return;
        if (fixed) bw_set_edge(false);
        else apply_rf_bandwidth(true);
        exit_us = now ? now : 1u;
    }
    weak_since = strong_since = 0;
    ++s_v3_bw_switches;
    ++s_gain_transition_count;
}

/* Observe four separated 64-byte regions in the latest completed descriptor.
 * This task never touches a descriptor still owned by the RX DMA engine and
 * never participates in the 40 MS/s video clock. Only this task writes gain. */
void direct_gain_v3_observer_task(void *arg)
{
    (void)arg;
    uint8_t sample[RX_PROBE_REGIONS * RX_PROBE_REGION_BYTES];
    uint32_t seen_profile = UINT32_MAX, seen_arc = UINT32_MAX, seen_phy = UINT32_MAX;
    bool was_active = false;
    int last_block_idx = -1;
    for (;;) {
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1));
        bool active = s_rx_profile == RX_PROFILE_DIRECT_GAIN &&
                      s_agc_mode == ANALOG_AGC_ACTIVE && !s_menu_active &&
                      !phy_rx_lab_busy() && !s_pre_q4_probe_active && !s_rssi_probe_active;
        if (!active) {
            was_active = false;
            /* Never clear state 1 while the sentinel owns publication. */
            if (__sync_bool_compare_and_swap(&s_v3_fast_overload_state, 2u, 1u))
                __sync_lock_release(&s_v3_fast_overload_state);
            /* Finer range lanes are owned by Direct Gain only. */
            if (rf_get_iq_lanes()
                && c5vrx4_fixed_lane() == C5VRX4_LANE_ADAPTIVE
            ) {
                rf_set_iq_lanes(0u);
                ++s_gain_transition_count;
            }
            continue;
        }
        uint32_t profile = s_profile_generation;
        uint32_t arc = rf_get_arc_generation();
        uint32_t phy = phy_rx_lab_generation();
        if (!was_active || profile != seen_profile || arc != seen_arc || phy != seen_phy ||
            s_direct_gain_v3.current_gain != s_current_gain) {
            direct_gain_v3_reset(&s_direct_gain_v3, rf_get_arc_gain_table(),
                                 s_current_gain, rf_get_arc_survival_gain());
            /* The reset keeps an in-RAM map on the same table; a fresh boot
             * starts from the persisted one. */
            if (!s_direct_gain_v3.learned && s_dg3_saved_valid &&
                direct_gain_v3_import_map(&s_direct_gain_v3, &s_dg3_saved))
                ++s_dg3_map_imports;
            direct_gain_v3_enable_lanes(&s_direct_gain_v3,
                                        (uint8_t)(RF_IQ_LANE_SETS - 1u));
            direct_gain_v3_enable_boost(&s_direct_gain_v3, c5vrx4_radius_boost_enabled());
            if (rf_get_iq_lanes()
                && c5vrx4_fixed_lane() == C5VRX4_LANE_ADAPTIVE
            ) {
                rf_set_iq_lanes(0u);
                ++s_gain_transition_count;
            }
            seen_profile = profile;
            seen_arc = arc;
            seen_phy = phy;
            was_active = true;
        }

        if (__sync_bool_compare_and_swap(&s_v3_fast_overload_state, 2u, 1u)) {
            __sync_synchronize();
            dg3_observation_t overload = s_v3_fast_overload_observation;
            rx_control_epoch_t epoch = s_v3_fast_overload_epoch;
            __sync_lock_release(&s_v3_fast_overload_state);
            rx_control_epoch_t current = {profile, phy, s_gain_transition_count};
            if (!rx_control_observation_current(epoch, current, overload.observed_us,
                                                 (uint64_t)esp_timer_get_time())) continue;
            uint8_t emergency = direct_gain_v3_tick(&s_direct_gain_v3,
                                                    &overload);
            s_direct_gain_v3.lane = c5vrx4_lane_target(rf_get_iq_lanes(),
                s_direct_gain_v3.lane, NULL, 0, overload.observed_us);
            direct_gain_v3_apply_target(emergency, profile, phy);
            continue;
        }

        if (!c5vrx4_lane_window_ready((uint64_t)esp_timer_get_time())) continue;
        uint32_t gain_epoch = s_gain_transition_count;
        int block_idx = -1;
        if (!rx_probe_copy_completed_idx(sample, &block_idx)) continue;
        if (gain_epoch != s_gain_transition_count) continue;
        /* Never measure the same completed descriptor twice: the settle
         * check counts consecutive stable windows and must see new data. */
        if (block_idx == last_block_idx) continue;
        last_block_idx = block_idx;
        const int64_t obs_t0 = esp_timer_get_time();
        dg3_observation_t observation = direct_gain_v3_measure(
            sample, sizeof(sample), c5vrx_phase8_gain_lut,
            (uint64_t)esp_timer_get_time());
        s_v3_p50 = observation.p50;
        s_v3_p90 = observation.p90;
        s_v3_p95 = observation.p95;
        s_v3_origin_pm = observation.origin_pm;
        s_v3_clip_pm = observation.clip_pm;
        s_v3_coherence = observation.coherence;
        if (phy != phy_rx_lab_generation() || phy_rx_lab_busy()) continue;
        uint8_t target = direct_gain_v3_tick(&s_direct_gain_v3, &observation);
        s_direct_gain_v3.lane = c5vrx4_lane_target(rf_get_iq_lanes(),
            s_direct_gain_v3.lane, sample, sizeof(sample), observation.observed_us);
        direct_gain_v3_apply_target(target, profile, phy);
        direct_gain_v5_dc_observe(sample, sizeof(sample), &observation);
        /* The per-window glitch count and the DC sums for the (disabled)
         * digital recentring ran here every 200 us. Board 2026-10-06: the
         * task watchdog fired with gain_v3_obs on the CPU and the console
         * (USB input) starved; this work fed nothing anymore. */
        direct_gain_v5_bw_gear(&observation);
        const uint32_t obs_us = (uint32_t)(esp_timer_get_time() - obs_t0);
        s_obs_us_sum += obs_us;
        if (obs_us > s_obs_us_max) s_obs_us_max = obs_us;
        ++s_obs_windows;
    }
}

volatile uint32_t s_obs_us_sum, s_obs_us_max, s_obs_windows;
