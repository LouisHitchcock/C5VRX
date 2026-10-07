/* C5VRX-4: diagnostics responsibilities. */
#include "video_internal.h"

static void record_transport_event(uint32_t flags);
static void lab_clear_transport_sticky(void);
static uint32_t lab_delta(uint32_t current, uint32_t base);
#define P8ENV_CAPTURE_ATTEMPTS 96u

#define P8ENV_CAPTURE_PROBES   32u

#define GDMA_OUT_FAULT_MASK 0x7cu /* DSCR_ERR, TOTAL_EOF, FIFO OVF/UDF, AHB response */

#define GDMA_IN_FAULT_MASK  0xfcu /* ERR_EOF, DSCR_ERR/EMPTY, FIFO OVF/UDF, AHB response */

#include "phase8_gain_lut.h"

static void record_transport_event(uint32_t flags)
{
    if (!flags) return;
    int64_t now = esp_timer_get_time();
    uint32_t seq = s_lag_event_head + 1u;
    unsigned index = (seq - 1u) % LAG_EVENT_LOG_SIZE;
    uint32_t rx_off = get_rx_dma_offset(NULL);
    uint32_t tx_off = get_tx_dma_offset(NULL);

    s_lag_events[index] = (lag_event_t) {
        .time_us = now,
        .seq = seq,
        .flags = flags,
        .rx_off = (uint16_t)rx_off,
        .tx_off = (uint16_t)tx_off,
        .gain = s_current_gain,
        .agc_state = (uint8_t)s_agc_state,
    };
    s_lag_event_head = seq;
    s_last_transport_event_us = now;
    s_last_transport_flags = flags;
    ++s_hw_counters.lag_event_count;

    if (s_last_gain_write_us > 0) {
        int64_t dt = now - s_last_gain_write_us;
        if (dt >= 0 && dt <= 200000) ++s_hw_counters.near_gain_event_count;
    }
    if (s_last_phy_write_us > 0) {
        int64_t dt = now - s_last_phy_write_us;
        if (dt >= 0 && dt <= 200000) ++s_hw_counters.near_phy_event_count;
    }
}

void poll_transport_faults(void)
{
    uint32_t flags = 0;
    bool tx_rempty = PARL_IO.int_raw.tx_fifo_rempty_int_raw != 0;

    if (tx_rempty) {
        ++s_hw_counters.parl_tx_rempty_count;
        PARL_IO.int_clr.tx_fifo_rempty_int_clr = 1;
        flags |= LAG_EVT_PARLIO_TX_REMPTY;
    }
    if (PARL_IO.int_raw.rx_fifo_wovf_int_raw) {
        ++s_hw_counters.parl_rx_wovf_count;
        PARL_IO.int_clr.rx_fifo_wovf_int_clr = 1;
        flags |= LAG_EVT_PARLIO_RX_WOVF;
    }
    if (PARL_IO.int_raw.tx_eof_int_raw) {
        ++s_hw_counters.parl_tx_eof_count;
        PARL_IO.int_clr.tx_eof_int_clr = 1;
        flags |= LAG_EVT_PARLIO_TX_EOF;
    }

    if (s_rx_dma_ch >= 0 && s_rx_dma_ch < 3) {
        uint32_t raw = AHB_DMA.in_intr[s_rx_dma_ch].raw.val & GDMA_IN_FAULT_MASK;
        if (raw) {
            ++s_hw_counters.gdma_in_fault_count;
            AHB_DMA.in_intr[s_rx_dma_ch].clr.val = raw;
            flags |= LAG_EVT_GDMA_IN_FAULT;
        }
    }
    if (s_tx_dma_ch >= 0 && s_tx_dma_ch < 3) {
        uint32_t raw = AHB_DMA.out_intr[s_tx_dma_ch].raw.val & GDMA_OUT_FAULT_MASK;
        if (raw) {
            ++s_hw_counters.gdma_out_fault_count;
            AHB_DMA.out_intr[s_tx_dma_ch].clr.val = raw;
            flags |= LAG_EVT_GDMA_OUT_FAULT;
        }
    }

    if (tx_rempty && BITSCRAMBLER.state[BITSCRAMBLER_DIR_TX].fifo_empty) {
        ++s_hw_counters.bs_fifo_empty_count;
    }
    if (BITSCRAMBLER.state[BITSCRAMBLER_DIR_TX].eof_overload) {
        ++s_hw_counters.bs_eof_overload_count;
        BITSCRAMBLER.state[BITSCRAMBLER_DIR_TX].val = 1u << 31;
        flags |= LAG_EVT_BS_EOF_OVERLOAD;
    }

    ++s_hw_counters.checks;
    record_transport_event(flags);
    if (flags && !s_menu_active) c5v4_level_hw_transport_fault();
}

