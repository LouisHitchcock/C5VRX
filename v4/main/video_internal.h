/* Private video subsystem contract. Hardware remains owned by transport;
 * mutable state is shared only where the existing task handoffs require it.
 * Application code must use video.h, not this header. */
#pragma once

#include "video.h"
#include "c5vrx4.h"
#include "cvbs_monitor.h"
#include "cvbs_level_hw.h"
#include "cvbs_level.h"
#include "cvbs_snapshot.h"
#include "predemod.h"
#include "rx_recal.h"
#include "driver/temperature_sensor.h"
#include "agc_witness.h"
#include "idle_raster.h"
#include "sync_flywheel.h"
#include "rf.h"
#include "phy_rx_lab.h"
#include "rx_control_epoch.h"
#include "menu_raster.h"
#include "demod_quality.h"
#include "fusion_receiver.h"
#include "direct_gain_v3.h"
#include "analog_video_detect.h"
#include "phase8_envelope.h"
#include "afc_state.h"
#include "afc_v2.h"
#include "afc_v2_ctrl.h"
#include "hal/parlio_ll.h"
#include "hal/usb_serial_jtag_ll.h"
#include <stdint.h>
#include <stdlib.h>
#include <inttypes.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include "driver/usb_serial_jtag_vfs.h"
#include "driver/bitscrambler.h"
#include "driver/gpio.h"
#include "driver/parlio_rx.h"
#include "driver/parlio_tx.h"
#include "esp_attr.h"
#include "esp_cache.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "nvs.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "soc/parl_io_struct.h"
#include "soc/bitscrambler_struct.h"
#include "soc/ahb_dma_struct.h"
#include "soc/pcr_struct.h"
#include "hal/misc.h"
#include "modem/modem_syscon_reg.h"
#include "hal/dma_types.h"
#include "esp_clock_output.h"

#define LAG_EVENT_LOG_SIZE 12u
#define IQ_RATE_HZ       40000000u   /* MODEM_DIAG / PARLIO RX clock */
#define DAC_RATE_HZ      40000000u   /* default 6-bit PARLIO TX clock */
#define RAW_RING_BYTES   32768u      /* 32 KiB cyclic ring; Golden Phase5 datapath */
#define DAC_IDLE_CODE    20u         /* Black/blanking pedestal; sync is 0 */
#define MENU_RUNTIME_ENABLED 1       /* Native CVBS menu enabled after geometry rework */
#define CONTROL_SAMPLE_BYTES 4092u  /* one complete, already-finished GDMA descriptor */
#define MENU_NODE_CHUNK  128u   /* 128 x 12 B = 1,536 B, a cache-line multiple */
#define IDLE_RASTER_ACTIVE() (s_idle.active)
#define MAX_RING_DESCRIPTORS 16
#define RX_PROBE_REGION_BYTES 64u
#define RX_PROBE_REGIONS      4u

/* C5VRX-4: raw IQ40M -> three-bundle Phase8 -> [D,D,D] DAC40M.
 * CPU tasks observe completed snapshots; DMA/BitScrambler own pacing. */

/* Hardware diagnostic counters in IRAM to measure transport hiccups without printf spam */
typedef struct {
    uint32_t parl_rx_wovf_count;
    uint32_t parl_tx_rempty_count;
    uint32_t parl_tx_eof_count;
    uint32_t gdma_in_fault_count;
    uint32_t gdma_out_fault_count;
    uint32_t bs_fifo_empty_count;
    uint32_t bs_eof_overload_count;
    uint32_t lag_event_count;
    uint32_t near_gain_event_count;
    uint32_t near_phy_event_count;
    uint32_t gain_quality_drop_count;
    uint32_t user_lag_mark_count;
    uint32_t checks;
} hw_transport_counters_t;

enum {
    LAG_EVT_PARLIO_TX_REMPTY = 1u << 0,
    LAG_EVT_PARLIO_RX_WOVF   = 1u << 1,
    LAG_EVT_PARLIO_TX_EOF    = 1u << 2,
    LAG_EVT_GDMA_IN_FAULT    = 1u << 3,
    LAG_EVT_GDMA_OUT_FAULT   = 1u << 4,
    LAG_EVT_BS_EOF_OVERLOAD  = 1u << 5,
};

enum {
    PHY_WRITE_NONE   = 0,
    PHY_WRITE_GAIN   = 1,
    PHY_WRITE_BW     = 2,
    PHY_WRITE_OFFSET = 3,
    PHY_WRITE_FFT    = 4,
};

