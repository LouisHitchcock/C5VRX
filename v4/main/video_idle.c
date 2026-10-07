#define SFW_MIN_BUDGET 64u
/* C5VRX-4: idle responsibilities. */
#include "video_internal.h"

#define SFW_TARGET_US 50u /* per 200 us wake: at most 25 % CPU (was 30 per 100 us) */

#define IDLE_STD_SYNC_WINDOWS 20u /* valid syncs before a standard is stored */

#define IDLE_RETRY_TICKS     200u /* 10 s after the raster could not take TX */

/* No-carrier idle raster: the standalone raster owns TX (s_menu_active) with
 * a black picture instead of the menu; see idle_raster.h. */
idle_raster_t s_idle;

static unsigned s_idle_failures;

static const char *s_idle_last = "none";

/* Sync flywheel (SYNC_FLYWHEEL.md, operator decision 2026-10-05). 100 us
 * cadence from the V5 timer. RX writes the ring and the TX BitScrambler reads
 * it ~16 KiB (~409 us) later: the flywheel works on data older than the newest
 * completed RX descriptor and writes only beyond the TX descriptor in flight
 * (a ~180 us window per line, hence the 100 us wake).
 * Absolute byte positions use the timer as wrap disambiguator (40 bytes/us).
 * Internal SRAM is not cached on the C5, so no cache maintenance. The work
 * per wake is budgeted from the learned cost per evaluation. */

static sync_flywheel_t s_sfw;

static volatile bool s_sfw_running;

volatile uint32_t s_sfw_last_us, s_sfw_max_us, s_sfw_rebases;

static volatile uint32_t s_sfw_budget = 600u, s_sfw_ns_per_eval = 150u;

/* One control tick (50 ms), analog_agc_task only: it also owns the menu. */
void idle_raster_service(int q_phase, bool fresh_sync, unsigned sync_age_ticks)
{
    static unsigned std_windows, retry_ticks;
    static int std_seen = -1;
    /* Stable live standard: the same detection over 20 fresh sync windows. */
    if (fresh_sync && !s_menu_active && s_detected_video_std_valid) {
        int std = s_detected_video_std == VIDEO_STD_PAL;
        if (std != std_seen) { std_seen = std; std_windows = 0; }
        if (++std_windows == IDLE_STD_SYNC_WINDOWS) c5vrx4_last_standard_store((uint8_t)std);
    }
    if (retry_ticks) --retry_ticks;
    const bool native = rf_native_agc_active();
    const arc_gain_table_t *table = rf_get_arc_gain_table();
    const bool v5_max = s_rx_profile == RX_PROFILE_DIRECT_GAIN &&
        s_agc_mode == ANALOG_AGC_ACTIVE && table &&
        s_direct_gain_v3.current_gain == table->max_index;
    const bool settling = !native && s_direct_gain_v3.state == DG3_SETTLE;
    const bool bw_waiting = bw_autocal_waiting();
    /* During and right after a calibration the raster neither enters nor
     * leaves on what the calibration itself did to the IQ. */
    const bool cal_settle = esp_timer_get_time() < s_cal_settle_until_us || phy_rx_lab_busy();
    if (cal_settle && s_idle.active) { q_phase = 0; fresh_sync = false; }
    idle_raster_obs_t o = {
        .enabled = c5vrx4_idle_raster_enabled() && MENU_RUNTIME_ENABLED,
        .owner_free = !s_menu_active && !s_rssi_probe_active &&
                      !s_pre_q4_probe_active && !phy_rx_lab_busy() &&
                      !s_channel_scan_active && !bw_waiting && retry_ticks == 0u,
        /* No-carrier survival state: V5 table maximum, or native AGC. */
        .survival_gain = native || v5_max,
        .settling = settling || cal_settle,
        .q_phase = q_phase,
        .fresh_sync = fresh_sync,
        .sync_age_ticks = sync_age_ticks,
    };
    switch (idle_raster_step(&s_idle, &o)) {
    case IDLE_RASTER_ENTER:
        video_set_menu_mode(true);
        if (!s_menu_active) {
            idle_raster_abandon(&s_idle);
            ++s_idle_failures;
            retry_ticks = IDLE_RETRY_TICKS;
            s_idle_last = "refused_menu_raster_unavailable";
            printf("IDLE_RASTER refused=menu_raster_unavailable retry_s=10\n");
            break;
        }
        s_idle_last = "entered_no_carrier";
        printf("IDLE_RASTER enter std=%s q=%d gain=%u native=%u\n",
               s_video_std == VIDEO_STD_PAL ? "PAL" : "NTSC", q_phase, s_current_gain, native);
        break;
    case IDLE_RASTER_EXIT:
        video_set_menu_mode(false);
        s_idle_last = fresh_sync ? "exit_sync" : "exit_carrier";
        printf("IDLE_RASTER exit reason=%s q=%d gain=%u\n",
               fresh_sync ? "sync" : "carrier", q_phase, s_current_gain);
        break;
    default:
        break;
    }
}

