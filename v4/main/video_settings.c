/* C5VRX-4: settings responsibilities. */
#include "video_internal.h"

static uint8_t profile_gain_min(void);
static uint8_t profile_gain_clamp(int gain);
static uint8_t apply_rx_gain_tracked(uint8_t gain);
static void arm_native_agc_and_reboot(bool enable);
#define SETTINGS_KEY "settings"

#define SETTINGS_NAMESPACE "c5vrx4"

#define SETTINGS_VERSION 4u

typedef struct {
    uint8_t version;
    uint8_t channel_index;
    uint8_t rf_bw_mode;
    uint8_t afc_mode;
    uint8_t output_mode;
    uint8_t video_std_mode;
    uint8_t agc_mode;
    uint8_t manual_gain;
    int16_t frequency_offset_khz;
    uint8_t menu_boot_btn_enabled;
    uint8_t rx_profile;
    uint8_t demod_mode;
    uint8_t bw_afc_persist; /* validates persisted BW/AFC choices */
} persisted_settings_t;

_Static_assert(sizeof(persisted_settings_t) == 14u,
               "settings v3/v4 migration layout changed");

volatile hw_transport_counters_t s_hw_counters;

lag_event_t s_lag_events[LAG_EVENT_LOG_SIZE];

volatile uint32_t s_lag_event_head;

volatile int64_t s_last_gain_write_us;

volatile int64_t s_last_phy_write_us;

volatile uint8_t s_last_phy_write_kind;

volatile int64_t s_last_transport_event_us;

volatile uint32_t s_last_transport_flags;

volatile uint32_t s_last_gain_drop_transition;

volatile int64_t s_last_user_lag_mark_us;

volatile video_standard_mode_t s_video_std_mode = VIDEO_STD_MODE_AUTO;

volatile video_standard_t s_video_std = VIDEO_STD_NTSC;

volatile video_standard_t s_detected_video_std = VIDEO_STD_NTSC;

volatile bool s_detected_video_std_valid;

volatile uint8_t s_video_std_pal_score;

volatile uint8_t s_video_std_ntsc_score;

volatile uint16_t s_last_line_period_20m;

volatile uint16_t s_last_sync_width_20m;

volatile int s_last_sync_quality;

TaskHandle_t s_level_task;

unsigned s_level_work_us;

TaskHandle_t s_sfw_task_handle;

/* V5's measured gain map, persisted (NVS c5vrx4/dg3_map): after a reset or
 * a reboot V5 starts from what this board measured instead of the tuple
 * model, so it does not explore unknown steps (each a little grain) again. */
dg3_map_blob_t s_dg3_saved;

bool s_dg3_saved_valid;

uint32_t s_dg3_map_imports, s_dg3_map_saves;

volatile rf_bw_mode_t s_rf_bw_mode = RF_BW_MODE_AUTO;

volatile bool s_current_bw40 = true;

volatile demod_mode_t s_demod_mode = DEMOD_MODE_GOLDEN_PHASE5;

volatile uint32_t s_gain_transition_count = 0;

volatile dg3_state_t s_last_direct_gain_state = DG3_ACQUIRE;

volatile uint8_t s_last_direct_gain_target;

volatile int s_last_direct_gain_delta;

volatile uint32_t s_last_direct_gain_total_writes;

volatile uint32_t s_last_direct_gain_hold_cycles;

volatile uint32_t s_profile_generation;

const char *TAG = "c5vrx4_video";

SemaphoreHandle_t s_cvbs_analyze_lock;

volatile analog_agc_mode_t s_agc_mode = ANALOG_AGC_ACTIVE;

/* Firmware AGC mode to persist while the native experiment forces MANUAL. */
static analog_agc_mode_t s_agc_mode_before_native = ANALOG_AGC_ACTIVE;

volatile agc_state_t s_agc_state = AGC_STATE_SEARCH;

/* WBFM instantaneous phase slope contains the video modulation itself.
 * The short-window CFO estimator is useful diagnostics, but it is not yet a
 * calibrated LO-error estimator. Never retune automatically at boot. */
volatile afc_mode_t s_afc_mode = AFC_MODE_OFF;

volatile bool s_afc_video_locked;

volatile unsigned s_afc_fresh, s_afc_corrections;

volatile uint8_t s_current_gain = 62u;

/* Physical RF gain applied */
volatile uint8_t s_shadow_gain = 62u;

/* Controller recommended gain */
volatile int s_last_p_median = 25;