typedef struct {
    int64_t time_us;
    uint32_t seq;
    uint32_t flags;
    uint16_t rx_off;
    uint16_t tx_off;
    uint8_t gain;
    uint8_t agc_state;
} lag_event_t;

/* ----- Fixed production constants ----- */

typedef enum {
    VIDEO_STD_MODE_AUTO = 0,
    VIDEO_STD_MODE_NTSC = 1,
    VIDEO_STD_MODE_PAL  = 2,
} video_standard_mode_t;

typedef enum {
    RF_BW_MODE_BW40 = 0,
    RF_BW_MODE_BW20 = 1,
    RF_BW_MODE_AUTO = 2,
} rf_bw_mode_t;

typedef enum {
    DEMOD_MODE_GOLDEN_PHASE5 = 0,
    DEMOD_MODE_TRAJECTORY_V2 = 1,
    DEMOD_MODE_COUNT,
} demod_mode_t;

typedef enum { RX_PROFILE_DIRECT_GAIN = 10 } rx_profile_t;

typedef struct {
    dma_descriptor_t *dscr;
    uint8_t *buffer;
    uint32_t length;
} ring_dscr_node_t;

typedef struct {
    int p_median;
    int q_phase;
    int n_clip;
    int n_origin;
    int clip_permille;
    int origin_permille;
    int n_coherent;
    int sum_cross;
    int sum_dot;
    int sum_i;
    int sum_q;
    int sum_i2;
    int sum_q2;
    int sum_iq;
    int dc_i_x100;
    int dc_q_x100;
    int iq_skew_permille;
    int iq_cross_permille;
    int winding_events;
    int winding_triplets;
    int winding_permille;
    int strong_winding_events;
    int strong_winding_triplets;
    int strong_winding_permille;
    uint32_t trajectory_uncertainty_sum;
    uint32_t trajectory_states;
    fusion_shadow_metrics_t fusion_shadow;
} control_metrics_t;

/* Firmware gain mode and AFC ownership. USB never paces the hardware. */

typedef enum {
    ANALOG_AGC_SHADOW = 0,
    ANALOG_AGC_ACTIVE = 1,
    ANALOG_AGC_MANUAL = 2,
} analog_agc_mode_t;

typedef enum {
    AGC_STATE_SEARCH = 0,
    AGC_STATE_LEARN  = 1,
    AGC_STATE_TRACK  = 2,
} agc_state_t;

typedef enum {
    AFC_MODE_AUTO = 0, /* Auto Carrier Centering: centers within safe +/-1.5 MHz bound when locked */
    AFC_MODE_HOLD = 1, /* AFC Hold: freeze current offset */
    AFC_MODE_OFF  = 2, /* AFC Off: reset to 0 kHz offset */
} afc_mode_t;

/* ---- Pre-demodulation labs (#165) -------------------------------------
 * Observers over completed DMA regions only; the 40 MS/s path is unchanged.
 * Each lab pauses the gain controller exactly like the 11p A/B above. */
typedef struct {
    uint32_t glitches, samples;
    int dc_i, dc_q;             /* milli-cells of the current lane */
    unsigned windows;
    control_metrics_t m;
} predemod_window_t;

/* Acquisition-only centering characterization. This deliberately does not
 * become a continuous AFC loop: each PHY retune can disturb analog video.
 * The probe scores actual demod/sync quality and restores the prior offset. */

/* Modern standalone menu renderer.
 *
 * SRAM-safe production raster: 384x56 logical pixels. Horizontal coordinates
 * use a sharp fractional 189/50 DAC-sample scale, and every logical Y row is
 * emitted on three scanlines. This makes the on-screen controls 50% taller without the
 * oversized 400x72x4 backing store that exceeded ESP32-C5 DRAM.
 */

_Static_assert(IQ_RATE_HZ == 40000000u, "IQ rate must be 40 MHz");
_Static_assert(DAC_RATE_HZ == 40000000u, "DAC rate must be 40 MHz");
_Static_assert(RAW_RING_BYTES == 32768u, "Ring must be exactly 32768 bytes");
_Static_assert(CONTROL_SAMPLE_BYTES <= 4092u, "Control window must fit one GDMA descriptor");
/* v4 deliberately consumes one of v3's two reserved bytes for demod_mode.
 * Keep the blob byte-for-byte the same size so a v3 record can be migrated
 * safely with DEMOD_MODE_GOLDEN_PHASE5. */
