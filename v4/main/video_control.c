/* C5VRX-4: control responsibilities. */
#include "video_internal.h"

static int signal_strength_score(const control_metrics_t *m, uint8_t gain);
static bool copy_recent_endpoints(uint8_t *dst, size_t n);
static analog_video_t scan_video_confidence(void);
static void channel_auto_search(void);
static void handle_button_short_click(void);
static void open_recovery_menu(void);
static void handle_button_long_click(void);
#define SCAN_VIDEO_PAIRS 4092u

#define GAIN_SETTLE_TICKS 10        /* 500 ms decision hold after a physical gain write */

#define BOOT_BTN_GPIO    GPIO_NUM_28 /* Seeed Studio XIAO ESP32-C5 BOOT Button */

#include "phase8_gain_lut.h"

/* AGC sampling and channel scan are serialized in analog_agc_task, so they
 * share one descriptor-sized CPU snapshot instead of reserving 8 KiB. */
static uint8_t s_control_sample_buf[CONTROL_SAMPLE_BYTES];

static volatile int s_last_n_clip = 0;

static volatile int s_last_n_origin = 0;

/* Issue #128: median analog-video confidence of three windows of ~4096
 * endpoints on the current channel (analog_video_detect.c). The window is
 * heap-allocated only for the duration of a scan: static buffers took the
 * internal heap the standalone menu needs (ESP_ERR_NO_MEM). */

static uint8_t *s_scan_video_buf;

static int signal_strength_score(const control_metrics_t *m, uint8_t gain)
{
    if (m->q_phase < 18 || m->origin_permille > 850) return 0;
    int coherence = (m->q_phase - 18) * 100 / 62;
    /* A finer range lane shows the envelope 2^k larger: undo it (power). */
    int power = ((m->p_median >> (2u * rf_get_iq_lanes())) - 6) * 100 / 28;
    int max_gain = profile_gain_max();
    int gain_headroom = (max_gain - (int)gain) * 100 /
                        (max_gain > 2 ? max_gain - 2 : 1);
    if (coherence < 0) coherence = 0; else if (coherence > 100) coherence = 100;
    if (power < 0) power = 0; else if (power > 100) power = 100;
    if (gain_headroom < 0) gain_headroom = 0; else if (gain_headroom > 100) gain_headroom = 100;
    return (coherence * 2 + power + gain_headroom) / 4;
}

void init_boot_button(void)
{
    const gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << BOOT_BTN_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
}

/* Copy the endpoint bytes (odd ring byte of each pair, as Phase8 reads
 * them) of the most recent `n` RX-completed pairs, unwrapping the ring. */
static bool copy_recent_endpoints(uint8_t *dst, size_t n)
{
    if (s_rx_dma_ch < 0 || s_rx_dma_ch >= 3 || s_rx_dscr_count < 2 ||
        2u * n > sizeof(s_raw_ring) / 2u) return false;
    int idx = find_dscr_index(s_rx_dscr_nodes, s_rx_dscr_count,
                              AHB_DMA.channel[s_rx_dma_ch].in.in_dscr_bf0.val);
    if (idx < 0) return false;
    uint8_t *buf = s_rx_dscr_nodes[idx].buffer;
    if (buf < s_raw_ring || buf >= s_raw_ring + sizeof(s_raw_ring)) return false;
    size_t end = (size_t)(buf - s_raw_ring) & ~(size_t)1u;
    size_t pos = (end + sizeof(s_raw_ring) - 2u * n) % sizeof(s_raw_ring);
    sync_dma_m2c(s_raw_ring, sizeof(s_raw_ring));
    for (size_t k = 0; k < n; ++k) {
        dst[k] = s_raw_ring[pos + 1u];
        pos = (pos + 2u) % sizeof(s_raw_ring);
    }
    return true;
}