volatile int s_last_q_phase = 0;

volatile int s_signal_strength = 0;

volatile int s_last_clip_permille = 0;

volatile int s_last_origin_permille = 0;

volatile int s_last_dc_i_x100 = 0;

volatile int s_last_dc_q_x100 = 0;

volatile int s_last_iq_skew_permille = 0;

volatile int s_last_iq_cross_permille = 0;

volatile int s_last_winding_permille = 0;

volatile int s_last_strong_winding_permille = 0;

volatile int s_cfo_khz = 0;

/* Carrier Frequency Offset in kHz */
volatile bool s_channel_scan_active;

volatile unsigned s_channel_scan_progress;

/* Issue #27/#28 lab characterization is deliberately console-driven and
 * opt-in. It never paces the realtime IQ/CVBS path and adds no periodic task. */
volatile bool s_lab_quiet;

volatile bool s_pre_q4_probe_active;

volatile bool s_rssi_probe_active;

/* ---- Native AGC acquisition witness (calibration) ------------------------
 * The C5 packet AGC re-acquires every ~25-50 us on a continuous carrier; each
 * ~2-3 us gain walk produces saturated or starved IQ (native-agc-v2.md). The
 * RF dump word on MODEM_DIAG carries the gain index (DIAG[20..27]) and the
 * AGC state (DIAG[28..31]). With native AGC running, the eight PARLIO lanes
 * capture DIAG[20..26] plus one state bit per pass; gain changes mark the
 * acquisitions and the state bit that separates them becomes the hold flag
 * on data bit 0 for the masked program. Live video is garbage for ~50 ms. */
/* 24 per bit: with the native restart patch walks are rare (5 in 24 windows
 * on the board, 2026-10-06) and the choice needs >= 8 acquisitions. */

const char *s_witness_result = "never";

agc_witness_result_t s_witness_last = {.bit = -1};

unsigned s_witness_runs;

volatile uint32_t s_cvbs_capture_running;

const char *rx_profile_name(void)
{
    return rf_native_agc_active() ? "NATIVE HW AGC" : "DIRECT GAIN V5";
}

static uint8_t profile_gain_min(void) { return 2u; }

uint8_t profile_gain_max(void) { return rf_get_arc_gain_table()->max_index; }

static uint8_t profile_gain_clamp(int gain)
{
    int lo = profile_gain_min();
    int hi = profile_gain_max();
    if (gain < lo) gain = lo;
    if (gain > hi) gain = hi;
    return (uint8_t)gain;
}

const char *rf_bw_mode_name(void)
{
    return s_rf_bw_mode == RF_BW_MODE_BW40 ? "BW40" :
           s_rf_bw_mode == RF_BW_MODE_BW20 ? "BW20" : "AUTO EXP";
}

const char *output_mode_name(void) { return "6BIT@40"; }

const char *demod_mode_name(void)
{
    if (c5vrx4_reference_demod()) return c5vrx4_demodulator_name();
    return c5vrx4_history_enabled() ? "C5V4 U8HC75" : "C5V4 U8S75";
}

void apply_rf_bandwidth(bool bw40)
{
    s_last_phy_write_us = esp_timer_get_time();
    s_last_phy_write_kind = PHY_WRITE_BW;
    s_current_bw40 = bw40;
    rf_set_analog_bandwidth(bw40);
}

/* Fixed analog BW calibrated: the V5 gear then moves between the normal
 * code and the measured edge profile. */
bool bw_fixed_calibrated(void)
{
    return c5vrx4_fixed_bw_enabled() && c5vrx4_bw_code() != C5VRX4_BW_UNCALIBRATED;
}

void bw_set_edge(bool edge)
{
    s_last_phy_write_us = esp_timer_get_time();
    s_last_phy_write_kind = PHY_WRITE_BW;
    s_current_bw40 = !(edge && c5vrx4_bw_edge_digital());
    rf_set_fixed_bw_edge(edge);
}

void cycle_rf_bandwidth_mode(void)
{
    if (rf_fixed_bw_edge_active()) bw_set_edge(false);
    if (s_rf_bw_mode == RF_BW_MODE_BW40) {
        s_rf_bw_mode = RF_BW_MODE_BW20;
        apply_rf_bandwidth(false);
    } else if (s_rf_bw_mode == RF_BW_MODE_BW20) {
        s_rf_bw_mode = RF_BW_MODE_AUTO;
        apply_rf_bandwidth(true); /* AUTO always enters in high gear. */
    } else {
        s_rf_bw_mode = RF_BW_MODE_AUTO;
        apply_rf_bandwidth(true);
    }
}