hw_transport_counters_t lab_counter_snapshot(void)
{
    hw_transport_counters_t snapshot;
    memcpy(&snapshot, (const void *)&s_hw_counters, sizeof(snapshot));
    return snapshot;
}

static void lab_clear_transport_sticky(void)
{
    PARL_IO.int_clr.val = UINT32_MAX;
    if (s_rx_dma_ch >= 0) AHB_DMA.in_intr[s_rx_dma_ch].clr.val = UINT32_MAX;
    if (s_tx_dma_ch >= 0) AHB_DMA.out_intr[s_tx_dma_ch].clr.val = UINT32_MAX;
    BITSCRAMBLER.state[BITSCRAMBLER_DIR_TX].val = 1u << 31;
}

void lab_reset_correlation(void)
{
    memset((void *)&s_hw_counters, 0, sizeof(s_hw_counters));
    memset(s_lag_events, 0, sizeof(s_lag_events));
    s_lag_event_head = 0;
    s_last_transport_event_us = 0;
    s_last_transport_flags = 0;
    s_last_user_lag_mark_us = 0;
    s_last_gain_write_us = 0;
    s_last_phy_write_us = 0;
    s_last_phy_write_kind = PHY_WRITE_NONE;
    s_last_gain_drop_transition = s_gain_transition_count;
    lab_clear_transport_sticky();
}

static uint32_t lab_delta(uint32_t current, uint32_t base)
{
    return current - base;
}

void lab_print_row(const char *kind, const hw_transport_counters_t *base)
{
    const hw_transport_counters_t current = lab_counter_snapshot();
    const hw_transport_counters_t zero = {0};
    if (!base) base = &zero;

    int64_t now = esp_timer_get_time();
    long long gain_age_ms = s_last_gain_write_us > 0 ?
        (long long)((now - s_last_gain_write_us) / 1000) : -1;
    long long transport_age_ms = s_last_transport_event_us > 0 ?
        (long long)((now - s_last_transport_event_us) / 1000) : -1;
    long long phy_age_ms = s_last_phy_write_us > 0 ?
        (long long)((now - s_last_phy_write_us) / 1000) : -1;

    rf_phy_snapshot_t phy = {0};
    rf_get_phy_snapshot(&phy);

    printf("C5VRX_LAB_ROW kind=%s gain=%u gain_reg=0x%08lx bw=%u afc=%u "
           "offset_khz=%d agc=%u state=%u p=%d q=%d clip_pm=%d origin_pm=%d "
           "sync_q=%d gain_age_ms=%lld phy_age_ms=%lld transport_age_ms=%lld "
           "tx_empty=%lu rx_ovf=%lu gdma_in=%lu gdma_out=%lu\n",
           kind, s_current_gain, (unsigned long)phy.gain_reg,
           s_current_bw40 ? 40u : 20u, (unsigned)s_afc_mode,
           rf_get_frequency_offset_khz(), (unsigned)s_agc_mode, (unsigned)s_agc_state,
           s_last_p_median, s_last_q_phase, s_last_clip_permille, s_last_origin_permille,
           s_last_sync_quality, gain_age_ms, phy_age_ms, transport_age_ms,
           (unsigned long)lab_delta(current.parl_tx_rempty_count, base->parl_tx_rempty_count),
           (unsigned long)lab_delta(current.parl_rx_wovf_count, base->parl_rx_wovf_count),
           (unsigned long)lab_delta(current.gdma_in_fault_count, base->gdma_in_fault_count),
           (unsigned long)lab_delta(current.gdma_out_fault_count, base->gdma_out_fault_count));
    printf("DG3_OBS p50=%d p90=%d p95=%d origin_pm=%d clip_pm=%d coherence=%d "
           "state=%u gain=%u lane=%u lane_cap=%u noise_r2_q4=%u "
           "dc_i_mstep=%d dc_q_mstep=%d bw40=%u bw_switches=%lu "
           "lane_changes=%lu fold_drops=%lu "
           "virtual_q8=%ld writes=%lu holds=%lu verified=%lu learned=%lu "
           "settle_fine_us=%u settle_bb_us=%u settle_rf_us=%u obs_windows=%lu obs_us_avg=%lu obs_us_max=%lu "
           "sfw_us_last=%lu sfw_us_max=%lu\n",
           s_v3_p50, s_v3_p90, s_v3_p95, s_v3_origin_pm, s_v3_clip_pm,
           s_v3_coherence,
           (unsigned)s_direct_gain_v3.state, s_current_gain,
           (unsigned)rf_get_iq_lanes(),
           (unsigned)s_direct_gain_v3.lane_cap,
           (unsigned)s_direct_gain_v3.noise_p50_q4,
           s_v3_dc_i_mstep, s_v3_dc_q_mstep,
           s_current_bw40 ? 1u : 0u, (unsigned long)s_v3_bw_switches,
           (unsigned long)s_direct_gain_v3.lane_changes,
           (unsigned long)s_direct_gain_v3.fold_drops,
           (long)s_direct_gain_v3.virtual_gain_q8,
           (unsigned long)s_direct_gain_v3.writes,
           (unsigned long)s_direct_gain_v3.holds,
           (unsigned long)s_direct_gain_v3.verified,
           (unsigned long)s_direct_gain_v3.learned,
           s_direct_gain_v3.settle_us[DG3_FINE],
           s_direct_gain_v3.settle_us[DG3_BB],
           s_direct_gain_v3.settle_us[DG3_RF],
           (unsigned long)s_obs_windows,
           (unsigned long)(s_obs_windows ? s_obs_us_sum / s_obs_windows : 0u),
           (unsigned long)s_obs_us_max,
           (unsigned long)s_sfw_last_us, (unsigned long)s_sfw_max_us);
    s_obs_us_sum = 0; s_obs_us_max = 0; s_obs_windows = 0;
}

