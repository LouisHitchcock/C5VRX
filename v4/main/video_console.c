/* C5VRX-4: console responsibilities. */
#include "video_internal.h"
#include "snr_meter_hw.h"

#define LAB_GAIN_STEP      2u        /* characterize the states production actually uses */

#define LAB_GAIN_MIN       2u        /* production controller lower bound */

void console_diag_task(void *arg)
{
    (void)arg;

    for (;;) {
        /* IDF 6.0's O_NONBLOCK VFS read consults the installed driver's
         * available-byte count even in no-driver mode. Poll the FIFO here;
         * this task is its sole reader. Bound each batch so paste cannot
         * monopolize the task. No host line-ending or DTR assumption. */
        for (unsigned received = 0; received < 64; ++received) {
            uint8_t byte;
            if (usb_serial_jtag_ll_read_rxfifo(&byte, 1) == 0) break;
            int c = byte;
            if (c != EOF && c > 0) {
                if (c == 'T') {
                    c5v4_level_hw_print();
                    printf("C5V4_LEVEL_TASK work_us=%u stack_free=%u heap_free=%u snapshot_bytes=8190\n",
                        s_level_work_us, s_level_task ? (unsigned)uxTaskGetStackHighWaterMark(s_level_task) : 0u,
                        (unsigned)esp_get_free_heap_size());
                    printf("C5VRX4_MENU active=%u boot_button_enabled=%u "
                           "standard=%s descriptor_bytes_max=%u chunk_bytes=%u "
                           "allocated_chunks=%u free=%u largest=%u\n",
                           s_menu_active, s_menu_boot_btn_enabled,
                           video_standard_name(resolved_menu_standard()),
                           (unsigned)(MENU_MAX_NODES * sizeof(dma_descriptor_t)),
                           (unsigned)(MENU_NODE_CHUNK * sizeof(dma_descriptor_t)),
                           s_menu_chunk_count,
                           (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA_DESC_AHB | MALLOC_CAP_INTERNAL),
                           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA_DESC_AHB | MALLOC_CAP_INTERNAL));
                }
                if (c == 'J') {
                    if (c5vrx4_reference_demod()) {
                        printf("C5V4_CVBS refused=reference_demod_no_span75_estimator\n");
                        continue;
                    }
                    if (__sync_bool_compare_and_swap(&s_cvbs_capture_running, 0u, 1u) &&
                        xTaskCreate(cvbs_capture_task, "cvbs_capture", 4096, NULL, 1, NULL) != pdPASS) {
                        __sync_lock_release(&s_cvbs_capture_running);
                        printf("C5V4_CVBS refused=task_memory\n");
                    }
                    continue;
                }
                if (c5vrx4_console(c)) continue;
                if (snr_meter_console(c)) continue;
                if (c == '`') rf_reboot_to_download(); /* flashing, never returns */
                if (c == 0x14) { dco_ab_toggle(); continue; }
                if (phy_rx_lab_profile_active() && c < 128 &&
                    !strchr("[]HpLl}q\r\n", c)) phy_rx_lab_stock();
                if (rf_native_agc_active() && c < 128 &&
                    strchr("FBWA+-kjasmD", c)) {
                    printf("C5VRX_NATIVE_AGC_OWNS_GAIN command=%c action=ignored "
                           "hint=N_returns_to_firmware_gain\n", c);
                    continue;
                }

                if (c == 'l' || c == 'L') {
                    int64_t now = esp_timer_get_time();
                    phy_rx_lab_mark();
                    s_last_user_lag_mark_us = now;
                    ++s_hw_counters.user_lag_mark_count;
                    long long gain_age_ms = s_last_gain_write_us > 0 ?
                        (long long)((now - s_last_gain_write_us) / 1000) : -1;
                    long long transport_age_ms = s_last_transport_event_us > 0 ?
                        (long long)((now - s_last_transport_event_us) / 1000) : -1;
                    long long phy_age_ms = s_last_phy_write_us > 0 ?
                        (long long)((now - s_last_phy_write_us) / 1000) : -1;
                    printf("[LAG MARK] #%lu gain_age=%lldms phy_age=%lldms phy_kind=%u transport_age=%lldms flags=0x%02lx G=%u state=%u\n",
                           (unsigned long)s_hw_counters.user_lag_mark_count,
                           gain_age_ms, phy_age_ms, (unsigned)s_last_phy_write_kind, transport_age_ms,
                           (unsigned long)s_last_transport_flags,
                           s_current_gain, (unsigned)s_agc_state);
                } else if (c == 'b') {
                    lab_enter_quiet_baseline();
                } else if (c == 'r') {
                    lab_reset_correlation();
                    printf("C5VRX_LAB_RESET gain=%u bw=%u afc=%u\n",
                           s_current_gain, s_current_bw40 ? 40u : 20u, (unsigned)s_afc_mode);
                } else if (c == 'p') {
                    lab_print_row("SNAPSHOT", NULL);
                } else if (c == 'E') {
                    p8env_capture_report();
                } else if (c == 'N') {
                    lab_toggle_native_agc_boot();
                } else if (c == 'T') {
                    rf_dump_agc_regs();
                } else if (c == 'Q') {
                    lab_dump_raw_probe();
                } else if (c == 'W') {
                    lab_run_bandwidth_probe(false);
                } else if (c == 'B') {
                    lab_run_bandwidth_probe(true);
                } else if (c == 'H') {
                    lab_print_arc_oracle();
                    phy_rx_lab_dump(false);
                } else if (c == '}') {
                    phy_rx_lab_toggle_monitor();
                } else if (c == '{') {
                    if (!rf_native_agc_active() && !s_menu_active) {
                        lab_enter_quiet_baseline();
                        phy_rx_lab_dump(true);
                    } else {
                        printf("PHYLAB analog_snapshot_refused=busy_or_native_owner\n");
                    }
                } else if (c == '[') {
                    if (!rf_native_agc_active() && !s_menu_active) {
                        /* Enter baseline once; later profiles restore their own
                         * saved fields without changing the RF reference. */
                        if (!phy_rx_lab_profile_active()) lab_enter_quiet_baseline();
                        phy_rx_lab_next_profile();
                    } else {
                        printf("PHYLAB profile_refused=busy_or_native_owner\n");
                    }
                } else if (c == ']') {
                    phy_rx_lab_stock();
                } else if (c == 'K') {
                    lab_request_fresh_phy_calibration();
                } else if (c == '(' || c == ')') {
                    lab_run_native_hold(c == '(' ? 1u : 100u);
                } else if (c == ':') {
                    lab_run_11p_probe();
                } else if (c == '\'') {
                    lab_run_sigrssi();
                } else if (c == 'd') {
                    lab_run_sigrssi_ladder();
                } else if (c == '"') {
                    lab_run_phy_track();
                } else if (c == '/') {
                    lab_run_dfilt();
                } else if (c == '~') {
                    lab_run_rx_recal();
                } else if (c == '?') {
                    flight_log_print();
                } else if (c == ';') {
                    lab_run_bw20_wide();
                } else if (c == '!') {
                    lab_predemod_status();
                } else if (c == '@') {
                    lab_run_sample_phase_scan();
                } else if (c == '#') {
                    lab_run_dco_probe();
                } else if (c == '$') {
                    lab_run_filter_sweep();
                } else if (c == '=') {
                    (void)lab_run_bw_calibration(false);
                } else if (c == '*') {
                    if (lab_run_agc_witness(false) && c5vrx4_agc_mask_enabled()) {
                        printf("AGC_WITNESS rebooting to apply the acquisition mask\n");
                        fflush(stdout);
                        vTaskDelay(pdMS_TO_TICKS(150));
                        esp_restart();
                    }
                } else if (c == 'D') {
                    apply_rx_profile(RX_PROFILE_DIRECT_GAIN);
                    settings_save();
                    printf("[RX PROFILE] -> DIRECT GAIN V5\n");
                } else if (c == 'X') {
                    cycle_rx_profile();
                } else if (c == 't') {
                    rf_dump_tracked_timers();
                } else if (c == 'q') {
                    s_lab_quiet = !s_lab_quiet;
                    printf("C5VRX_LAB_QUIET enabled=%u\n", s_lab_quiet ? 1u : 0u);
                } else if (c == '+' || c == 'k') {
                    leave_experimental_profile();
                    s_agc_mode = ANALOG_AGC_MANUAL;
                    uint8_t manual_max = rf_get_arc_gain_table()->max_index;
                    if (s_current_gain < manual_max) {
                        s_current_gain = s_current_gain <= manual_max - LAB_GAIN_STEP ?
                                         (uint8_t)(s_current_gain + LAB_GAIN_STEP) : manual_max;
                        s_last_gain_write_us = esp_timer_get_time();
                        s_last_phy_write_us = s_last_gain_write_us;
                        s_last_phy_write_kind = PHY_WRITE_GAIN;
                        rf_set_rx_gain(true, s_current_gain);
                        ++s_gain_transition_count;
                    }
                    settings_save();
                    printf("[MANUAL GAIN] = %u (reg=0x%08lx)\n", s_current_gain, (unsigned long)rf_get_rx_gain_reg());
                } else if (c == '-' || c == 'j') {
                    leave_experimental_profile();
                    s_agc_mode = ANALOG_AGC_MANUAL;
                    if (s_current_gain > LAB_GAIN_MIN) {
                        s_current_gain = s_current_gain >= LAB_GAIN_MIN + LAB_GAIN_STEP ?
                                         (uint8_t)(s_current_gain - LAB_GAIN_STEP) : LAB_GAIN_MIN;
                        s_last_gain_write_us = esp_timer_get_time();
                        s_last_phy_write_us = s_last_gain_write_us;
                        s_last_phy_write_kind = PHY_WRITE_GAIN;
                        rf_set_rx_gain(true, s_current_gain);
                        ++s_gain_transition_count;
                    }
                    settings_save();
                    printf("[MANUAL GAIN] = %u (reg=0x%08lx)\n", s_current_gain, (unsigned long)rf_get_rx_gain_reg());
                } else if (c == 'a') {
                    leave_experimental_profile();
                    s_agc_mode = ANALOG_AGC_ACTIVE;
                    settings_save();
                    printf("[AGC MODE] -> ACTIVE (Self-Calibrating Adaptive Gain Controller ACTIVE)\n");
                } else if (c == 's') {
                    leave_experimental_profile();
                    s_agc_mode = ANALOG_AGC_SHADOW;
                    settings_save();
                    printf("[AGC MODE] -> FROZEN (RF gain held at %u)\n", s_current_gain);
                } else if (c == 'm') {
                    leave_experimental_profile();
                    s_agc_mode = ANALOG_AGC_MANUAL;
                    settings_save();
                    printf("[AGC MODE] -> MANUAL (Fixed gain=%u)\n", s_current_gain);
                } else if (c == 'c') {
                    rf_cycle_channel_in_band();
                    const fpv_channel_t *ch = rf_get_current_channel();
                    s_cfo_khz = 0;
                    s_agc_state = AGC_STATE_SEARCH;
                    ++s_profile_generation;
                    video_standard_detector_reset();
                    settings_save();
                    printf("[CHANNEL] Switched to %s (%u MHz) in %s\n",
                           ch->name, ch->freq_mhz, rf_get_band_name(rf_get_current_band()));
                } else if (c == 'C') {
                    rf_cycle_band();
                    const fpv_channel_t *ch = rf_get_current_channel();
                    s_cfo_khz = 0;
                    s_agc_state = AGC_STATE_SEARCH;
                    ++s_profile_generation;
                    video_standard_detector_reset();
                    settings_save();
                    printf("[BAND] Switched to %s - Channel %s (%u MHz)\n",
                           rf_get_band_name(rf_get_current_band()), ch->name, ch->freq_mhz);
                } else if (c == 'f') {
                    if (s_afc_mode == AFC_MODE_AUTO) {
                        s_afc_mode = AFC_MODE_HOLD;
                        printf("[AFC] -> HOLD (Current offset %+d kHz frozen)\n", rf_get_frequency_offset_khz());
                    } else if (s_afc_mode == AFC_MODE_HOLD) {
                        s_afc_mode = AFC_MODE_OFF;
                        apply_frequency_offset_khz_tracked(0);
                        printf("[AFC] -> OFF (Offset reset to 0 kHz)\n");
                    } else {
                        s_afc_mode = AFC_MODE_AUTO;
                        printf("[AFC] -> AUTO EXPERIMENTAL (uncalibrated WBFM bias estimator)\n");
                    }
                    settings_save();
                } else if (c == ',' || c == '<') {
                    step_frequency_offset_khz_tracked(-50);
                    settings_save();
                    int off = rf_get_frequency_offset_khz();
                    int tot = (int)rf_get_current_channel()->freq_mhz * 1000 + off;
                    printf("[FINE TUNE] Offset = %+d kHz (Tuned: %d.%03d MHz)\n",
                           off, tot / 1000, (tot % 1000 >= 0 ? tot % 1000 : -(tot % 1000)));
                } else if (c == '.' || c == '>') {
                    step_frequency_offset_khz_tracked(+50);
                    settings_save();
                    int off = rf_get_frequency_offset_khz();
                    int tot = (int)rf_get_current_channel()->freq_mhz * 1000 + off;
                    printf("[FINE TUNE] Offset = %+d kHz (Tuned: %d.%03d MHz)\n",
                           off, tot / 1000, (tot % 1000 >= 0 ? tot % 1000 : -(tot % 1000)));
                } else if (c == '0') {
                    apply_frequency_offset_khz_tracked(0);
                    settings_save();
                    printf("[FINE TUNE] Offset reset to +0 kHz\n");
                } else if (c == 'e') {
                    PARL_IO.rx_clk_cfg.rx_clk_i_inv = !PARL_IO.rx_clk_cfg.rx_clk_i_inv;
                    printf("[EDGE] RX SAMPLE EDGE TOGGLED -> %s (rx_clk_i_inv=%d)\n",
                           PARL_IO.rx_clk_cfg.rx_clk_i_inv ? "NEG" : "POS",
                           (int)PARL_IO.rx_clk_cfg.rx_clk_i_inv);
                } else if (c == 'o' || c == 'v' || c == 'V' || c == 'O' ||
                           c == ' ' || c == 'n' || c == '\t' ||
                           c == '\r' || c == '\n' || c == 'x') {
                    /* The control task exclusively owns mode changes and rendering. */
                    if (xQueueSend(s_menu_commands, &c, 0) != pdTRUE) {
                        printf("[MENU] Command queue full\n");
                    }
                } else {
                    uint32_t rx_dscr = 0, tx_dscr = 0;
                    uint32_t rx_off = get_rx_dma_offset(&rx_dscr);
                    uint32_t tx_off = get_tx_dma_offset(&tx_dscr);
                    uint32_t dist = (rx_off >= tx_off) ? (rx_off - tx_off) : (sizeof(s_raw_ring) - tx_off + rx_off);
                    int rx_nodes = s_rx_dscr_count;
                    int tx_nodes = s_menu_active ? 0 : s_tx_dscr_count;
                    const fpv_channel_t *ch = rf_get_current_channel();
                    int off = rf_get_frequency_offset_khz();
                    int tot = (int)ch->freq_mhz * 1000 + off;

                    printf("\n=======================================================\n");
                    printf(" C5VRX-4 REALTIME RECEPTION & FREQUENCY DIAGNOSTICS\n");
                    printf(" Receiver Channel:           %s (%u MHz)\n", ch->name, ch->freq_mhz);
                    printf(" Tuned Frequency:            %d.%03d MHz (Offset: %+d kHz)\n",
                           tot / 1000, (tot % 1000 >= 0 ? tot % 1000 : -(tot % 1000)), off);
                    printf(" Carrier Frequency Offset:   %+d kHz (VTX %s)\n",
                           s_cfo_khz, (s_cfo_khz > 20) ? "high" : (s_cfo_khz < -20) ? "low" : "centered");
                    printf(" AFC Mode:                   %s\n",
                           (s_afc_mode == AFC_MODE_AUTO) ? "AUTO EXPERIMENTAL (uncalibrated estimator)" :
                           (s_afc_mode == AFC_MODE_HOLD) ? "HOLD (Offset Frozen)" : "OFF (0 kHz)");
                    printf(" RX Profile:                 %s%s\n",
                           rx_profile_name(),
                           s_rx_profile == RX_PROFILE_DIRECT_GAIN ? " [DEFAULT]" : " [A/B]");
                    printf(" RF Bandwidth:               mode=%s active=%s\n",
                           rf_bw_mode_name(), s_current_bw40 ? "BW40" : "BW20");
                    printf(" Adaptive AGC Mode:          %s (State=%s)\n",
                           (s_agc_mode == ANALOG_AGC_ACTIVE) ? "ACTIVE" :
                           (s_agc_mode == ANALOG_AGC_SHADOW) ? "SHADOW (Safe Dry-Run)" : "MANUAL",
                           (s_agc_state == AGC_STATE_TRACK) ? "TRACK" :
                           (s_agc_state == AGC_STATE_LEARN) ? "LEARN" : "SEARCH");
                    printf(" Gain Settings:              G_actual=%u, G_shadow_rec=%u (reg=0x%08lx)\n",
                           s_current_gain, s_shadow_gain, (unsigned long)rf_get_rx_gain_reg());
                    if (s_rx_profile == RX_PROFILE_DIRECT_GAIN) {
                        printf(" Direct Gain V5 State:       %s (target=G%u, delta=%+d, floor=G%u)\n",
                               s_last_direct_gain_state == DG3_HOLD ? "HOLD" : "SEEK",
                               (unsigned)s_last_direct_gain_target,
                               s_last_direct_gain_delta,
                               20u);
                        printf(" Direct Gain V5 Telemetry:   writes=%" PRIu32 ", hold_samples=%" PRIu32
                               " verified=%" PRIu32 " learned=%" PRIu32
                               " settle_us=%u/%u/%u\n",
                               s_last_direct_gain_total_writes,
                               s_last_direct_gain_hold_cycles,
                               s_direct_gain_v3.verified, s_direct_gain_v3.learned,
                               s_direct_gain_v3.settle_us[DG3_FINE],
                               s_direct_gain_v3.settle_us[DG3_BB],
                               s_direct_gain_v3.settle_us[DG3_RF]);
                    }
                    printf(" FM Vector Metrics:          P_median=%d, Q_phase=%d%%, Clip=%d.%d%%, Origin=%d.%d%%\n",
                           s_last_p_median, s_last_q_phase,
                           s_last_clip_permille / 10, s_last_clip_permille % 10,
                           s_last_origin_permille / 10, s_last_origin_permille % 10);
                    printf(" IQ Frontend Metrics:        DC I=%+.2f Q=%+.2f, skew=%d.%d%% cross=%d.%d%%\n",
                           (double)s_last_dc_i_x100 / 100.0, (double)s_last_dc_q_x100 / 100.0,
                           s_last_iq_skew_permille / 10, s_last_iq_skew_permille % 10,
                           s_last_iq_cross_permille / 10, s_last_iq_cross_permille % 10);
                    printf(" Demod Quality:              endpoint winding=%d.%d%% strong=%d.%d%% syncQ=%d width=%u\n",
                           s_last_winding_permille / 10, s_last_winding_permille % 10,
                           s_last_strong_winding_permille / 10, s_last_strong_winding_permille % 10,
                           s_last_sync_quality, (unsigned)s_last_sync_width_20m);
                    printf(" Gain Transitions:           %lu (control window=%u IQ samples / %.1f us)\n",
                           (unsigned long)s_gain_transition_count, CONTROL_SAMPLE_BYTES,
                           (double)CONTROL_SAMPLE_BYTES * 1000000.0 / (double)IQ_RATE_HZ);
                    printf(" GDMA Ring:                  dist=%lu (rx_off=%lu, tx_off=%lu)\n",
                           (unsigned long)dist, (unsigned long)rx_off, (unsigned long)tx_off);
                    printf(" Zero-EOF Status:            RX patched=%d nodes, TX patched=%d nodes\n",
                           rx_nodes, tx_nodes);
                    printf(" Transport Faults:           PARLIO tx_empty=%lu rx_ovf=%lu tx_eof=%lu | GDMA in=%lu out=%lu | BS eof_ovl=%lu\n",
                           (unsigned long)s_hw_counters.parl_tx_rempty_count,
                           (unsigned long)s_hw_counters.parl_rx_wovf_count,
                           (unsigned long)s_hw_counters.parl_tx_eof_count,
                           (unsigned long)s_hw_counters.gdma_in_fault_count,
                           (unsigned long)s_hw_counters.gdma_out_fault_count,
                           (unsigned long)s_hw_counters.bs_eof_overload_count);
                    int64_t transport_age_ms = s_last_transport_event_us > 0 ?
                        (esp_timer_get_time() - s_last_transport_event_us) / 1000 : -1;
                    printf(" Lag Correlation:            events=%lu near_gain_200ms=%lu near_phy_200ms=%lu gain_Qdrop=%lu marks=%lu last_flags=0x%02lx age=%lldms checks=%lu\n",
                           (unsigned long)s_hw_counters.lag_event_count,
                           (unsigned long)s_hw_counters.near_gain_event_count,
                           (unsigned long)s_hw_counters.near_phy_event_count,
                           (unsigned long)s_hw_counters.gain_quality_drop_count,
                           (unsigned long)s_hw_counters.user_lag_mark_count,
                           (unsigned long)s_last_transport_flags,
                           (long long)transport_age_ms,
                           (unsigned long)s_hw_counters.checks);
                    unsigned available = s_lag_event_head < LAG_EVENT_LOG_SIZE ?
                                         s_lag_event_head : LAG_EVENT_LOG_SIZE;
                    for (unsigned n = 0; n < available; ++n) {
                        uint32_t seq = s_lag_event_head - n;
                        const lag_event_t *event = &s_lag_events[(seq - 1u) % LAG_EVENT_LOG_SIZE];
                        printf("  EVT#%lu flags=0x%02lx t=%lldus rx=%u tx=%u G%u state=%u\n",
                               (unsigned long)event->seq,
                               (unsigned long)event->flags,
                               (long long)event->time_us,
                               event->rx_off, event->tx_off,
                               event->gain, event->agc_state);
                    }
                    printf(" RX Sample Edge:             %s (rx_clk_i_inv=%d)\n",
                           PARL_IO.rx_clk_cfg.rx_clk_i_inv ? "NEG" : "POS",
                           (int)PARL_IO.rx_clk_cfg.rx_clk_i_inv);
                    printf(" Video Standard:             mode=%s output=%s detected=%s period=%u samples (PAL=%u NTSC=%u)\n",
                           s_video_std_mode == VIDEO_STD_MODE_AUTO ? "AUTO" :
                           s_video_std_mode == VIDEO_STD_MODE_PAL ? "PAL" : "NTSC",
                           video_standard_name(s_video_std),
                           s_detected_video_std_valid ? video_standard_name(s_detected_video_std) : "UNKNOWN",
                           s_last_line_period_20m,
                           s_video_std_pal_score, s_video_std_ntsc_score);
                    printf(" Menu Status:                %s\n",
                           MENU_RUNTIME_ENABLED ?
                           (s_menu_active ? "OPEN" : "CLOSED") :
                           "TEMPORARILY DISABLED (live video only)");
                    printf(" Keys: T/E/p diagnostics, d sigRSSI ladder, J CVBS snapshots, r reset counters, l/L lag mark\n"
                           " N/X native AGC toggle (reboot), D Direct Gain V5, a/s/m active/frozen/manual\n"
                           " +/- manual gain, c channel, f AFC, ,/. offset, 0 center, e sample edge\n"
                           " o/v/O menu/standard/BOOT, space/n/tab next, enter/x select\n"
                           " ! predemod status, @ sample phase, = bandwidth calibration (VTX off)\n"
                           " * native witness calibration, # DCO A/B, $ filter sweep\n"
                           " W/B bandwidth A/B, : 11p A/B, / digital-filter A/B, ; BW20-wide A/B\n"
                           " '/\" signal-RSSI/tracking A/B, (/) native hold A/B, H/{/}/[/] PHY lab\n"
                           " K fresh PHY calibration, ~ RX recal lab, ? flight log, Ctrl-T DCO A/B\n"
                           " q quiet, t timers, ` USB download\n"
                           " 7 SNR reading, 8 SNR floor at this gain (VTX off), 9 5 Hz SNR rows\n"
                           " h/M/Z decode/transfer/lanes, u/%%/&/^/|/_/y/w boot options (reboot)\n");
                    printf("=======================================================\n\n");
                }
            }
        }
        /* Heartbeat (USB lockup diagnosis, 2026-10-06): if this stops while
         * "HB predemod" goes on, the console task is starved or stuck. */
        static int64_t hb_us;
        int64_t hb_now = esp_timer_get_time();
        if (hb_now - hb_us > 5000000) {
            hb_us = hb_now;
            printf("HB console t_s=%lld idle_raster=%u menu=%u\n", hb_now / 1000000,
                   IDLE_RASTER_ACTIVE(), s_menu_active);
        }
        snr_meter_tick();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
