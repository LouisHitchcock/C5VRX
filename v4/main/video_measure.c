/* C5VRX-4: measure responsibilities. */
#include "video_internal.h"

static inline unsigned trajectory_v2_stage1_address(uint8_t previous_phase5,
                                                    uint8_t middle_raw,
                                                    uint8_t current_raw);
static void video_standard_vote(video_standard_t standard, uint16_t period);
static void cvbs_analyze_locked(const uint8_t *raw, size_t bytes, c5v4_cvbs_stats_t *stats);
static bool copy_level_snapshot(uint8_t *raw);
#include "trajectory_v2_lut.h"

/* Exact Phase5 state decode mirrored from the embedded fm.bsasm LUT.  The
 * detector is observation-only: the realtime BitScrambler remains the sole
 * live demodulator. */
static const uint8_t s_phase5_state_lut[256] = {
     4,  6,  7,  7,  7,  8,  8,  8, 24, 24, 24, 25, 25, 25, 26, 28,
     2,  4,  5,  6,  6,  7,  7,  7, 25, 25, 25, 26, 26, 27, 28, 30,
     1,  3,  4,  5,  5,  6,  6,  6, 26, 26, 26, 27, 27, 28, 29, 31,
     1,  2,  3,  4,  5,  5,  5,  6, 26, 27, 27, 27, 28, 29, 30, 31,
     1,  2,  3,  3,  4,  5,  5,  5, 27, 27, 27, 28, 29, 29, 30, 31,
     0,  1,  2,  3,  3,  4,  4,  5, 27, 28, 28, 28, 29, 30, 31,  0,
     0,  1,  2,  3,  3,  4,  4,  4, 28, 28, 28, 29, 29, 30, 31,  0,
     0,  1,  2,  2,  3,  3,  4,  4, 28, 28, 29, 29, 30, 30, 31,  0,
    16, 15, 14, 14, 13, 13, 12, 12, 20, 20, 19, 19, 18, 18, 17, 16,
    16, 15, 14, 13, 13, 12, 12, 12, 20, 20, 20, 19, 19, 18, 17, 16,
    16, 15, 14, 13, 13, 12, 12, 11, 21, 20, 20, 19, 19, 18, 17, 16,
    15, 14, 13, 13, 12, 12, 11, 11, 21, 21, 21, 20, 19, 19, 18, 17,
    15, 14, 13, 12, 11, 11, 11, 10, 22, 21, 21, 21, 20, 19, 18, 17,
    15, 13, 12, 11, 11, 10, 10, 10, 22, 22, 22, 21, 21, 20, 19, 17,
    14, 12, 11, 10, 10,  9,  9,  9, 23, 23, 23, 22, 22, 21, 20, 18,
    12, 10,  9,  9,  9,  8,  8,  8, 24, 24, 24, 23, 23, 23, 22, 20,
};

/* Bit i is 1 when the production fm.bsasm delta LUT maps that
 * (previous_phase5,current_phase5) pair to DAC code <= 8.  This compact mask
 * lets the control task recognize real H-sync tips without duplicating the
 * 1024-entry output LUT or using floating point. */

static uint32_t s_receive_generation;

static inline unsigned trajectory_v2_stage1_address(uint8_t previous_phase5,
                                                    uint8_t middle_raw,
                                                    uint8_t current_raw)
{
    /* Must mirror fm_traj.bsasm exactly:
     *   A0..A7 = current raw Q4/I4
     *   A8     = middle raw-I sign
     *   A9     = previous actual Phase5 MSB. */
    return (unsigned)current_raw |
           ((((unsigned)middle_raw >> 7u) & 1u) << 8u) |
           ((((unsigned)previous_phase5 >> 4u) & 1u) << 9u);
}

fusion_shadow_metrics_t active_demod_shadow(fusion_shadow_metrics_t shadow)
{
    return shadow;
}

void video_standard_detector_reset(void)
{
    ++s_receive_generation;
    s_video_std_pal_score = 0;
    s_video_std_ntsc_score = 0;
    s_detected_video_std_valid = false;
    s_last_line_period_20m = 0;
    s_last_sync_width_20m = 0;
    s_last_sync_quality = 0;
}