/* Issue #119 origin-collapse oracle. On demand only ('E'): copy up to 32
 * completed-descriptor probes (128 x 64 adjacent 25 ns samples) from the
 * console task and print one P8ENV row. No PHY write, no gain decision and no
 * participation in RX/TX pacing. A host sweep script sends 'E' per step. */

void p8env_capture_report(void)
{
    static p8env_accum_t acc;
    static uint8_t sample[RX_PROBE_REGIONS * RX_PROBE_REGION_BYTES];
    rf_native_agc_state_t native_before, native_after;
    rf_get_native_agc_state(&native_before);
    const uint32_t gain_epoch = s_gain_transition_count;
    unsigned probes = 0, stale = 0;
    uint32_t gain_reg_changes = 0, last_gain_reg = native_before.gain_status_reg;

    p8env_reset(&acc);
    for (unsigned attempt = 0; attempt < P8ENV_CAPTURE_ATTEMPTS &&
                               probes < P8ENV_CAPTURE_PROBES; ++attempt) {
        if (rx_probe_copy_completed(sample)) {
            for (unsigned r = 0; r < RX_PROBE_REGIONS; ++r) {
                p8env_break(&acc);
                p8env_add(&acc, sample + r * RX_PROBE_REGION_BYTES,
                          RX_PROBE_REGION_BYTES, c5vrx_phase8_gain_lut);
            }
            ++probes;
        } else {
            ++stale;
        }
        uint32_t gain_reg = rf_get_rx_gain_reg();
        if (gain_reg != last_gain_reg) {
            ++gain_reg_changes;
            last_gain_reg = gain_reg;
        }
        vTaskDelay(1);
    }
    rf_get_native_agc_state(&native_after);
    const p8env_summary_t e = p8env_summarize(&acc, &P8ENV_PROVISIONAL);
    const hw_transport_counters_t t = lab_counter_snapshot();

    printf("P8ENV t_ms=%lld native=%u blocked=%lu gain_reg=0x%08lx agc_reg=0x%08lx "
           "gain_reg_changes=%lu fw_gain_epochs=%lu gain=%u agc=%u profile=%u demod=%u "
           "probes=%u stale=%u n=%lu pairs=%lu p50=%u p90=%u p95=%u "
           "central_pm=%u origin_pm=%u clip_pm=%u coh_pm=%u hard_pm=%u "
           "hard_central_pm=%u hard_outer_pm=%u central_hard_share_pm=%u class=%s "
           "radius_pm=%u/%u/%u/%u/%u/%u/%u/%u/%u/%u/%u "
           "delta_pm=%u/%u/%u/%u/%u/%u/%u/%u/%u "
           "sync_q=%d std_valid=%u rx_ovf=%lu tx_empty=%lu gdma_in=%lu gdma_out=%lu "
           "bs_empty=%lu bs_eof=%lu\n",
           (long long)(esp_timer_get_time() / 1000),
           native_after.active ? 1u : 0u,
           (unsigned long)native_after.blocked_writes,
           (unsigned long)native_after.gain_status_reg,
           (unsigned long)native_after.agc_ctrl_reg,
           (unsigned long)gain_reg_changes,
           (unsigned long)(s_gain_transition_count - gain_epoch),
           s_current_gain, (unsigned)s_agc_mode, (unsigned)s_rx_profile,
           (unsigned)s_demod_mode, probes, stale,
           (unsigned long)acc.samples, (unsigned long)acc.pairs,
           e.p50, e.p90, e.p95, e.central_pm, e.origin_pm, e.clip_pm,
           e.coherence_pm, e.hard_pm, e.hard_given_central_pm,
           e.hard_given_outer_pm, e.central_hard_share_pm,
           p8env_class_name(e.cls),
           e.radius_pm[0], e.radius_pm[1], e.radius_pm[2], e.radius_pm[3],
           e.radius_pm[4], e.radius_pm[5], e.radius_pm[6], e.radius_pm[7],
           e.radius_pm[8], e.radius_pm[9], e.radius_pm[10],
           e.delta_pm[0], e.delta_pm[1], e.delta_pm[2], e.delta_pm[3],
           e.delta_pm[4], e.delta_pm[5], e.delta_pm[6], e.delta_pm[7],
           e.delta_pm[8],
           s_last_sync_quality, s_detected_video_std_valid ? 1u : 0u,
           (unsigned long)t.parl_rx_wovf_count,
           (unsigned long)t.parl_tx_rempty_count,
           (unsigned long)t.gdma_in_fault_count,
           (unsigned long)t.gdma_out_fault_count,
           (unsigned long)t.bs_fifo_empty_count,
           (unsigned long)t.bs_eof_overload_count);
}