/* PHY bit scan for the native packet-AGC restart (native, VTX on, 'i').
 * On a continuous carrier the C5 packet AGC re-acquires every 25-50 us
 * (docs/native-agc-v2.md); no decoded register stops that while keeping it
 * tracking. This flips one bit at a time in the BB AGC/detection blocks,
 * measures gain-walk starts per ms on DIAG[20..26] (the witness capture)
 * and restores the word. A bit that at least halves the share of samples
 * inside gain walks while the AGC still acquires is re-measured on 16
 * windows and reported; bits that stop it (frozen gain) are only counted.
 * The baseline includes any 'z' patch, so a second run searches on top of
 * the first result. Video is garbage while it runs; nothing is persisted. */

extern void phy_enable_agc(void);

void sync_dma_c2m(const void *addr, size_t size);
void sync_dma_m2c(const void *addr, size_t size);
esp_err_t prepare_rx(void);
esp_err_t prepare_tx(void);
esp_err_t start_rx(void);
esp_err_t start_tx(void);
uint32_t get_rx_dma_offset(uint32_t *out_dscr_addr);
uint32_t get_tx_dma_offset(uint32_t *out_dscr_addr);
int find_dscr_index(const ring_dscr_node_t *nodes, int count, uint32_t addr);
int patch_descriptors_clear_eof(int dma_ch, bool is_rx);
bool copy_completed_rx_window(uint8_t *dst, size_t bytes, size_t *ring_offset);
uint8_t *get_completed_rx_sample_window(size_t bytes);
fusion_shadow_metrics_t active_demod_shadow(fusion_shadow_metrics_t shadow);
void video_standard_detector_reset(void);
int video_semantic_observe(const uint8_t *raw, size_t bytes, size_t ring_offset);
control_metrics_t analyze_control_window(const uint8_t *sample, size_t bytes,
                                                size_t ring_offset);
const char *rx_profile_name(void);
uint8_t profile_gain_max(void);
const char *rf_bw_mode_name(void);
const char *output_mode_name(void);
const char *demod_mode_name(void);
void apply_rf_bandwidth(bool bw40);
bool bw_fixed_calibrated(void);
void bw_set_edge(bool edge);
void cycle_rf_bandwidth_mode(void);
void apply_frequency_offset_khz_tracked(int offset_khz);
void step_frequency_offset_khz_tracked(int delta_khz);
uint8_t apply_rx_gain_for_generation(uint8_t gain, uint32_t phy_generation);
bool rx_probe_copy_completed_idx(uint8_t sample[RX_PROBE_REGIONS * RX_PROBE_REGION_BYTES],
                                        int *out_idx);
bool rx_probe_copy_completed(uint8_t sample[RX_PROBE_REGIONS * RX_PROBE_REGION_BYTES]);
void direct_gain_v3_sentinel_timer_cb(void *arg);
void direct_gain_v3_sentinel_task(void *arg);
void direct_gain_v3_observer_task(void *arg);
void settings_save(void);
void settings_load(void);
void poll_transport_faults(void);
hw_transport_counters_t lab_counter_snapshot(void);
void lab_reset_correlation(void);
void lab_print_row(const char *kind, const hw_transport_counters_t *base);
void p8env_capture_report(void);
void lab_dump_raw_probe(void);
void lab_apply_vendor_gain(uint8_t gain);
void lab_enter_quiet_baseline(void);
void lab_run_bandwidth_probe(bool vendor_path);
void lab_run_11p_probe(void);
bool predemod_collect(unsigned windows, predemod_window_t *out);
unsigned predemod_ppm(uint32_t glitches, uint32_t samples);
void predemod_print(const char *tag, const char *stage, int extra,
                           const predemod_window_t *w);
bool predemod_pause(const char *tag, analog_agc_mode_t *saved_mode);
void predemod_resume(analog_agc_mode_t saved_mode);
void lab_predemod_status(void);
void rx_clock_slip(uint32_t us);
void lab_run_sample_phase_scan(void);
bool lab_dco_measure(int dc[2]);
void lab_run_dco_probe(void);
void lab_run_filter_sweep(void);
bool lab_run_bw_calibration(bool automatic);
void bw_status_print(void);
bool lab_run_agc_witness(bool automatic);
void agc_mask_observe(void);
void lab_run_sigrssi(void);
void lab_run_phy_track(void);
void lab_run_bw20_wide(void);
void lab_run_dfilt(void);
void lab_run_native_hold(unsigned cycles);
void lab_request_fresh_phy_calibration(void);
void lab_toggle_native_agc_boot(void);
void lab_print_arc_oracle(void);
void apply_rx_profile(rx_profile_t profile);
void cycle_rx_profile(void);
void leave_experimental_profile(void);
const char *video_standard_name(video_standard_t standard);
video_standard_t resolved_menu_standard(void);
unsigned menu_item_count(void);
bool menu_changes_pending(void);
bool menu_item_apply(unsigned item);
void menu_render_menu(void);
void quiet_tx_interrupts(void);
void start_flight_demodulator(void);
void video_set_menu_mode(bool active);
void video_open_menu(void);
void menu_cycle_standard_mode(void);
void init_boot_button(void);
void cvbs_level_task(void *arg);
void lab_dco_quiet(const char *stage);
bool bw_autocal_waiting(void);
void predemod_task(void *arg);
void predemod_correction_print(void);
void idle_raster_service(int q_phase, bool fresh_sync, unsigned sync_age_ticks);
void sync_flywheel_task(void *arg);
void sync_flywheel_status_print(void);
void idle_raster_status_print(void);
void analog_agc_task(void *arg);
void cvbs_capture_task(void *arg);
void console_diag_task(void *arg);
/* ELRS VRx backpack (video_backpack.c): UART1 RX on GPIO10 (D10), channel requests
 * posted for the analog_agc control task to apply. */