static analog_video_t scan_video_confidence(void)
{
    analog_video_t v[3] = {{0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}};
    if (!s_scan_video_buf) return v[0];
    for (unsigned w = 0; w < 3u; ++w) {
        if (w) vTaskDelay(pdMS_TO_TICKS(10));
        if (copy_recent_endpoints(s_scan_video_buf, SCAN_VIDEO_PAIRS))
            v[w] = analog_video_detect(s_scan_video_buf, SCAN_VIDEO_PAIRS,
                                       c5vrx_phase8_gain_lut);
    }
    /* median by confidence */
    analog_video_t a = v[0], b = v[1], c = v[2], t;
    if (a.confidence > b.confidence) { t = a; a = b; b = t; }
    if (b.confidence > c.confidence) { t = b; b = c; c = t; }
    if (a.confidence > b.confidence) { t = a; a = b; b = t; }
    return b;
}

static void channel_auto_search(void)
{
    const size_t original_channel = rf_get_channel_index();
    const uint8_t original_gain = s_current_gain;
    const size_t channel_count = rf_get_channel_count();
    size_t best_channel = original_channel;
    int best_rank = -1;
    int best_quality = 0;
    int best_video = -1;
    int best_offset = 0x7fffffff;
    const uint8_t max_gain = rf_get_arc_gain_table()->max_index;

    s_channel_scan_active = true;
    s_channel_scan_progress = 0;
    rf_set_rx_gain(true, 52u); /* Compare every channel at the same RF gain. */
    s_scan_video_buf = heap_caps_malloc(SCAN_VIDEO_PAIRS, MALLOC_CAP_INTERNAL);
    if (!s_scan_video_buf)
        printf("[AUTO SEARCH] no memory for the video check; channel kept\n");

    /* Issue #128: RF strength alone let 5 GHz Wi-Fi win (and pull the scan
     * into the L band). A channel only qualifies with analog-video
     * confidence (line-period periodicity); RF quality only breaks ties. A
     * channel starved at gain 52 (far VTX) is re-measured at maximum gain so
     * a weak but real VTX is not missed. */
    for (size_t channel = 0; channel < channel_count; ++channel) {
        if (rf_set_channel(channel) != ESP_OK) continue;
        vTaskDelay(pdMS_TO_TICKS(90));
        uint8_t *src = get_completed_rx_sample_window(CONTROL_SAMPLE_BYTES);
        size_t scan_ring_offset =
            (src >= s_raw_ring && src < s_raw_ring + sizeof(s_raw_ring)) ?
            (size_t)(src - s_raw_ring) : 0u;
        sync_dma_m2c(src, CONTROL_SAMPLE_BYTES);
        memcpy(s_control_sample_buf, src, sizeof(s_control_sample_buf));
        control_metrics_t metrics =
            analyze_control_window(s_control_sample_buf,
                                   sizeof(s_control_sample_buf),
                                   scan_ring_offset);
        int quality = signal_strength_score(&metrics, 52u);
        fusion_observation_t scan_fusion = fusion_make_observation(
            metrics.p_median, metrics.q_phase, metrics.clip_permille,
            metrics.origin_permille, metrics.winding_permille,
            metrics.strong_winding_permille, metrics.iq_skew_permille,
            metrics.iq_cross_permille, 0, active_demod_shadow(metrics.fusion_shadow));
        int rank = scan_fusion.quality + quality * 2;
        analog_video_t video = scan_video_confidence();
        if (video.confidence < ANALOG_VIDEO_MIN_CONFIDENCE &&
            metrics.origin_permille > 500) {
            rf_set_rx_gain(true, max_gain);
            vTaskDelay(pdMS_TO_TICKS(20));
            analog_video_t far = scan_video_confidence();
            rf_set_rx_gain(true, 52u);
            if (far.confidence > video.confidence) video = far;
        }
        if (video.confidence >= ANALOG_VIDEO_MIN_CONFIDENCE) {
            int off = video.offset_khz < 0 ? -video.offset_khz : video.offset_khz;
            printf("[AUTO SEARCH] candidate %s (%u MHz): video=%d offset=%d kHz lag=%d %s rf=%d\n",
                   rf_get_current_channel()->name, rf_get_current_channel()->freq_mhz,
                   video.confidence, video.offset_khz, video.lag,
                   video.standard == 1 ? "PAL" : "NTSC", rank);
            /* The BW40 filter lets a VTX through on neighbouring channels
             * too (hardware: A1's VTX gave valid video on B8/F7/F8/R7), and
             * the confidence ignores offset by design. Only a carrier
             * centred within 3 MHz qualifies. That alone is not enough: the
             * 50 ns offset measurement wraps every 20 MHz, so a channel
             * exactly 20 MHz away also reads "centred" (hardware: A2, E5,
             * F6 around an A1 VTX; one scan picked A2). There the VTX sits
             * on the filter edge and is weak, so among centred candidates
             * the strongest RF wins (A1 rf=168 vs 0), then the better
             * centred one in 500 kHz steps (B8 at 5866 MHz reads the same
             * RF as A1 but -1040 vs -14 kHz), then confidence. */
            bool centred = off <= 3000;
            bool best_centred = best_offset <= 3000;
            int off_bucket = off / 500, best_bucket = best_offset / 500;
            if ((centred && !best_centred) ||
                (centred == best_centred &&
                 (rank > best_rank ||
                  (rank == best_rank &&
                   (off_bucket < best_bucket ||
                    (off_bucket == best_bucket &&
                     video.confidence > best_video)))))) {
                best_offset = off;
                best_video = video.confidence;
                best_rank = rank;
                best_channel = channel;
                best_quality = quality;
            }
        }
        s_channel_scan_progress = (unsigned)((channel + 1u) * 100u / channel_count);
        menu_render_menu();
    }

    /* Confirm the winner before committing; otherwise keep the channel. */
    if (best_video >= 0) {
        (void)rf_set_channel(best_channel);
        vTaskDelay(pdMS_TO_TICKS(90));
        analog_video_t confirm = scan_video_confidence();
        if (confirm.confidence < ANALOG_VIDEO_MIN_CONFIDENCE) {
            rf_set_rx_gain(true, max_gain);
            vTaskDelay(pdMS_TO_TICKS(20));
            analog_video_t far = scan_video_confidence();
            if (far.confidence > confirm.confidence) confirm = far;
        }
        /* The winner must also have its carrier centred (a neighbouring
         * channel sees the VTX aliased several MHz off). */
        int coff = confirm.offset_khz < 0 ? -confirm.offset_khz : confirm.offset_khz;
        if (confirm.confidence < ANALOG_VIDEO_MIN_CONFIDENCE || coff > 3000) {
            printf("[AUTO SEARCH] %s failed confirmation (video=%d offset=%d kHz)\n",
                   rf_get_current_channel()->name, confirm.confidence,
                   confirm.offset_khz);
            best_video = -1;
        }
    }
    if (best_video < 0) {
        best_channel = original_channel;
        best_rank = -1;
    }
    heap_caps_free(s_scan_video_buf);
    s_scan_video_buf = NULL;
    (void)rf_set_channel(best_channel);
    rf_set_rx_gain(true, original_gain);
    ++s_profile_generation;
    s_signal_strength = best_rank < 0 ? 0 : best_quality;
    s_channel_scan_active = false;
    s_channel_scan_progress = 0;
    s_cfo_khz = 0;
    s_agc_state = AGC_STATE_SEARCH;
    video_standard_detector_reset();
    settings_save();
    menu_render_menu();
    printf("[AUTO SEARCH] %s -> %s (%u MHz), signal=%d\n",
           best_rank < 0 ? "No analog video found; restored" : "Selected",
           rf_get_current_channel()->name, rf_get_current_channel()->freq_mhz,
           s_signal_strength);
}