/* Read-only raw Q4/I4 dump ('Q'): one completed-descriptor probe, i.e. four
 * separated runs of 64 consecutive 25 ns samples, printed as hex. */
void lab_dump_raw_probe(void)
{
    static uint8_t sample[RX_PROBE_REGIONS * RX_PROBE_REGION_BYTES];
    for (unsigned attempt = 0; attempt < 16u; ++attempt) {
        if (!rx_probe_copy_completed(sample)) {
            vTaskDelay(1);
            continue;
        }
        for (unsigned r = 0; r < RX_PROBE_REGIONS; ++r) {
            printf("Q4RAW r=%u ", r);
            for (unsigned n = 0; n < RX_PROBE_REGION_BYTES; ++n)
                printf("%02x", sample[r * RX_PROBE_REGION_BYTES + n]);
            printf("\n");
        }
        return;
    }
    printf("Q4RAW unavailable\n");
}

/* Print the vendor-generated receive model without changing any PHY state. */
void lab_print_arc_oracle(void)
{
    const arc_gain_table_t *table = rf_get_arc_gain_table();
    const rf_phy_snapshot_t *tuple = rf_get_arc_receive_tuple();
    printf("C5VRX_ARC_ORACLE generation=%" PRIu32 " max=%u source=%s survival=%u spans=",
           rf_get_arc_generation(),
           table->max_index,
           table->runtime_spans_valid ? "vendor" : "fallback",
           rf_get_arc_survival_gain());
    for (unsigned i = 0; i < ARC_RX_STAGE_COUNT; ++i)
        printf("%s%u", i ? "," : "", table->spans[i]);
    printf("\n");
    printf("C5VRX_ARC_CAPTURE gain=%u rf_stage=%u rf_code=%u bb=%u fine=%u "
           "filter=%u adc=%u iq=%u/%d/%d\n",
           tuple->gain_tuple.gain_index, tuple->gain_tuple.rf_stage,
           tuple->gain_tuple.rf_code, tuple->gain_tuple.bb_code,
           tuple->gain_tuple.fine_code, tuple->rx_filter_mode,
           tuple->adc_rate_sel, tuple->iq_correction.enable,
           tuple->iq_correction.coef0, tuple->iq_correction.coef1);
    lab_print_row("ARC_ORACLE", NULL);
}

#define GAIN_FORCE_REG 0x600A702Cu
static uint32_t s_gain_readback_checks, s_gain_readback_mismatch, s_gain_rf_word_mismatch;
static uint32_t s_gain_readback_last;