void video_backpack_start(void);
bool video_backpack_take(size_t *index);
void video_backpack_print(void);
extern volatile hw_transport_counters_t s_hw_counters;
extern lag_event_t s_lag_events[LAG_EVENT_LOG_SIZE];
extern volatile uint32_t s_lag_event_head;
extern volatile int64_t s_last_gain_write_us;
extern volatile int64_t s_last_phy_write_us;
extern volatile uint8_t s_last_phy_write_kind;
extern volatile int64_t s_last_transport_event_us;
extern volatile uint32_t s_last_transport_flags;
extern volatile uint32_t s_last_gain_drop_transition;
extern volatile int64_t s_last_user_lag_mark_us;
extern volatile video_standard_mode_t s_video_std_mode;
extern volatile video_standard_t s_video_std;
extern volatile video_standard_t s_detected_video_std;
extern volatile bool s_detected_video_std_valid;
extern volatile uint8_t s_video_std_pal_score;
extern volatile uint8_t s_video_std_ntsc_score;
extern volatile uint16_t s_last_line_period_20m;
extern volatile uint16_t s_last_sync_width_20m;
extern volatile int s_last_sync_quality;
extern QueueHandle_t s_menu_commands;
extern bitscrambler_handle_t s_flight_bs;
extern unsigned s_menu_chunk_count;
extern TaskHandle_t s_level_task;
extern unsigned s_level_work_us;
extern volatile bool s_menu_active;
extern idle_raster_t s_idle;
extern TaskHandle_t s_sfw_task_handle;
extern volatile bool s_menu_boot_btn_enabled;
extern volatile int s_menu_cursor;
extern bool s_menu_edit;
extern unsigned s_menu_item;
extern volatile bool s_menu_bw_cal_request, s_menu_witness_request;
extern dg3_map_blob_t s_dg3_saved;
extern bool s_dg3_saved_valid;
extern uint32_t s_dg3_map_imports, s_dg3_map_saves;
extern int s_menu_timeout_ticks;
extern volatile rf_bw_mode_t s_rf_bw_mode;
extern volatile bool s_current_bw40;
extern volatile demod_mode_t s_demod_mode;
extern volatile rx_profile_t s_rx_profile;
extern direct_gain_v3_t s_direct_gain_v3;
extern volatile int s_v3_p50, s_v3_p90, s_v3_p95, s_v3_origin_pm;
extern volatile int s_v3_dc_i_mstep, s_v3_dc_q_mstep;
extern volatile uint32_t s_v3_bw_switches;
extern volatile int s_v3_clip_pm, s_v3_coherence;
extern TaskHandle_t s_v3_observer_task_handle;
extern TaskHandle_t s_v3_sentinel_task_handle;
extern volatile uint32_t s_gain_transition_count;
extern volatile dg3_state_t s_last_direct_gain_state;
extern volatile uint8_t s_last_direct_gain_target;
extern volatile int s_last_direct_gain_delta;
extern volatile uint32_t s_last_direct_gain_total_writes;
extern volatile uint32_t s_last_direct_gain_hold_cycles;
extern volatile uint32_t s_profile_generation;
extern const char *TAG;
extern uint8_t s_raw_ring[RAW_RING_BYTES];
extern parlio_rx_unit_handle_t      s_rx;
extern parlio_tx_unit_handle_t      s_tx;
extern int s_rx_dma_ch;
extern int s_tx_dma_ch;
extern ring_dscr_node_t s_rx_dscr_nodes[MAX_RING_DESCRIPTORS];
extern int s_rx_dscr_count;
extern ring_dscr_node_t s_tx_dscr_nodes[MAX_RING_DESCRIPTORS];
extern int s_tx_dscr_count;
extern SemaphoreHandle_t s_cvbs_analyze_lock;
extern volatile analog_agc_mode_t s_agc_mode;
extern volatile agc_state_t s_agc_state;
extern volatile afc_mode_t s_afc_mode;
extern volatile bool s_afc_video_locked;
extern volatile unsigned s_afc_fresh, s_afc_corrections;
extern volatile uint8_t s_current_gain;
extern volatile uint8_t s_shadow_gain;
extern volatile int s_last_p_median;
extern volatile int s_last_q_phase;
extern volatile int s_signal_strength;
extern volatile int s_last_clip_permille;
extern volatile int s_last_origin_permille;
extern volatile int s_last_dc_i_x100;
extern volatile int s_last_dc_q_x100;
extern volatile int s_last_iq_skew_permille;
extern volatile int s_last_iq_cross_permille;
extern volatile int s_last_winding_permille;
extern volatile int s_last_strong_winding_permille;
extern volatile int s_cfo_khz;
extern volatile bool s_channel_scan_active;
extern volatile unsigned s_channel_scan_progress;
extern volatile bool s_lab_quiet;
extern volatile bool s_pre_q4_probe_active;
extern volatile bool s_rssi_probe_active;
extern const char *s_witness_result;
extern agc_witness_result_t s_witness_last;
extern unsigned s_witness_runs;
extern volatile uint32_t s_cvbs_capture_running;