static void handle_button_short_click(void)
{
    if (s_menu_active && !IDLE_RASTER_ACTIVE()) {
        if (s_menu_edit) {
            s_menu_item = (s_menu_item + 1u) % (menu_item_count() + 1u);
            menu_render_menu();
            s_menu_timeout_ticks = 0;
            printf("[BTN: SHORT] Menu item -> %u\n", s_menu_item);
            return;
        }
        s_menu_cursor = (s_menu_cursor + 1) % 6;
        menu_render_menu();
        s_menu_timeout_ticks = 0;
        printf("[BTN: SHORT] Menu cursor -> %d\n", s_menu_cursor);
    } else {
        rf_cycle_channel_in_band();
        s_cfo_khz = 0;
        s_agc_state = AGC_STATE_SEARCH;
        ++s_profile_generation;
        video_standard_detector_reset();
        settings_save();
        const fpv_channel_t *ch = rf_get_current_channel();
        if (IDLE_RASTER_ACTIVE()) menu_render_menu();
        printf("[BTN: SHORT] Channel switched to %s (%u MHz) in %s\n",
               ch->name, ch->freq_mhz, rf_get_band_name(rf_get_current_band()));
    }
}

static void open_recovery_menu(void)
{
    /* Persisted Safe Flight state must never make the on-screen controls
     * unreachable after flashing another build. A deliberate three-second
     * hold restores the simplest proven video contract before menu TX starts. */
    s_menu_boot_btn_enabled = true;
    s_video_std_mode = VIDEO_STD_MODE_AUTO;
    s_demod_mode = DEMOD_MODE_GOLDEN_PHASE5;
    apply_rx_profile(RX_PROFILE_DIRECT_GAIN);
    video_standard_detector_reset();
    s_menu_cursor = 0;
    s_menu_timeout_ticks = 0;
    settings_save();
    video_open_menu();
    printf("[RECOVERY] GOLDEN + 6BIT@40 + DIRECT GAIN V3 TEST restored; menu %s\n",
           s_menu_active ? "opened" : "unavailable");
}

