/**
 * main.c - C5VRX-3 application entry point.
 *
 * Ultra-minimal single-purpose FPV receiver.
 * Starts RF frontend, starts video pipeline, then exits.
 * The hardware runs forever; the application is done.
 */

#include "rf.h"
#include "video.h"
#include "esp_log.h"
#include "bs_relative_worker_probe.h"
#include "bs_relative_middle_probe.h"
#if CONFIG_C5VRX_BS_SPEEDLAB
#include "bs_speedlab.h"
#endif

void app_main(void)
{
#if CONFIG_C5VRX_BS_SPEEDLAB
    bs_speedlab_run();
#endif
#if CONFIG_C5VRX_BS_RELATIVE_WORKER_PROBE
    bs_relative_worker_probe_run();
#endif
#if CONFIG_C5VRX_BS_RELATIVE_MIDDLE_PROBE
    bs_relative_middle_probe_run();
#endif
    ESP_ERROR_CHECK(rf_start());
    ESP_ERROR_CHECK(video_start());
    /* Hardware pipeline is running. Application has nothing more to do. */
}