/* Frequency-offset writes in rf.c re-assert the current forced RX gain after
 * touching the PHY channel offset. Track that hidden gain write so issue #28
 * correlation does not incorrectly call an AFC/fine-tune transient unrelated. */
void apply_frequency_offset_khz_tracked(int offset_khz)
{
    int64_t now = esp_timer_get_time();
    s_last_gain_write_us = now; /* rf.c re-asserts the forced RX gain */
    s_last_phy_write_us = now;
    s_last_phy_write_kind = PHY_WRITE_OFFSET;
    rf_set_frequency_offset_khz(offset_khz);
    ++s_gain_transition_count;
}

void step_frequency_offset_khz_tracked(int delta_khz)
{
    apply_frequency_offset_khz_tracked(rf_get_frequency_offset_khz() + delta_khz);
}

uint8_t apply_rx_gain_for_generation(uint8_t gain, uint32_t phy_generation)
{
    /* Native AGC experiment: the vendor loop owns gain; keep state unchanged. */
    if (rf_native_agc_active()) return s_current_gain;
    gain = profile_gain_clamp(gain);
    s_shadow_gain = gain;

    uint8_t next_gain = gain;
    if (next_gain == s_current_gain) return s_current_gain;
    /* A busy or changed PHY must not leave software ahead of hardware. */
    if (!rf_try_set_rx_gain(true, next_gain, phy_generation)) return s_current_gain;
    s_current_gain = next_gain;
    s_last_gain_write_us = esp_timer_get_time();
    s_last_phy_write_us = s_last_gain_write_us;
    s_last_phy_write_kind = PHY_WRITE_GAIN;
    ++s_gain_transition_count;
    return next_gain;
}

static uint8_t apply_rx_gain_tracked(uint8_t gain)
{
    return apply_rx_gain_for_generation(gain, phy_rx_lab_generation());
}

void settings_save(void)
{
    persisted_settings_t settings = {
        .version = SETTINGS_VERSION,
        .channel_index = (uint8_t)rf_get_channel_index(),
        .rf_bw_mode = (uint8_t)s_rf_bw_mode,
        .afc_mode = (uint8_t)s_afc_mode,
        .output_mode = 0,
        .video_std_mode = (uint8_t)s_video_std_mode,
        .agc_mode = (uint8_t)(rf_native_agc_active() ?
                              s_agc_mode_before_native : s_agc_mode),
        .manual_gain = s_current_gain,
        .frequency_offset_khz = (int16_t)rf_get_frequency_offset_khz(),
        .menu_boot_btn_enabled = s_menu_boot_btn_enabled ? 1u : 0u,
        .rx_profile = (uint8_t)s_rx_profile,
        .demod_mode = (uint8_t)s_demod_mode,
        .bw_afc_persist = 1u,
    };
    nvs_handle_t handle;
    esp_err_t err = nvs_open(SETTINGS_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        err = nvs_set_blob(handle, SETTINGS_KEY, &settings, sizeof(settings));
        if (err == ESP_OK) err = nvs_commit(handle);
        nvs_close(handle);
    }
    if (err != ESP_OK) ESP_LOGW(TAG, "Could not save settings: %s", esp_err_to_name(err));
}