static void handle_button_long_click(void)
{
    if (!s_menu_active || IDLE_RASTER_ACTIVE()) {
        if (!MENU_RUNTIME_ENABLED) {
            printf("[BTN: LONG] Menu temporarily disabled; live video unchanged\n");
            return;
        }
        if (!s_menu_boot_btn_enabled) {
            printf("[BTN: LONG] Menu via BOOT button is DISABLED (Safe Flight Mode)\n");
            return;
        }
        s_menu_cursor = 0;
        s_menu_timeout_ticks = 0;
        video_open_menu();
        printf("[BTN: LONG] Menu Opened!\n");
    } else {
        if (s_menu_edit) {
            if (s_menu_item >= menu_item_count()) {
                s_menu_edit = false;
                s_menu_item = 0;
            } else if (!menu_item_apply(s_menu_item)) {
                return;
            }
            menu_render_menu();
            s_menu_timeout_ticks = 0;
            return;
        }
        if (s_menu_cursor == 2 || s_menu_cursor == 3) {
            s_menu_edit = true;
            s_menu_item = 0;
            menu_render_menu();
            s_menu_timeout_ticks = 0;
            return;
        }
        if (s_menu_cursor == 4) {
            menu_cycle_standard_mode(); /* re-renders and saves */
            s_menu_timeout_ticks = 0;
            printf("[MENU: STANDARD] -> %s\n",
                   s_video_std_mode == VIDEO_STD_MODE_AUTO ? "AUTO" :
                   s_video_std_mode == VIDEO_STD_MODE_PAL ? "PAL" : "NTSC");
            return;
        }
        if (s_menu_cursor == 5 && menu_changes_pending()) {
            settings_save();
            printf("[BTN: LONG] Save and exit -> reboot to apply boot options\n");
            fflush(stdout);
            vTaskDelay(pdMS_TO_TICKS(150));
            esp_restart();
        }
        switch (s_menu_cursor) {
        case 0: /* BAND */
            rf_cycle_band();
            s_cfo_khz = 0;
            s_agc_state = AGC_STATE_SEARCH;
            ++s_profile_generation;
            video_standard_detector_reset();
            settings_save();
            printf("[MENU: BAND] Switched to %s\n", rf_get_band_name(rf_get_current_band()));
            break;
        case 1: /* CHANNEL */
            rf_cycle_channel_in_band();
            s_cfo_khz = 0;
            s_agc_state = AGC_STATE_SEARCH;
            ++s_profile_generation;
            video_standard_detector_reset();
            settings_save();
            printf("[MENU: CHANNEL] Switched to %s (%u MHz)\n",
                   rf_get_current_channel()->name, rf_get_current_channel()->freq_mhz);
            break;
        case 2: /* RF BANDWIDTH; gain is always native AGC from the menu */
            cycle_rf_bandwidth_mode();
            settings_save();
            printf("[MENU: RF BW] Mode -> %s (active %s)\n",
                   rf_bw_mode_name(), s_current_bw40 ? "BW40" : "BW20");
            break;
        case 3: /* AFC MODE */
            if (s_afc_mode == AFC_MODE_AUTO) {
                s_afc_mode = AFC_MODE_HOLD;
            } else if (s_afc_mode == AFC_MODE_HOLD) {
                s_afc_mode = AFC_MODE_OFF;
                apply_frequency_offset_khz_tracked(0);
            } else {
                s_afc_mode = AFC_MODE_AUTO;
            }
            printf("[MENU: AFC] Mode -> %d\n", s_afc_mode);
            settings_save();
            break;
        case 4: /* VIDEO OUTPUT */
            printf("[MENU: OUTPUT] 6BIT@40 fixed for C5V4 UNWRAP/75\n");
            break;
        case 5: /* SAVE & EXIT */
            settings_save();
            video_set_menu_mode(false);
            printf("[BTN: LONG] Menu Closed -> Live Video!\n");
            return;
        default:
            break;
        }
        menu_render_menu();
        s_menu_timeout_ticks = 0;
    }
}