#define CAL_SETTLE_US 1500000LL
typedef enum { SPHASE_SCAN_REFUSED, SPHASE_SCAN_UNSETTLED, SPHASE_SCAN_SETTLED } sphase_scan_t;

typedef struct {
    int16_t code[2];
    int16_t residual_mcells[2];    /* provenance: DC left at those codes */
    uint8_t valid;
} dco_entry_t;

typedef struct {
    uint16_t freq;
    uint8_t version, lo, hi;
    uint8_t band5, lane_mode, iq_scale_sel;
    uint8_t recal;               /* measured on top of our exact-frequency recal */
    int8_t filter_code, filter_skirt;
    dco_entry_t e[ARC_VENDOR_GAIN_MAX + 1u];
} dco_table_blob_t;

typedef struct {
    uint32_t uptime_s;
    int16_t temp_c10, dc_i, dc_q;
    uint8_t gain, p50, p95, coherence, idle, boot;
} flog_entry_t;

#define FLOG_N 16u
typedef struct {
    uint8_t version, next, boot;
    flog_entry_t e[FLOG_N];
} flog_blob_t;

typedef enum { SPHASE_UNVERIFIED, SPHASE_CHECKING, SPHASE_SETTLED, SPHASE_FAILED } sphase_state_t;

void dco_post_gain(uint8_t g);
void dco_hook_print(const char *tag);
void dco_ab_toggle(void);
void lab_run_rx_recal(void);
void predemod_dco_service(void);

void predemod_gain_readback_service(void);
void gain_readback_print(void);
void predemod_dc_drift_service(void);

void lab_run_sigrssi_ladder(void);

void flight_log_service(void);
void flight_log_print(void);
const char *sphase_state_name(void);
void predemod_sphase_autocheck(void);

extern int64_t s_quiet_since_us, s_quiet_eval_us;
extern uint16_t s_rx_recal_freq;
extern uint32_t s_rx_recal_runs;
extern volatile int64_t s_last_idle_sync_us;
extern dco_table_blob_t s_dco_tab;
extern uint32_t s_dco_searches, s_dco_holds, s_dco_loads, s_dco_saves, s_dco_carrier_refusals;
extern bool s_dco_dirty;
extern uint8_t s_dco_env_buf[24u * RX_PROBE_REGIONS * RX_PROBE_REGION_BYTES];
extern unsigned s_dco_env_ratio;
extern uint32_t s_dco_broken_iq, s_dco_hold_aborts;
extern float s_temp_c;
extern int s_drift_avg[2];
extern uint32_t s_drift_nudges;
extern volatile uint32_t s_obs_us_sum, s_obs_us_max, s_obs_windows;
extern volatile uint32_t s_sfw_last_us, s_sfw_max_us, s_sfw_rebases;
extern bool s_sphase_auto_done;
extern unsigned s_sphase_auto_ppm;
extern unsigned s_sphase_scans;
extern volatile int64_t s_cal_settle_until_us;

extern unsigned char phy_param[];