void settings_load(void)
{
    persisted_settings_t settings = {0};
    size_t length = sizeof(settings);
    nvs_handle_t handle;
    esp_err_t err = nvs_open(SETTINGS_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_OK) {
        err = nvs_get_blob(handle, SETTINGS_KEY, &settings, &length);
        nvs_close(handle);
    }
    bool legacy_v3 = settings.version == 3u;
    if (err != ESP_OK || length != sizeof(settings) ||
        (!legacy_v3 && settings.version != SETTINGS_VERSION)) {
        s_rx_profile = RX_PROFILE_DIRECT_GAIN; /* legacy: s_rx_profile = RX_PROFILE_ARC */
        s_demod_mode = DEMOD_MODE_GOLDEN_PHASE5;
        s_rf_bw_mode = RF_BW_MODE_AUTO; /* V5 gear: starts BW40 */
        s_afc_mode = AFC_MODE_OFF;
        s_agc_mode = ANALOG_AGC_ACTIVE;
        s_current_gain = rf_get_arc_survival_gain();
        s_shadow_gain = s_current_gain;
        apply_rf_bandwidth(true);
        rf_set_rx_gain(true, s_current_gain);
        return;
    }

    if (settings.channel_index < rf_get_channel_count()) (void)rf_set_channel(settings.channel_index);
    /* Board 2026-10-06: an old record's BW40 switched V5's edge gear off
     * (gear=manual) once the menu choice started to persist. */
    if (settings.bw_afc_persist == 1u && settings.rf_bw_mode <= RF_BW_MODE_AUTO)
        s_rf_bw_mode = (rf_bw_mode_t)settings.rf_bw_mode;
    apply_rf_bandwidth(s_rf_bw_mode != RF_BW_MODE_BW20);
    if (settings.bw_afc_persist == 1u && settings.afc_mode <= AFC_MODE_OFF)
        s_afc_mode = (afc_mode_t)settings.afc_mode;
    /* v3 used this exact byte as zero-initialized reserved storage, so old
     * Range-v2 settings migrate losslessly with the proven GOLDEN demod. */
    s_demod_mode = DEMOD_MODE_GOLDEN_PHASE5;
    if (settings.video_std_mode <= VIDEO_STD_MODE_PAL) s_video_std_mode = (video_standard_mode_t)settings.video_std_mode;
    s_rx_profile = RX_PROFILE_DIRECT_GAIN;
    /* The menu's DIGITAL BW and AFC choices persist (menu audit
     * 2026-10-06: both were saved and then reset here on every boot). The
     * defaults above (no/old settings) stay AUTO and OFF. */
    if (s_video_std_mode == VIDEO_STD_MODE_PAL) s_video_std = VIDEO_STD_PAL;
    else if (s_video_std_mode == VIDEO_STD_MODE_NTSC) s_video_std = VIDEO_STD_NTSC;
    if (settings.agc_mode <= ANALOG_AGC_MANUAL) s_agc_mode = (analog_agc_mode_t)settings.agc_mode;
    if (s_agc_mode == ANALOG_AGC_MANUAL && settings.manual_gain >= 2u &&
        settings.manual_gain <= rf_get_arc_gain_table()->max_index) {
        s_current_gain = profile_gain_clamp(settings.manual_gain);
    } else {
        s_current_gain = rf_get_arc_survival_gain();
    }
    s_shadow_gain = s_current_gain;
    rf_set_rx_gain(true, s_current_gain);
    s_menu_boot_btn_enabled = settings.menu_boot_btn_enabled != 0;
    if (s_afc_mode == AFC_MODE_HOLD) apply_frequency_offset_khz_tracked(settings.frequency_offset_khz);
    else if (s_afc_mode == AFC_MODE_OFF) apply_frequency_offset_khz_tracked(0);
    ESP_LOGI(TAG, "Restored settings%s: channel=%u profile=%s BW=%s output=%s demod=%s",
             legacy_v3 ? " (v3 migrated)" : "",
             settings.channel_index, rx_profile_name(), rf_bw_mode_name(),
             output_mode_name(), demod_mode_name());
}

void apply_rx_profile(rx_profile_t profile)
{
    (void)profile; /* persisted C5VRX-3 profiles migrate to V5 */
    s_rx_profile = RX_PROFILE_DIRECT_GAIN;
    ++s_profile_generation;
    s_agc_state = AGC_STATE_SEARCH;
    s_agc_mode = ANALOG_AGC_ACTIVE;
    s_rf_bw_mode = RF_BW_MODE_AUTO;
    apply_rf_bandwidth(true);
    s_afc_mode = AFC_MODE_OFF;
    if (rf_get_frequency_offset_khz() != 0) apply_frequency_offset_khz_tracked(0);
    apply_rx_gain_tracked(rf_get_arc_survival_gain());
    if (rf_native_agc_active()) {
        s_agc_mode_before_native = s_agc_mode;
        s_agc_mode = ANALOG_AGC_MANUAL;
    }
    video_standard_detector_reset();
}

static void arm_native_agc_and_reboot(bool enable)
{
    settings_save();
    esp_err_t err = rf_request_native_agc_boot(enable);
    printf("[RX PROFILE] -> %s on reboot err=%s\n",
           enable ? "NATIVE HW AGC" : "DIRECT GAIN V5", esp_err_to_name(err));
    if (err != ESP_OK) return;
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(150));
    esp_restart();
}

void cycle_rx_profile(void)
{
    arm_native_agc_and_reboot(!rf_native_agc_active());
}

void leave_experimental_profile(void)
{
    ++s_profile_generation;
}
