/* C5VRX-4: video responsibilities. */
#include "video_internal.h"

#define C5V4_LEVEL_TASK_ENABLED 0

static esp_timer_handle_t s_v3_sentinel_timer;

/* Frozen stride-3 Phase8/winding estimate: ~63 unique samples per H-sync,
 * ~847.4/853.3 per NTSC/PAL line. Exact live alignment/history is not tagged.
 * Standard voting retains its historical period-in-20M-units interface. */
/* c5v4_cvbs_analyze has one static workspace (no 9.7 KiB frame per task):
 * analog_agc, cvbs_level and the J capture take turns. Created in
 * video_start before any of them, without heap. */
static StaticSemaphore_t s_cvbs_analyze_lock_buf;

/* ----- Main entry point ----- */

esp_err_t video_start(void)
{
    /* Keep output unbuffered on the no-driver USB VFS. Input is drained directly
     * by console_diag_task; USB is never involved in DMA sample pacing. */
    setvbuf(stdin, NULL, _IONBF, 0);
    setvbuf(stdout, NULL, _IONBF, 0);
    int flags = fcntl(fileno(stdin), F_GETFL, 0);
    fcntl(fileno(stdin), F_SETFL, flags | O_NONBLOCK);
    flags = fcntl(fileno(stdout), F_GETFL, 0);
    fcntl(fileno(stdout), F_SETFL, flags | O_NONBLOCK);
    usb_serial_jtag_vfs_use_nonblocking();

    /* Initialize BOOT button on GPIO 28 */
    init_boot_button();

    /* Menu control is serialized with BOOT handling in the AGC task. */
    s_cvbs_analyze_lock = xSemaphoreCreateMutexStatic(&s_cvbs_analyze_lock_buf);
    s_menu_commands = xQueueCreate(16, sizeof(int));
    if (!s_menu_commands) return ESP_ERR_NO_MEM;

    settings_load();
    c5vrx4_options_snapshot();
    s_dg3_saved_valid = c5vrx4_blob_load("dg3_map", &s_dg3_saved, sizeof(s_dg3_saved)) &&
                        s_dg3_saved.version == DG3_MAP_VERSION &&
                        s_dg3_saved.freq_mhz == rf_get_frequency_mhz() &&
                        s_dg3_saved.lane_mode == c5vrx4_fixed_lane();
    printf("DG3_MAP loaded=%u\n", s_dg3_saved_valid);
    const rf_bw_mode_t boot_bw_mode = s_rf_bw_mode;
    apply_rx_profile(s_rx_profile);
    if (s_rx_profile == RX_PROFILE_DIRECT_GAIN && boot_bw_mode != s_rf_bw_mode) {
        s_rf_bw_mode = boot_bw_mode;
        apply_rf_bandwidth(boot_bw_mode != RF_BW_MODE_BW20);
    }
    if (rf_native_agc_active()) {
        rf_native_agc_state_t native;
        rf_get_native_agc_state(&native);
        ESP_LOGW(TAG, "NATIVE HW AGC (opt-in): vendor AGC never disabled, "
                 "firmware gain writes blocked (gain_reg=0x%08lx agc_reg=0x%08lx). "
                 "'E' = P8ENV row, 'N' = reboot to Direct Gain V5",
                 (unsigned long)native.gain_status_reg,
                 (unsigned long)native.agc_ctrl_reg);
    }

    /* Select the forced lane before capture starts; the fixed comparison
     * never switches geometry inside a live line or at a DMA boundary. */
    rf_set_iq_lanes(0u);
    /* Zero the ring before starting. Flush to DMA-visible SRAM. */
    memset(s_raw_ring, 0, sizeof(s_raw_ring));
    sync_dma_c2m(s_raw_ring, sizeof(s_raw_ring));

    esp_err_t err;

    if ((err = prepare_rx()) != ESP_OK) return err;
    if ((err = prepare_tx()) != ESP_OK) return err;

    start_flight_demodulator();

    /* Start RX cyclic ring. GDMA begins writing at s_raw_ring[0]. */
    if ((err = start_rx()) != ESP_OK) return err;

    /* Put PARLIO RX into pure continuous hardware mode:
     * 1. Disable all GDMA RX channel interrupts so the CPU is never interrupted
     *    (~9,775 ISRs/sec eliminated!).
     * 2. Set rx_eof_gen_sel = 1 (external enable, non-existent in soft mode)
     *    so PARLIO RX never generates an EOF stall event.
     * This matches PARLIO TX's unbroken hardware loop, eliminating pointer drift! */
    AHB_DMA.in_intr[0].ena.val = 0;
    AHB_DMA.in_intr[1].ena.val = 0;
    AHB_DMA.in_intr[2].ena.val = 0;
    PARL_IO.rx_genrl_cfg.rx_eof_gen_sel = 1;

    /* Request half-ring producer/consumer separation before starting TX.
     * The integer-microsecond delay and driver latency need hardware validation. */
    esp_rom_delay_us((RAW_RING_BYTES / 2ULL) * 1000000ULL / IQ_RATE_HZ);

    if ((err = start_tx()) != ESP_OK) return err;

    /* Discover AHB_DMA channels assigned to PARL_IO (peripheral ID 9) */
    for (int i = 0; i < 3; i++) {
        if (AHB_DMA.channel[i].in.in_peri_sel.peri_in_sel_chn == 9) {
            s_rx_dma_ch = i;
        }
        if (AHB_DMA.channel[i].out.out_peri_sel.peri_out_sel_chn == 9) {
            s_tx_dma_ch = i;
        }
    }

    /* Put PARLIO TX into pure continuous hardware mode:
     * Disable all GDMA TX channel interrupts and PARL_IO core interrupts.
     * Prevents PARLIO_LL_EVENT_TX_FIFO_EMPTY and EOF interrupts from stealing CPU cycles! */
    AHB_DMA.out_intr[0].ena.val = 0;
    AHB_DMA.out_intr[1].ena.val = 0;
    AHB_DMA.out_intr[2].ena.val = 0;
    PARL_IO.int_ena.val = 0;

    /* Clear suc_eof on ALL GDMA descriptors for both RX and TX to eliminate
     * hardware wrap EOF bubbles completely! The buffer becomes a truly infinite ring. */
    int rx_nodes = patch_descriptors_clear_eof(s_rx_dma_ch, true);
    int tx_nodes = patch_descriptors_clear_eof(s_tx_dma_ch, false);

    /* Issue #28: clear stale startup/driver status once. Subsequent sticky
     * faults are observed by poll_transport_faults() without enabling IRQs. */
    PARL_IO.int_clr.val = UINT32_MAX;
    if (s_rx_dma_ch >= 0) AHB_DMA.in_intr[s_rx_dma_ch].clr.val = UINT32_MAX;
    if (s_tx_dma_ch >= 0) AHB_DMA.out_intr[s_tx_dma_ch].clr.val = UINT32_MAX;
    BITSCRAMBLER.state[BITSCRAMBLER_DIR_TX].val = 1u << 31;

    ESP_ERROR_CHECK(xTaskCreate(direct_gain_v3_observer_task, "gain_v3_obs",
                                4096, NULL, 3,
                                &s_v3_observer_task_handle) == pdPASS ?
                    ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(xTaskCreate(direct_gain_v3_sentinel_task, "gain_v3_fast",
                                3072, NULL, 4,
                                &s_v3_sentinel_task_handle) == pdPASS ?
                    ESP_OK : ESP_ERR_NO_MEM);
    /* Priority 4: its data expires ~0.5 ms after RX writes it, so it must
     * run on time (board 2026-10-06: at priority 2 the analog AGC task held
     * it off for up to 90 ms and it never locked). Its CPU share is bounded
     * by its per-run budget (50 us per 200 us), not by its priority. */
    if (c5vrx4_sync_flywheel_enabled())
        ESP_ERROR_CHECK(xTaskCreate(sync_flywheel_task, "sync_fw", 3072, NULL, 4,
                                    &s_sfw_task_handle) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
    const esp_timer_create_args_t v3_timer_args = {
        .callback = direct_gain_v3_sentinel_timer_cb,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "dg3_fast",
    };
    ESP_ERROR_CHECK(esp_timer_create(&v3_timer_args,
                                     &s_v3_sentinel_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(s_v3_sentinel_timer, 200));

    /* Start interactive console for on-demand diagnostics (zero periodic CPU/bus traffic) */
    /* 6 KiB: the P8ENV printf takes ~90 arguments (3 KiB overflowed). */
    xTaskCreate(console_diag_task, "console_diag", 6144, NULL, 1, NULL);
    video_backpack_start();

    /* Start dedicated Analog Video AGC engine (slow physical actuator). */
    /* The slow task now also calls the stride-3 diagnostic, whose workspace
     * is static (cvbs_analyze_locked). -fcallgraph-info worst case 6288 B;
     * 9 KiB leaves room for printf. Keep it outside the sample-paced path. */
    if (xTaskCreate(analog_agc_task, "analog_agc", 9216, NULL, 3, NULL) != pdPASS)
        return ESP_ERR_NO_MEM;

    /* The mask decision is latched per boot; every CPU snapshot observer
     * (sync/standard, AFC, level servo, J) decodes the same Q3 as the program. */
    c5v4_cvbs_set_mask_decode(c5vrx4_agc_mask_active());
    /* The level servo's live LUT writes are refused (unreliable live LUT
     * access, board 2026-10-06), so its 8 KiB analysis every 5-20 ms was
     * pure CPU load - part of the starvation that froze the USB console.
     * The task is not started until a safe update path exists. */
    if (C5V4_LEVEL_TASK_ENABLED && c5vrx4_level_enabled()) {
        /* CPU-only copy: LP RAM first, so DMA-capable RAM stays free for the
         * menu descriptors (allocated when the menu opens). */
        uint8_t *level_raw = heap_caps_malloc(C5V4_LEVEL_SAMPLE_BYTES, MALLOC_CAP_RTCRAM);
        if (!level_raw) level_raw = malloc(C5V4_LEVEL_SAMPLE_BYTES);
        if (!level_raw) return ESP_ERR_NO_MEM;
        /* 4 KiB: -fcallgraph-info worst case 1712 B now that the analyzer
         * workspace is static; no printf on this path. 'T' prints the
         * high-water mark. */
        if (xTaskCreate(cvbs_level_task, "cvbs_level", 4096, level_raw, 2, &s_level_task) != pdPASS) {
            free(level_raw); return ESP_ERR_NO_MEM;
        }
    }
    if (xTaskCreate(predemod_task, "predemod", 4096, NULL, 2, NULL) != pdPASS)
        return ESP_ERR_NO_MEM;
    /* Boot RAM margin, so a feature that eats it shows here before it
     * turns into ESP_ERR_NO_MEM. */
    printf("C5V4_HEAP after_video_start free=%u largest=%u dma_free=%u dma_largest=%u\n",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));

    /* Print startup stamp (visible on serial monitor at boot). */
    ESP_EARLY_LOGW(TAG, "C5VRX-4 UNWRAP/75: IQ40M -> %s -> DAC13.333M "
                   "[D,D,D]@40M gain_owner=%s; descriptors RX=%d TX=%d; "
                   "experimental, no range claim",
                   c5vrx4_history_enabled() ? "Unwrap8 HISTORY" : "Unwrap8 STATIC",
                   rf_native_agc_active() ? "NATIVE" : rx_profile_name(),
                   rx_nodes, tx_nodes);

    return ESP_OK;
}