static void video_standard_vote(video_standard_t standard, uint16_t period)
{
    s_last_line_period_20m = period;
    if (standard == VIDEO_STD_PAL) {
        if (s_video_std_pal_score < 8u) ++s_video_std_pal_score;
        if (s_video_std_ntsc_score) --s_video_std_ntsc_score;
    } else {
        if (s_video_std_ntsc_score < 8u) ++s_video_std_ntsc_score;
        if (s_video_std_pal_score) --s_video_std_pal_score;
    }

    if (s_video_std_pal_score >= 3u &&
        s_video_std_pal_score >= s_video_std_ntsc_score + 2u) {
        s_detected_video_std = VIDEO_STD_PAL;
        s_detected_video_std_valid = true;
    } else if (s_video_std_ntsc_score >= 3u &&
               s_video_std_ntsc_score >= s_video_std_pal_score + 2u) {
        s_detected_video_std = VIDEO_STD_NTSC;
        s_detected_video_std_valid = true;
    }
}

static void cvbs_analyze_locked(const uint8_t *raw, size_t bytes, c5v4_cvbs_stats_t *stats)
{
    xSemaphoreTake(s_cvbs_analyze_lock, portMAX_DELAY);
    c5v4_cvbs_analyze(raw, bytes, c5vrx4_history_enabled(), c5vrx4_cvbs_mode(), stats);
    xSemaphoreGive(s_cvbs_analyze_lock);
}

int video_semantic_observe(const uint8_t *raw, size_t bytes, size_t ring_offset)
{
    (void)ring_offset;
    c5v4_cvbs_stats_t stats;
    cvbs_analyze_locked(raw, bytes, &stats);
    unsigned period = stats.period_raw;
    int quality = stats.levels_valid && stats.repeated ? 90 : 0;
    s_last_sync_width_20m = 0; /* No fabricated Phase5-width measurement. */
    if (quality) {
        if (period >= 2532u && period <= 2550u)
            video_standard_vote(VIDEO_STD_NTSC, (uint16_t)((period + 1u) / 2u));
        else if (period >= 2553u && period <= 2571u)
            video_standard_vote(VIDEO_STD_PAL, (uint16_t)((period + 1u) / 2u));
        else quality = 0;
    }
    s_last_sync_quality = quality;
    return quality;
}

