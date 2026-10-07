#include "video_internal.h"

static flog_blob_t s_flog;
static bool s_flog_loaded;
static int64_t s_flog_last_us;

void flight_log_service(void)
{
    const int64_t now = esp_timer_get_time();
    if (!s_flog_loaded) {
        s_flog_loaded = true;
        if (!c5vrx4_blob_load("flightlog", &s_flog, sizeof(s_flog)) || s_flog.version != 1u) {
            memset(&s_flog, 0, sizeof(s_flog));
            s_flog.version = 1u;
        }
        ++s_flog.boot;
        s_flog_last_us = now;
    }
    if (now - s_flog_last_us < 60000000LL) return;
    s_flog_last_us = now;
    predemod_window_t w;
    bool dc = predemod_collect(8, &w);
    flog_entry_t *e = &s_flog.e[s_flog.next % FLOG_N];
    *e = (flog_entry_t){
        .uptime_s = (uint32_t)(now / 1000000),
        .temp_c10 = (int16_t)(s_temp_c * 10.0f),
        .dc_i = (int16_t)(dc ? w.dc_i : 0), .dc_q = (int16_t)(dc ? w.dc_q : 0),
        .gain = s_current_gain, .p50 = (uint8_t)s_v3_p50, .p95 = (uint8_t)s_v3_p95,
        .coherence = (uint8_t)s_v3_coherence, .idle = IDLE_RASTER_ACTIVE(), .boot = s_flog.boot,
    };
    s_flog.next = (uint8_t)((s_flog.next + 1u) % FLOG_N);
    (void)c5vrx4_blob_store("flightlog", &s_flog, sizeof(s_flog));
}

void flight_log_print(void)
{
    if (!s_flog_loaded && !c5vrx4_blob_load("flightlog", &s_flog, sizeof(s_flog))) {
        printf("FLIGHTLOG empty\n");
        return;
    }
    printf("FLIGHTLOG boot_now=%u (oldest first)\n", s_flog.boot);
    for (unsigned k = 0; k < FLOG_N; ++k) {
        const flog_entry_t *e = &s_flog.e[(s_flog.next + k) % FLOG_N];
        if (!e->boot) continue;
        printf("FLIGHTLOG boot=%u t_s=%lu temp_c=%.1f gain=%u p50=%u p95=%u coherence=%u idle=%u dc_mcells=%d/%d\n",
               e->boot, (unsigned long)e->uptime_s, e->temp_c10 / 10.0, e->gain, e->p50, e->p95,
               e->coherence, e->idle, e->dc_i, e->dc_q);
        vTaskDelay(pdMS_TO_TICKS(2));
    }
}