void analog_agc_task(void *arg)
{
    (void)arg;
    int settle_ticks = 0;
    afc2_ctrl_t afc2_ctrl = {0};
    bool afc2_own_write = false;
    uint8_t afc_lost_windows = 0;
    int menu_refresh_ticks = 0;
    uint32_t seen_profile_generation = s_profile_generation;
    int sync_age_ticks = 40;
    int btn_ticks = 0;
    bool btn_long_fired = false;
    bool btn_scan_fired = false;
    bool btn_recovery_fired = false;
    bool was_locked = false;
    uint32_t seen_phy_generation = phy_rx_lab_generation();
    int boot_grace_ticks = 20;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(50));

        /* Labs exclusively own gain/BW/AFC while observing the PHY. */
        if (s_rssi_probe_active) continue;

        int command;
        for (unsigned commands = 0; commands < 16 &&
             xQueueReceive(s_menu_commands, &command, 0) == pdTRUE; ++commands) {
            if (command == 'o') {
                if (MENU_RUNTIME_ENABLED) {
                    if (!s_menu_active || IDLE_RASTER_ACTIVE()) video_open_menu();
                    else video_set_menu_mode(false);
                }
                else printf("[MENU] Temporarily disabled; live video unchanged\n");
            } else if (command == 'v' || command == 'V') {
                if (MENU_RUNTIME_ENABLED) menu_cycle_standard_mode();
                else printf("[MENU] Video-standard control unavailable while menu is disabled\n");
            } else if (command == 'O') {
                if (MENU_RUNTIME_ENABLED) {
                    s_menu_boot_btn_enabled = !s_menu_boot_btn_enabled;
                    settings_save();
                }
                else printf("[MENU] BOOT menu trigger is temporarily disabled\n");
            } else if (s_menu_active && (command == ' ' || command == 'n' || command == '\t'))
                handle_button_short_click();
            else if (s_menu_active && !IDLE_RASTER_ACTIVE()) handle_button_long_click();
        }

        if (boot_grace_ticks > 0) {
            boot_grace_ticks--;
            btn_ticks = 0;
            btn_long_fired = false;
            btn_scan_fired = false;
            btn_recovery_fired = false;
        } else {
            int btn_level = gpio_get_level(BOOT_BTN_GPIO);
            if (btn_level == 0) {
                btn_ticks++;

                /* This path deliberately ignores the persisted BOOT-menu bit.
                 * The ordinary 0.6 s long-click may report Safe Flight at tick
                 * 12; continuing to hold until tick 60 must still recover. */
                if ((!s_menu_active || IDLE_RASTER_ACTIVE()) && btn_ticks >= 60 &&
                    !btn_recovery_fired) {
                    btn_recovery_fired = true;
                    btn_long_fired = true;
                    open_recovery_menu();
                }

                if (btn_ticks >= 12 && !btn_long_fired) {
                    btn_long_fired = true;
                    handle_button_long_click();
                }

                if (btn_ticks >= 40 && s_menu_active && !IDLE_RASTER_ACTIVE() &&
                    s_menu_cursor == 1 && !btn_scan_fired) {
                    btn_scan_fired = true;
                    channel_auto_search();
                    s_menu_timeout_ticks = 0;
                }
            } else if (btn_ticks > 0) {
                if (!btn_long_fired && btn_ticks >= 2) {
                    handle_button_short_click();
                }
                btn_ticks = 0;
                btn_long_fired = false;
                btn_scan_fired = false;
                btn_recovery_fired = false;
            }
        }

        phy_rx_lab_poll();
        uint32_t phy_generation = phy_rx_lab_generation();
        if (phy_generation != seen_phy_generation) {
            seen_phy_generation = phy_generation;
            /* Do not reuse AFC/sync observations spanning a compound tune. */
            video_standard_detector_reset();
            s_cfo_khz = 0;
            afc2_ctrl_invalidate(&afc2_ctrl);
            settle_ticks = GAIN_SETTLE_TICKS;
        }
        if (s_menu_active && phy_rx_lab_profile_active()) phy_rx_lab_stock();
        bool menu_was_active = s_menu_active;
        if (seen_profile_generation != s_profile_generation) {
            seen_profile_generation = s_profile_generation;
            settle_ticks = GAIN_SETTLE_TICKS;
            sync_age_ticks = 40;
        }

        if (menu_was_active && !IDLE_RASTER_ACTIVE()) {
            s_menu_timeout_ticks++;
            if (s_menu_timeout_ticks >= 240) {
                settings_save();
                video_set_menu_mode(false);
                printf("[MENU] Inactivity timeout (12s) -> Live Video\n");
            }
        }

        poll_transport_faults();

        /* A complete finished descriptor gives 102.3 us of Q4/I4 rather than
         * the old 6.4 us peek, while averaging only ~82 kB/s of CPU reads. */
        rx_control_epoch_t sample_epoch = {s_profile_generation,
            phy_rx_lab_generation(), s_gain_transition_count};
        unsigned sample_lane = rf_get_iq_lanes();
        uint32_t sampled_context = afc_context((unsigned)s_afc_mode,
            (unsigned)rf_get_channel_index(), s_profile_generation, s_current_bw40,
            rf_get_frequency_offset_khz(), rf_get_arc_generation()) ^
            (sample_epoch.phy * 2654435761u);
        size_t ring_offset = 0;
        if (!copy_completed_rx_window(s_control_sample_buf,
                                     sizeof(s_control_sample_buf), &ring_offset)) {
            afc2_ctrl_invalidate(&afc2_ctrl);
            s_cfo_khz = 0; s_last_sync_quality = 0;
            s_afc_video_locked = afc2_native_lock(s_afc_video_locked, false, false, 0, &afc_lost_windows);
            s_afc_fresh = 0;
            continue;
        }
        control_metrics_t metrics =
            analyze_control_window(s_control_sample_buf,
                                   sizeof(s_control_sample_buf),
                                   ring_offset);

        int p_median = metrics.p_median;
        int q_phase = metrics.q_phase;
        int n_clip = metrics.n_clip;
        int n_origin = metrics.n_origin;
        int clip_permille = metrics.clip_permille;
        int origin_permille = metrics.origin_permille;

        s_last_p_median = p_median;
        s_last_q_phase = q_phase;
        s_last_n_clip = n_clip;
        s_last_n_origin = n_origin;
        s_last_clip_permille = clip_permille;
        s_last_origin_permille = origin_permille;
        s_last_dc_i_x100 = metrics.dc_i_x100;
        s_last_dc_q_x100 = metrics.dc_q_x100;
        s_last_iq_skew_permille = metrics.iq_skew_permille;
        s_last_iq_cross_permille = metrics.iq_cross_permille;
        s_last_winding_permille = metrics.winding_permille;
        s_last_strong_winding_permille = metrics.strong_winding_permille;

        int instant_strength = signal_strength_score(&metrics, s_current_gain);
        s_signal_strength = (s_signal_strength * 3 + instant_strength + 2) / 4;

        if (menu_was_active) {
            if (++menu_refresh_ticks >= 5) {
                menu_refresh_ticks = 0;
                menu_render_menu();
            }
            /* The menu raster has its own TX DMA chain, while PARLIO RX keeps
             * filling the raw IQ ring.  Keep the receive controller running
             * so Gxx and the signal indication reflect the selected channel
             * instead of freezing at the value from when the menu opened. */
        } else {
            menu_refresh_ticks = 0;
        }

        /* Issue #28 classifier: if raw carrier coherence collapses within
         * 200 ms of a new gain state while no transport fault is required to
         * explain it, count the transition once. This is evidence for an
         * RF/PHY transient, not proof of a DMA stall. */
        if (s_gain_transition_count != s_last_gain_drop_transition &&
            s_last_gain_write_us > 0) {
            int64_t gain_age = esp_timer_get_time() - s_last_gain_write_us;
            if (gain_age >= 0 && gain_age <= 200000 &&
                q_phase < 25 && p_median < 12) {
                ++s_hw_counters.gain_quality_drop_count;
                s_last_gain_drop_transition = s_gain_transition_count;
            }
        }

        int64_t control_now_us = esp_timer_get_time();
        rx_control_epoch_t afc_epoch = {s_profile_generation,
            phy_rx_lab_generation(), s_gain_transition_count};
        uint32_t afc_ctx = afc_context((unsigned)s_afc_mode,
            (unsigned)rf_get_channel_index(), s_profile_generation, s_current_bw40,
            rf_get_frequency_offset_khz(), rf_get_arc_generation()) ^
            (afc_epoch.phy * 2654435761u);
        if (afc2_ctrl_sync(&afc2_ctrl, afc_ctx, afc2_own_write)) {
            s_afc_video_locked = false; afc_lost_windows = 0;
        }
        afc2_own_write = false;
        bool afc_window_ok = settle_ticks == 0 && sampled_context == afc_ctx &&
            rx_control_epoch_equal(sample_epoch, afc_epoch) &&
            sample_lane == rf_get_iq_lanes() &&
            control_now_us - s_last_phy_write_us >= 100000 &&
            c5vrx4_lane_window_ready((uint64_t)control_now_us) &&
            afc2_envelope_stationary(s_control_sample_buf, sizeof(s_control_sample_buf));
        afc2_result_t afc2 = {0};
        if (afc_window_ok) {
            afc2 = afc2_measure(s_control_sample_buf, sizeof(s_control_sample_buf),
                               c5vrx_phase8_gain_lut);
            afc2_ctrl_observe(&afc2_ctrl, &afc2);
        } else afc2_ctrl_invalidate(&afc2_ctrl);
        s_cfo_khz = afc2.porch_pairs ? afc2.porch_khz : 0;
        bool was_afc_locked = s_afc_video_locked;
        bool afc_valid = afc_window_ok && afc2.lines && afc2.standard &&
            afc2.porch_pairs && afc2.sync_pairs && afc2.burst_x10 >= AFC2_BURST_MIN_X10;
        s_afc_video_locked = afc2_native_lock(s_afc_video_locked, afc_valid,
            afc2_ctrl_can_lock(&afc2_ctrl, s_afc_mode == AFC_MODE_AUTO),
            afc2_ctrl.n, &afc_lost_windows);
        if (was_afc_locked && !s_afc_video_locked)
            afc2_ctrl_reset(&afc2_ctrl, afc_ctx, true);
        s_afc_fresh = afc2_ctrl.n;
        s_afc_corrections = afc2_ctrl.corrections;

        if (settle_ticks > 0) --settle_ticks;
        int sync_quality = 0;
        bool fresh_sync = false;
        if (settle_ticks == 0 && rx_control_epoch_equal(sample_epoch, afc_epoch) &&
            sample_lane == rf_get_iq_lanes()) {
            sync_quality = video_semantic_observe(s_control_sample_buf,
                                                  sizeof(s_control_sample_buf),
                                                  ring_offset);
            fresh_sync = sync_quality >= 70;
        }
        if (fresh_sync) sync_age_ticks = 0;
        else if (sync_age_ticks < 100) ++sync_age_ticks;
        /* The idle raster blanks video, so a fringe sync counts as a
         * transmitter there (IDLE_RASTER_SYNC_Q), not only a clean one. */
        static unsigned idle_sync_age = 100u;
        const bool idle_sync = sync_quality >= IDLE_RASTER_SYNC_Q;
        if (idle_sync) { idle_sync_age = 0; s_last_idle_sync_us = esp_timer_get_time(); }
        else if (idle_sync_age < 100u) ++idle_sync_age;
        idle_raster_service(q_phase, idle_sync, idle_sync_age);

        if (s_afc_mode == AFC_MODE_AUTO) {
            /* Burst-confirmed video AFC TRACK never retunes. Gain HOLD is
             * independent: an annulus alone must not prevent acquisition. */
            bool eligible = !s_afc_video_locked && afc_window_ok &&
                q_phase >= 55 && !s_menu_active &&
                rx_control_epoch_equal(afc_epoch, (rx_control_epoch_t){
                    s_profile_generation, phy_rx_lab_generation(), s_gain_transition_count});
            int32_t step = 0;
            if (eligible && phy_rx_lab_try_actuator(afc_epoch.phy)) {
                /* A gain/profile writer may have won ownership between the
                 * pre-check and this acquire. Re-check while excluding it. */
                bool current = s_afc_mode == AFC_MODE_AUTO && !s_afc_video_locked &&
                    rx_control_epoch_equal(afc_epoch, (rx_control_epoch_t){
                        s_profile_generation, phy_rx_lab_generation(), s_gain_transition_count});
                if (afc2_ctrl_decide(&afc2_ctrl, current, &step)) {
                    apply_frequency_offset_khz_tracked(rf_get_frequency_offset_khz() + (int)step);
                    afc2_own_write = true;
                    settle_ticks = 2;
                }
                phy_rx_lab_end_actuator();
            }
        } else if (s_afc_mode == AFC_MODE_OFF && rf_get_frequency_offset_khz() != 0) {
            apply_frequency_offset_khz_tracked(0);
        }

        bool is_locked = s_agc_state == AGC_STATE_TRACK && q_phase >= 55;
        if (is_locked && !was_locked && !s_lab_quiet) {
            const fpv_channel_t *ch = rf_get_current_channel();
            printf("[CARRIER] Locked on %s (%u MHz) in %s (P=%d Q=%d%% G=%u)\n",
                   ch->name, ch->freq_mhz, rf_get_band_name(rf_get_current_band()),
                   p_median, q_phase, s_current_gain);
        }
        was_locked = is_locked;

    }
}