control_metrics_t analyze_control_window(const uint8_t *sample, size_t bytes,
                                                size_t ring_offset)
{
    control_metrics_t m = {0};
    uint16_t hist[129] = {0};
    const size_t production_first = (ring_offset & 1u) ? 0u : 1u;
    int8_t prev_i = 0, prev_q = 0;
    uint8_t prev_phase = 0, prev2_phase = 0;
    uint8_t prev_byte = 0;
    int prev_power = 0, prev2_power = 0;
    fusion_shadow_t fusion_shadow;
    fusion_shadow_reset(&fusion_shadow);

    for (size_t i = 0; i < bytes; ++i) {
        uint8_t byte = sample[i];
        int8_t q = (int8_t)((byte & 0x0fu) << 4) >> 4;
        int8_t in_val = (int8_t)(byte & 0xf0u) >> 4;

        if (in_val == -8 || in_val == 7 || q == -8 || q == 7) ++m.n_clip;
        int i2 = (int)in_val * in_val;
        int q2 = (int)q * q;
        m.sum_i += in_val;
        m.sum_q += q;
        m.sum_i2 += i2;
        m.sum_q2 += q2;
        m.sum_iq += (int)in_val * q;

        int p = i2 + q2;
        const int raw_power = p;
        const uint8_t phase = s_phase5_state_lut[byte];
        fusion_shadow_push(&fusion_shadow, phase, raw_power);
        if (p <= 4) ++m.n_origin;
        if (p > 128) p = 128;
        ++hist[p];

        if (i > 0) {
            int dot = (int)in_val * (int)prev_i + (int)q * (int)prev_q;
            int cross = (int)q * (int)prev_i - (int)in_val * (int)prev_q;
            int abs_cross = cross < 0 ? -cross : cross;
            if (p >= 8 && dot > 0 && abs_cross <= dot) {
                ++m.n_coherent;
                m.sum_cross += cross;
                m.sum_dot += dot;
            }
        }

        /* fm.bsasm consumes one parity at 20 MS/s. Count exactly those
         * endpoint intervals, while using the skipped 40 MS/s middle sample
         * only as a shadow oracle. */
        bool production_endpoint =
            i >= production_first + 2u &&
            ((i - production_first) & 1u) == 0u;
        if (production_endpoint) {
            bool winding = demod_phase5_endpoint_loses_winding(prev2_phase,
                                                               prev_phase,
                                                               phase);
            ++m.winding_triplets;
            if (winding) ++m.winding_events;
            if (prev2_power >= DEMOD_STRONG_POWER_MIN &&
                prev_power >= DEMOD_STRONG_POWER_MIN &&
                raw_power >= DEMOD_STRONG_POWER_MIN) {
                ++m.strong_winding_triplets;
                if (winding) ++m.strong_winding_events;
            }

            unsigned traj_addr =
                trajectory_v2_stage1_address(prev2_phase, prev_byte, byte);
            m.trajectory_uncertainty_sum +=
                255u - c5vrx_trajectory_v2_confidence[traj_addr];
            ++m.trajectory_states;
        }

        prev2_phase = prev_phase;
        prev_phase = phase;
        prev2_power = prev_power;
        prev_power = raw_power;
        prev_byte = byte;
        prev_i = in_val;
        prev_q = q;
    }

    unsigned cumulative = 0;
    for (unsigned p = 0; p <= 128u; ++p) {
        cumulative += hist[p];
        if (cumulative >= (bytes + 1u) / 2u) {
            m.p_median = (int)p;
            break;
        }
    }
    m.q_phase = bytes > 1u ? (m.n_coherent * 100) / (int)(bytes - 1u) : 0;
    m.clip_permille = bytes ? (m.n_clip * 1000) / (int)bytes : 0;
    m.origin_permille = bytes ? (m.n_origin * 1000) / (int)bytes : 1000;
    m.winding_permille = m.winding_triplets ?
        (m.winding_events * 1000) / m.winding_triplets : 0;
    m.strong_winding_permille = m.strong_winding_triplets ?
        (m.strong_winding_events * 1000) / m.strong_winding_triplets : 0;
    m.dc_i_x100 = bytes ? (m.sum_i * 100) / (int)bytes : 0;
    m.dc_q_x100 = bytes ? (m.sum_q * 100) / (int)bytes : 0;

    /* These two dimensionless metrics are intentionally cheap. A perfect
     * centered/circular I/Q cloud tends toward zero skew and zero I/Q cross
     * correlation. They let us quantify DC/IQ calibration quality without
     * putting any new calibration routine in the realtime path. */
    int iq_power = m.sum_i2 + m.sum_q2;
    if (iq_power > 0) {
        int skew = m.sum_i2 - m.sum_q2;
        if (skew < 0) skew = -skew;
        int cross = m.sum_iq;
        if (cross < 0) cross = -cross;
        m.iq_skew_permille = (skew * 1000) / iq_power;
        m.iq_cross_permille = (cross * 2000) / iq_power;
    }
    m.fusion_shadow = fusion_shadow_finish(&fusion_shadow);
    if (m.trajectory_states) {
        m.fusion_shadow.trajectory_uncertainty_permille =
            (int)((m.trajectory_uncertainty_sum * 1000u) /
                  (m.trajectory_states * 255u));
    }
    return m;
}

/* A separate adaptive 5/20-ms supervisor leaves the 50-ms button/menu/AFC timers intact.
 * It copies 204.75 us of completed IQ, never the descriptor currently written.
 * This is control-plane gain/offset correction; live pixels stay in hardware. */