void sync_flywheel_task(void *arg)
{
    (void)arg;
    uint64_t rx_abs = 0;
    uint32_t last_rx_off = UINT32_MAX;
    int64_t last_us = 0;
    sfw_init(&s_sfw);
    s_sfw.self_gate = true;
    for (;;) {
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        /* Labs measure raw receiver noise and IQ: no synthetic pulses then. */
        bool active = c5vrx4_sync_flywheel_enabled() && !s_menu_active && !IDLE_RASTER_ACTIVE() &&
                      !phy_rx_lab_busy() && !s_rssi_probe_active && !s_pre_q4_probe_active &&
                      s_rx_dma_ch >= 0 && s_rx_dma_ch < 3 && s_tx_dma_ch >= 0 && s_tx_dma_ch < 3 &&
                      s_rx_dscr_count >= 2 && s_tx_dscr_count >= 2;
        s_sfw_running = active;
        if (!active) { last_rx_off = UINT32_MAX; continue; }
        int ri = find_dscr_index(s_rx_dscr_nodes, s_rx_dscr_count,
                                 AHB_DMA.channel[s_rx_dma_ch].in.in_dscr_bf0.val);
        int ti = find_dscr_index(s_tx_dscr_nodes, s_tx_dscr_count,
                                 AHB_DMA.channel[s_tx_dma_ch].out.out_dscr_bf0.val);
        if (ri < 0 || ti < 0) continue;
        uint8_t *rx_buf = s_rx_dscr_nodes[ri].buffer, *tx_buf = s_tx_dscr_nodes[ti].buffer;
        if (rx_buf < s_raw_ring || rx_buf >= s_raw_ring + RAW_RING_BYTES ||
            tx_buf < s_raw_ring || tx_buf >= s_raw_ring + RAW_RING_BYTES) continue;
        uint32_t rx_off = (uint32_t)(rx_buf - s_raw_ring), tx_off = (uint32_t)(tx_buf - s_raw_ring);
        int64_t now = esp_timer_get_time();
        if (last_rx_off == UINT32_MAX) {
            sfw_init(&s_sfw);
            s_sfw.self_gate = true;
            rx_abs = (uint64_t)RAW_RING_BYTES * 4u + rx_off;
            ++s_sfw_rebases;
        } else {
            uint64_t d = (rx_off - last_rx_off) & (RAW_RING_BYTES - 1u);
            uint64_t expected = (uint64_t)(now - last_us) * 40u;
            while (d + RAW_RING_BYTES / 2u < expected) d += RAW_RING_BYTES;
            rx_abs += d;
        }
        last_rx_off = rx_off;
        last_us = now;
        uint64_t lag = (rx_off - tx_off) & (RAW_RING_BYTES - 1u);
        uint64_t floor = rx_abs - lag + s_tx_dscr_nodes[ti].length + 512u;
        /* Never touch the newest completed descriptor: every control
         * observer (V5, idle raster, level servo, AFC) copies exactly that
         * one, so they always see the unmodified reception. */
        int ni = (ri - 1 + s_rx_dscr_count) % s_rx_dscr_count;
        uint64_t newest = (rx_off - (uint32_t)(s_rx_dscr_nodes[ni].buffer - s_raw_ring)) &
                          (RAW_RING_BYTES - 1u);
        uint64_t ceiling = rx_abs - (newest ? newest : s_rx_dscr_nodes[ni].length);
        bool mask = c5vrx4_agc_mask_active();
        uint8_t flag = c5vrx4_agc_flag();
        /* Line repair source bound: RX overwrites the ring one ring behind
         * its write position; it may finish this descriptor and fill the
         * next one while the flywheel runs. */
        uint64_t reach = rx_abs + 2u * s_rx_dscr_nodes[ri].length + 512u;
        const sfw_ring_t ring = {
            s_raw_ring, RAW_RING_BYTES, c5v4_cvbs_phase_table(mask), mask,
            (uint8_t)((flag != C5VRX4_AGC_FLAG_UNKNOWN && (flag & 0x80u)) ? 1u : 0u),
            c5vrx4_line_repair_enabled() && reach > RAW_RING_BYTES ? reach - RAW_RING_BYTES : 0u,
        };
        int64_t t0 = esp_timer_get_time();
        /* Writes only inside its own detected fade window (self_gate). */
        (void)sfw_run(&s_sfw, &ring, ceiling, floor, true, s_sfw_budget);
        uint32_t spent = (uint32_t)(esp_timer_get_time() - t0);
        s_sfw_last_us = spent;
        if (spent > s_sfw_max_us) s_sfw_max_us = spent;
        /* Wall time includes preemption (Wi-Fi, esp_timer): a run that
         * took more than twice its target was preempted and says nothing
         * about the cost (board 2026-10-06: such runs drove the estimate to
         * 357 ns/eval, the budget to ~140 and acquisition never locked). */
        /* Every run counts again, but one sample can at most quadruple the
         * estimate: board 2026-10-07, acquiring on receiver noise at
         * priority 4, the runs were genuinely long, the "preempted" filter
         * dropped them, the budget stayed high and IDLE starved (task WDT in
         * gain_v3_obs). At priority 4 real preemption is rare. */
        if (s_sfw.evals >= 64u) {
            uint32_t ns = spent * 1000u / s_sfw.evals;
            if (ns > 4u * s_sfw_ns_per_eval) ns = 4u * s_sfw_ns_per_eval;
            s_sfw_ns_per_eval = (7u * s_sfw_ns_per_eval + ns) / 8u;
            if (!s_sfw_ns_per_eval) s_sfw_ns_per_eval = 1u;
        }
        uint32_t budget = SFW_TARGET_US * 1000u / s_sfw_ns_per_eval;
        s_sfw_budget = budget < SFW_MIN_BUDGET ? SFW_MIN_BUDGET : budget > 20000u ? 20000u : budget;
    }
}