void predemod_gain_readback_service(void)
{
    if (rf_native_agc_active() || s_rx_profile != RX_PROFILE_DIRECT_GAIN ||
        s_agc_mode != ANALOG_AGC_ACTIVE || phy_rx_lab_busy() ||
        s_direct_gain_v3.state == DG3_SETTLE) return;
    uint32_t reg = REG_READ(GAIN_FORCE_REG);
    s_gain_readback_last = reg;
    ++s_gain_readback_checks;
    bool forced = (reg >> 23) & 1u;
    uint8_t index = (uint8_t)(reg >> 24);
    if (!forced || index != s_current_gain) {
        if (++s_gain_readback_mismatch <= 5u)
            printf("GAIN_READBACK mismatch reg=0x%08lx forced=%u hw_index=%u v5_index=%u\n",
                   (unsigned long)reg, forced, index, s_current_gain);
    }
    /* The PBUS RF control word must carry the vendor tuple's RF code (not
     * held: in debug mode it is our re-asserted copy). */
    uint16_t w[3];
    arc_gain_tuple_t t;
    const arc_gain_table_t *table = rf_get_arc_gain_table();
    if (!phy_rx_lab_dco_held() && table && table->band5 && phy_rx_lab_gain_words(w) &&
        arc_gain_tuple_decode(table, s_current_gain, &t) && w[0] != t.rf_code) {
        if (++s_gain_rf_word_mismatch <= 5u)
            printf("GAIN_READBACK rf_word=%u expected_rf=%u gain=%u\n", w[0], t.rf_code, s_current_gain);
    }
}

void gain_readback_print(void)
{
    uint32_t reg = REG_READ(GAIN_FORCE_REG);
    arc_gain_tuple_t t = {0};
    const arc_gain_table_t *table = rf_get_arc_gain_table();
    bool ok = table && arc_gain_tuple_decode(table, (uint8_t)(reg >> 24), &t);
    printf("GAIN_READBACK reg=0x%08lx forced=%lu hw_index=%lu v5_index=%u tuple_rf=%u tuple_bb=%u "
           "tuple_fine=%u packed=0x%05lx band5=%u checks=%lu mismatches=%lu\n",
           (unsigned long)reg, (unsigned long)((reg >> 23) & 1u), (unsigned long)(reg >> 24),
           s_current_gain, ok ? t.rf_code : 0u, ok ? t.bb_code : 0u, ok ? t.fine_code : 0u,
           (unsigned long)(ok ? t.packed_state : 0u), table ? table->band5 : 0u,
           (unsigned long)s_gain_readback_checks, (unsigned long)s_gain_readback_mismatch);
    uint16_t w[3] = {0, 0, 0}, tr[3][3];
    uint32_t events = 0;
    bool words = phy_rx_lab_gain_words(w);
    phy_rx_lab_gain_trace(tr, &events);
    printf("GAIN_PBUS words=%u rf=%u bb=%u fine=%u rf_mismatches=%lu hold_trace_events=%lu "
           "enter=%u/%u/%u debug=%u/%u/%u release=%u/%u/%u dco_held=%u\n",
           words, w[0], w[1], w[2], (unsigned long)s_gain_rf_word_mismatch, (unsigned long)events,
           tr[0][0], tr[0][1], tr[0][2], tr[1][0], tr[1][1], tr[1][2], tr[2][0], tr[2][1], tr[2][2],
           phy_rx_lab_dco_held());
    /* IQ path state, read-only (review 2026-10-07): phy_param[44] selects the
     * phy_rxiq_opt() averaging branch, phy_param[650] the phy_rxiq_scale_set()
     * selector (0x0000 / 0xFA00 / 0x00FA in 0x600A043C[15:0]); 0x600A0438
     * holds the IQ coefficients. Image rejection itself is a bench item. */
    printf("IQ_STATE rxiq_opt_flag=%u scale_sel=%u scale_reg=0x%08lx coef_reg=0x%08lx\n",
           phy_param[44], phy_param[650], (unsigned long)REG_READ(0x600A043Cu),
           (unsigned long)REG_READ(0x600A0438u));
    /* Measured on the live IQ: meaningful with a strong carrier (the FM
     * signal sweeps the circle). An ellipse shows as fine grain on a strong
     * picture. */
    {
        const size_t w = RX_PROBE_REGIONS * RX_PROBE_REGION_BYTES;
        unsigned got = 0;
        for (unsigned tries = 0; tries < 48u && got < 24u; ++tries) {
            vTaskDelay(1);
            if (rx_probe_copy_completed(s_dco_env_buf + got * w)) ++got;
        }
        predemod_iq_imbalance_t m = predemod_iq_imbalance(s_dco_env_buf, got * w);
        printf("IQ_IMBALANCE windows=%u gain_ratio=%d.%03d phase_deg=%d.%d irr_db=%d.%d gain=%u p50=%d coherence=%d\n",
               got, m.gain_x1000 / 1000, m.gain_x1000 % 1000, m.phase_x10 / 10, abs(m.phase_x10 % 10),
               m.irr_db_x10 / 10, m.irr_db_x10 % 10, s_current_gain, s_v3_p50, s_v3_coherence);
    }
}
