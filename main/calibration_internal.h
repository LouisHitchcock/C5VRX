/* C5VRX-5 by Twotoz and contributors: what the calibration task's files
 * share. calibration.c (task, boot, manual startup phase, report),
 * calibration_sphase.c (sampling phase), calibration_filter.c (analog
 * filter), calibration_dc.c (DC tables, searches, health, live servo) and
 * calibration_bench.c (console L / self-noise) are one module: everything
 * here runs on the calibration task. The rest of the firmware uses only the
 * calibration_* functions in receiver.h. */
#pragma once
#include "receiver.h"
#include "sense_lab.h"
#include "cn_auto.h"
#include "calibration_noise.h"
#include "calibration_policy.h"
#include "dco_preset.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dsp.h"
#include "dc_live.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include "phy.h"
#include "profile_logic.h"
#include "sphase_logic.h"
#include "dco_slots.h"
#include "dc_health.h"
#include "rf.h"

typedef struct {
    uint32_t windows;
    uint64_t hf4_d4, hf4_count, hf2_d2;
    int dc_i, dc_q;          /* milli-cells: circle centre or plain mean */
    bool dc_circle, dc_qualified, dc_noise_dither;
    unsigned envelope_x100;
    metrics_t m;             /* last window */
    cna_sums_t noise_sums;   /* optional DC-verification reuse */
    double noise_m2, noise_m4;
} window_stats_t;

/* ---- calibration.c ---- */
extern int64_t s_probe_deadline;
extern bool s_boot_phase_pending;
extern volatile bool s_startup;
bool quiet_owner(void);
bool collect_stats(unsigned windows, window_stats_t *out, bool noise);
bool collect(unsigned windows, window_stats_t *out);
bool video_recent(void);
typedef struct { unsigned lines; uint32_t hash; bool failed; } cal_dump_t;
void cal_dump_line(cal_dump_t *d, const char *fmt, ...);

/* ---- calibration_sphase.c ---- */
extern int s_sphase_excess;
extern const char *s_sphase_state;
int sphase_record(const window_stats_t *w);
sphase_verdict_t sphase_scan_n(const char *tag, unsigned positions, unsigned windows, unsigned tries, bool carrier);
bool sphase_on_carrier(void);
void sphase_carrier_scan(const char *stage, bool repair);
void sphase_qualify(const char *stage, bool fresh_context);
void phase_service(void);
uint8_t sphase_code(void);
void cal_sphase_print(void);

/* ---- calibration_filter.c ---- */
#define FILTER_VERSION     6u  /* 6: 11p baseline plus measured analog profiles */
typedef struct {
    uint8_t valid, bw20, skirt;
    int8_t code;
    uint16_t width_khz, nbw_khz;
} filter_entry_t;
extern const char *s_filter_result;
filter_entry_t filter_class_entry(int filter_class);
void filter_measure_current(void);
void cal_filter_load(void);
bool cal_filter_measured(void);
bool cal_filter_discard(void);
void cal_filter_record(uint8_t *p);
void cal_filter_print(rf_filter_t f);
int dco_preset_class_of(uint16_t key);

/* ---- calibration_dc.c ---- */
#define DCO_ROW_OK    1u
#define DCO_ROW_FLOOR 2u
extern TaskHandle_t s_cal_task;
extern uint32_t s_dc_urgent_wakes;
extern cal_watch_t s_basemap_watch;
extern unsigned s_health_blocks, s_health_rejected;
extern int64_t s_health_us;
extern bool s_cal_notice;
extern const char *s_dco_last_why;
extern uint32_t s_dco_searches, s_dco_adopted, s_dco_preset_holds;
uint32_t health_context(void);
const char *noise_check(unsigned *env_out, int *clip_out);
void dco_hold_for_scan(void);
bool noise_like(void);
unsigned dco_rows(uint8_t state);
unsigned dco_valid_rows(void);
unsigned dco_template_rows(void);
bool dco_current_complete(void);
void dco_select(void);
void dco_ready_note(void);
void dco_startup(bool manual, int64_t deadline);
void dco_startup_profiles(bool manual, int64_t deadline);
void noise_map_startup(int64_t deadline);
void dco_service(void);
void servo_service(void);
void drift_service(void);
void cal_dc_noise_cache_clear(void);
void cal_dc_discard(void);
bool cal_dc_store_discarded(void);
void cal_dc_record(uint8_t *p);
void cal_dc_dump_tables(cal_dump_t *d);
void cal_dc_print(rf_filter_t f);

/* ---- calibration_bench.c ---- */
extern volatile bool s_lane_requested, s_selfnoise_requested;
void lane_compare(void);
void self_noise_test(void);