static bool copy_level_snapshot(uint8_t *raw)
{
    if (s_rx_dma_ch < 0 || s_rx_dma_ch >= 3 || s_rx_dscr_count < 4) return false;
    int64_t start = esp_timer_get_time();
    uint32_t before = AHB_DMA.channel[s_rx_dma_ch].in.in_dscr_bf0.val;
    int active = find_dscr_index(s_rx_dscr_nodes, s_rx_dscr_count, before);
    if (active < 0) return false;
    /* Verify this is the contiguous circular raw ring before using geometry. */
    size_t total = 0;
    for (int k = 0; k < s_rx_dscr_count; ++k) {
        if (!s_rx_dscr_nodes[k].length || total > sizeof(s_raw_ring) ||
            s_rx_dscr_nodes[k].buffer != s_raw_ring + total ||
            s_rx_dscr_nodes[k].length > sizeof(s_raw_ring)-total) return false;
        total += s_rx_dscr_nodes[k].length;
    }
    if (total != sizeof(s_raw_ring)) return false;
    size_t end = (size_t)(s_rx_dscr_nodes[active].buffer - s_raw_ring);
    c5v4_snapshot_plan_t plan;
    if (!c5v4_snapshot_plan(total, end, s_rx_dscr_nodes[active].length,
                            C5V4_LEVEL_SAMPLE_BYTES, &plan)) return false;
    sync_dma_m2c(s_raw_ring + plan.offset, plan.first);
    memcpy(raw, s_raw_ring + plan.offset, plan.first);
    size_t rest = C5V4_LEVEL_SAMPLE_BYTES - plan.first;
    if (rest) { sync_dma_m2c(s_raw_ring, rest); memcpy(raw+plan.first, s_raw_ring, rest); }
    uint32_t after = AHB_DMA.channel[s_rx_dma_ch].in.in_dscr_bf0.val;
    return c5v4_snapshot_current(before, after,
        (uint64_t)(esp_timer_get_time()-start), plan.safe_bytes, IQ_RATE_HZ);
}

void cvbs_level_task(void *arg)
{
    uint8_t *raw = arg;
    TickType_t wake = xTaskGetTickCount();
    int64_t last_capture_us = 0;
    bool have_capture = false;
    for (;;) {
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(5));
        /* Native AGC included: sync depth and black are measured in the phase
         * domain, which RF gain does not scale. Native's untagged acquisitions
         * only add outlier samples, which the plateau-MAD, ambiguity/origin
         * limits and three-window agreement already reject (HDZERO.md). */
        if (!c5vrx4_level_enabled() || !c5v4_level_hw_ready() || s_menu_active ||
            s_rssi_probe_active || s_pre_q4_probe_active ||
            phy_rx_lab_busy()) {
            c5v4_level_hw_invalidate(); continue;
        }
        rx_control_epoch_t epoch = {s_profile_generation, phy_rx_lab_generation(), s_gain_transition_count};
        unsigned lane = rf_get_iq_lanes();
        uint32_t context = epoch.profile ^ (epoch.phy * 2654435761u) ^
            (epoch.gain * 2246822519u) ^ (lane << 28);
        int64_t start = esp_timer_get_time();
        unsigned period = c5v4_level_hw_period(context, (uint64_t)start);
        /* Microseconds, half a fast period of tolerance: whole-tick counts
         * with a 5-ms wake turned one tick of jitter into a 10/25-ms cadence. */
        if (have_capture && start - last_capture_us <
            (int64_t)period - (int64_t)C5V4_LEVEL_FAST_US / 2) continue;
        rf_iq_lane_stats_t lane_stats;
        rf_get_iq_lane_stats(&lane_stats);
        bool settling = false;
        settling = s_rx_profile == RX_PROFILE_DIRECT_GAIN &&
            s_agc_mode == ANALOG_AGC_ACTIVE && s_direct_gain_v3.state == DG3_SETTLE;
        if (!c5v4_level_source_ready((uint64_t)start, s_last_gain_write_us,
                s_last_phy_write_us, lane_stats.last_switch_us, settling) ||
            !copy_level_snapshot(raw)) { c5v4_level_hw_invalidate(); continue; }
        last_capture_us = start; have_capture = true;
        c5v4_cvbs_stats_t stats;
        cvbs_analyze_locked(raw, C5V4_LEVEL_SAMPLE_BYTES, &stats);
        s_level_work_us = (unsigned)(esp_timer_get_time()-start);
        if (!phy_rx_lab_try_actuator(epoch.phy)) { c5v4_level_hw_invalidate(); continue; }
        bool fresh = !s_menu_active && !s_rssi_probe_active &&
            !s_pre_q4_probe_active && !phy_rx_lab_busy() &&
            lane == rf_get_iq_lanes() && esp_timer_get_time()-start < period &&
            rx_control_epoch_equal(epoch, (rx_control_epoch_t){s_profile_generation,
                phy_rx_lab_generation(), s_gain_transition_count});
        c5v4_level_hw_observe(&stats, fresh, context, (uint64_t)esp_timer_get_time());
        phy_rx_lab_end_actuator();
    }
}

