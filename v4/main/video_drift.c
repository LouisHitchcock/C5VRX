#include "video_internal.h"

#define DRIFT_LIMIT_MCELLS 300
#define DRIFT_HOLD_S       5u
#define DRIFT_MAX_STEPS    4
static temperature_sensor_handle_t s_tsens;
float s_temp_c = -100.0f;
static int32_t s_drift_sum[2];
static unsigned s_drift_n, s_drift_ticks, s_drift_over_s;
int s_drift_avg[2];
static RTC_FAST_ATTR int s_drift_offset[ARC_VENDOR_GAIN_MAX + 1u][2];
uint32_t s_drift_nudges;
static int64_t s_drift_last_nudge_us, s_drift_last_log_us;

void predemod_dc_drift_service(void)
{
    const int64_t now = esp_timer_get_time();
    if (!s_tsens) {
        temperature_sensor_config_t cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
        if (temperature_sensor_install(&cfg, &s_tsens) == ESP_OK) (void)temperature_sensor_enable(s_tsens);
        else s_tsens = (temperature_sensor_handle_t)(uintptr_t)1u;   /* do not retry */
    }
    if ((uintptr_t)s_tsens > 1u && (s_drift_ticks & 3u) == 0u) (void)temperature_sensor_get_celsius(s_tsens, &s_temp_c);
    ++s_drift_ticks;
    if (now - s_drift_last_log_us > 60000000LL) {
        s_drift_last_log_us = now;
        printf("DC_DRIFT temp_c=%.1f avg_mcells=%d/%d over_s=%u nudges=%lu gain=%u\n", (double)s_temp_c,
               s_drift_avg[0], s_drift_avg[1], s_drift_over_s, (unsigned long)s_drift_nudges, s_current_gain);
    }
    const arc_gain_table_t *table = rf_get_arc_gain_table();
    const uint8_t g = s_current_gain;
    if (!table || rf_native_agc_active() || s_rx_profile != RX_PROFILE_DIRECT_GAIN ||
        s_agc_mode != ANALOG_AGC_ACTIVE || IDLE_RASTER_ACTIVE() || phy_rx_lab_busy() ||
        s_direct_gain_v3.state != DG3_HOLD || !phy_rx_lab_dco_held() ||
        g > table->max_index || !s_dco_tab.e[g].valid) {
        s_drift_n = 0; s_drift_sum[0] = s_drift_sum[1] = 0; s_drift_over_s = 0;
        return;
    }
    predemod_window_t w;
    if (!predemod_collect(8, &w) || s_current_gain != g || s_direct_gain_v3.state != DG3_HOLD) {
        s_drift_n = 0; s_drift_sum[0] = s_drift_sum[1] = 0;
        return;
    }
    s_drift_sum[0] += w.dc_i; s_drift_sum[1] += w.dc_q;
    if (++s_drift_n < 4u) return;                 /* ~1 s at the 250 ms tick */
    s_drift_avg[0] = (int)(s_drift_sum[0] / (int32_t)s_drift_n);
    s_drift_avg[1] = (int)(s_drift_sum[1] / (int32_t)s_drift_n);
    s_drift_n = 0; s_drift_sum[0] = s_drift_sum[1] = 0;
    bool over = abs(s_drift_avg[0]) > DRIFT_LIMIT_MCELLS || abs(s_drift_avg[1]) > DRIFT_LIMIT_MCELLS;
    s_drift_over_s = over ? s_drift_over_s + 1u : 0u;
    if (s_drift_over_s < DRIFT_HOLD_S || now - s_drift_last_nudge_us < 2000000LL) return;
    int di = s_drift_avg[0] > DRIFT_LIMIT_MCELLS ? -1 : s_drift_avg[0] < -DRIFT_LIMIT_MCELLS ? 1 : 0;
    int dq = s_drift_avg[1] > DRIFT_LIMIT_MCELLS ? -1 : s_drift_avg[1] < -DRIFT_LIMIT_MCELLS ? 1 : 0;
    if (abs(s_drift_offset[g][0] + di) > DRIFT_MAX_STEPS) di = 0;
    if (abs(s_drift_offset[g][1] + dq) > DRIFT_MAX_STEPS) dq = 0;
    if (!di && !dq) return;
    int codes[2];
    if (!phy_rx_lab_dco_nudge(di, dq, codes)) return;
    s_drift_offset[g][0] += di; s_drift_offset[g][1] += dq;
    s_dco_tab.e[g].code[0] = (int16_t)codes[0];
    s_dco_tab.e[g].code[1] = (int16_t)codes[1];
    s_dco_dirty = true;
    s_drift_last_nudge_us = now;
    s_drift_over_s = 0;
    ++s_drift_nudges;
    printf("DC_DRIFT nudge gain=%u step=%d/%d codes=%d/%d avg_mcells=%d/%d temp_c=%.1f\n", g, di, dq,
           codes[0], codes[1], s_drift_avg[0], s_drift_avg[1], (double)s_temp_c);
}