void sync_flywheel_status_print(void)
{
    const sync_flywheel_t *f = &s_sfw;
    int std = sfw_standard(f);
    printf("SYNC_FW enabled=%u running=%u locked=%u std=%s state=%u lines=%lu clean=%lu "
           "repaired=%lu slots=%lu rebuilt=%lu missed=%lu vsyncs=%lu v_coasted=%lu parity=%lu "
           "relocks=%lu acq=%lu skipped=%lu floor_skips=%lu fast=%lu thr=%d sync_q4=%d blank_q4=%d "
           "period_q8=%ld noisy=%u last_us=%lu max_us=%lu budget=%lu ns_per_eval=%lu rebases=%lu "
           "line_repair=%u concealed=%lu conceal_no_source=%lu conceal_late=%lu "
           "stable=%u fade_pm=%u fade_detections=%lu jumps=%lu sampled=%lu "
           "hardware_acceptance=pending\n",
           c5vrx4_sync_flywheel_enabled(), s_sfw_running, sfw_locked(f),
           std == 1 ? "PAL" : std == 2 ? "NTSC" : "none", (unsigned)f->state,
           (unsigned long)f->lines, (unsigned long)f->clean, (unsigned long)f->repaired,
           (unsigned long)f->slots_repaired, (unsigned long)f->rebuilt, (unsigned long)f->missed,
           (unsigned long)f->vsyncs, (unsigned long)f->v_coasted, (unsigned long)f->v_parity,
           (unsigned long)f->relocks, (unsigned long)f->acquisitions, (unsigned long)f->skipped_lines,
           (unsigned long)f->skipped_floor, (unsigned long)f->fast_lines, f->thr, f->sync_q4,
           f->blank_q4, (long)f->period_q8, f->rebuild_lines ? 1u : 0u,
           (unsigned long)s_sfw_last_us, (unsigned long)s_sfw_max_us, (unsigned long)s_sfw_budget,
           (unsigned long)s_sfw_ns_per_eval, (unsigned long)s_sfw_rebases,
           c5vrx4_line_repair_enabled(), (unsigned long)f->concealed,
           (unsigned long)f->conceal_no_source, (unsigned long)f->conceal_late,
           f->stable, (unsigned)f->fade_pm, (unsigned long)f->fade_detections,
           (unsigned long)f->jumps, (unsigned long)f->sampled);
}

void idle_raster_status_print(void)
{
    uint8_t last = c5vrx4_last_standard();
    printf("IDLE_RASTER enabled=%u active=%u entries=%lu exits=%lu failures=%u last=%s "
           "std=%s last_live_std=%s enter_s=2 quiet_q<%d carrier_q>=%d hardware_acceptance=pending\n",
           c5vrx4_idle_raster_enabled(), s_idle.active, (unsigned long)s_idle.entries,
           (unsigned long)s_idle.exits, s_idle_failures, s_idle_last,
           video_standard_name(resolved_menu_standard()),
           last == C5VRX4_STD_UNKNOWN ? "unknown" : last ? "PAL" : "NTSC",
           IDLE_RASTER_QUIET_Q, IDLE_RASTER_CARRIER_Q);
}