void cvbs_capture_task(void *arg)
{
    (void)arg;
    printf("C5V4_AFC mode=%u video_track=%u fresh=%u corrections=%u porch_khz=%d auto_default=off\n",
           (unsigned)s_afc_mode, (unsigned)s_afc_video_locked,
           s_afc_fresh, s_afc_corrections, s_cfo_khz);
    uint8_t *raw = malloc(CONTROL_SAMPLE_BYTES);
    if (!raw) { printf("C5V4_CVBS refused=no_memory\n"); goto done; }
    for (unsigned capture = 0; capture < 8; ++capture) {
        if (s_menu_active || phy_rx_lab_busy() ||
            s_pre_q4_probe_active || s_rssi_probe_active) {
            printf("C5V4_CVBS refused=menu_or_phy_lab\n"); break;
        }
        if (s_rx_dma_ch < 0 || s_rx_dma_ch >= 3) break;
        unsigned capture_lane = rf_get_iq_lanes();
        rx_control_epoch_t before = {s_profile_generation, phy_rx_lab_generation(), s_gain_transition_count};
        int64_t begin = esp_timer_get_time();
        uint32_t active_addr = AHB_DMA.channel[s_rx_dma_ch].in.in_dscr_bf0.val;
        int active = find_dscr_index(s_rx_dscr_nodes, s_rx_dscr_count, active_addr);
        if (active < 0 || s_rx_dscr_count < 2) break;
        int completed = (active - 1 + s_rx_dscr_count) % s_rx_dscr_count;
        uint8_t *src = s_rx_dscr_nodes[completed].buffer;
        if (!src || src < s_raw_ring || src + CONTROL_SAMPLE_BYTES > s_raw_ring + sizeof(s_raw_ring) ||
            s_rx_dscr_nodes[completed].length < CONTROL_SAMPLE_BYTES) break;
        sync_dma_m2c(src, CONTROL_SAMPLE_BYTES);
        memcpy(raw, src, CONTROL_SAMPLE_BYTES);
        unsigned copy_us = (unsigned)(esp_timer_get_time() - begin);
        int after = find_dscr_index(s_rx_dscr_nodes, s_rx_dscr_count,
                            AHB_DMA.channel[s_rx_dma_ch].in.in_dscr_bf0.val);
        rx_control_epoch_t now = {s_profile_generation, phy_rx_lab_generation(), s_gain_transition_count};
        if (after != active || copy_us > 50 || !rx_control_epoch_equal(before, now) ||
            s_menu_active || phy_rx_lab_busy()) {
            printf("C5V4_CVBS discarded=context_or_dma copy_us=%u\n", copy_us);
        } else {
            c5v4_cvbs_stats_t stats;
            int64_t processing = esp_timer_get_time();
            cvbs_analyze_locked(raw, CONTROL_SAMPLE_BYTES, &stats);
            unsigned work_us = (unsigned)(esp_timer_get_time() - processing);
            printf("C5V4_CVBS snapshot=%u semantic_estimate=1 valid=%d mode=%s "
                   "period_raw=%u pulses=%u repeated=%u sync_bins=%d blank_bins=%d span_bins=%d "
                   "sync_mad=%d blank_mad=%d nominal_sync_mv=%d nominal_blank_mv=%d "
                   "nominal_depth_mv=%d proposal_q10=%u origin_pm=%u ambiguous_pm=%u "
                   "clip_pm=%u mean_i_mcell=%d mean_q_mcell=%d lane=%u copy_us=%u work_us=%u "
                   "actuator=none\n", capture, stats.levels_valid,
                   c5vrx4_cvbs_mode_name(),
                   stats.period_raw, stats.pulses, stats.repeated, stats.sync_bins,
                   stats.blank_bins, stats.span_bins, stats.sync_mad_bins, stats.blank_mad_bins,
                   stats.sync_mv, stats.blank_mv, stats.sync_depth_mv, stats.suggested_scale_q10,
                   stats.origin_pm, stats.ambiguous_pm, stats.clip_pm,
                   stats.mean_i_mcell, stats.mean_q_mcell, capture_lane, copy_us, work_us);
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    free(raw);
done:
    __sync_lock_release(&s_cvbs_capture_running);
    vTaskDelete(NULL);
}
