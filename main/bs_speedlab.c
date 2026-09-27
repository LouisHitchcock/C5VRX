/* Boot-only memory-to-memory BitScrambler throughput measurement.
 * Runs before RF/video is initialized; never participates in live CVBS. */
#include "bs_speedlab.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "driver/bitscrambler.h"
#include "driver/bitscrambler_loopback.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "hal/bitscrambler_peri_select.h"

#define SPEEDLAB_BYTES 8192u
#define SPEEDLAB_RUNS 16u

BITSCRAMBLER_PROGRAM(s_speedlab_k1, "bs_speedlab_k1");
BITSCRAMBLER_PROGRAM(s_speedlab_k2, "bs_speedlab_k2");
BITSCRAMBLER_PROGRAM(s_speedlab_k3, "bs_speedlab_k3");
BITSCRAMBLER_PROGRAM(s_speedlab_k4, "bs_speedlab_k4");
BITSCRAMBLER_PROGRAM(s_speedlab_k5, "bs_speedlab_k5");
BITSCRAMBLER_PROGRAM(s_speedlab_k6, "bs_speedlab_k6");
BITSCRAMBLER_PROGRAM(s_speedlab_k7, "bs_speedlab_k7");
BITSCRAMBLER_PROGRAM(s_speedlab_k8, "bs_speedlab_k8");

static const void *const programs[8] = {
    s_speedlab_k1, s_speedlab_k2, s_speedlab_k3, s_speedlab_k4,
    s_speedlab_k5, s_speedlab_k6, s_speedlab_k7, s_speedlab_k8,
};

static const struct {
    const char *name;
    int attach;
} attachments[] = {
    {"I2S0", SOC_BITSCRAMBLER_ATTACH_I2S0},
    {"GPSPI2", SOC_BITSCRAMBLER_ATTACH_GPSPI2},
    {"UHCI", SOC_BITSCRAMBLER_ATTACH_UHCI},
    {"AES", SOC_BITSCRAMBLER_ATTACH_AES},
    {"SHA", SOC_BITSCRAMBLER_ATTACH_SHA},
    {"ADC", SOC_BITSCRAMBLER_ATTACH_ADC},
    {"PARLIO", SOC_BITSCRAMBLER_ATTACH_PARL_IO},
};

static void run_attachment(const char *name, int attach, uint8_t *input,
                           uint8_t *output)
{
    bitscrambler_handle_t bs = NULL;
    esp_err_t err = bitscrambler_loopback_create(&bs, attach, SPEEDLAB_BYTES);
    if (err != ESP_OK) {
        printf("BS_SPEEDLAB attach=%s create=%s\n", name, esp_err_to_name(err));
        return;
    }

    for (unsigned k = 1; k <= 8; ++k) {
        err = bitscrambler_load_program(bs, programs[k - 1]);
        size_t written = 0;
        if (err == ESP_OK) {
            /* A warmup is kept outside the timed interval. Short writes and
             * stale output abort this case instead of reporting a speed. */
            memset(output, 0xa5, SPEEDLAB_BYTES);
            err = bitscrambler_loopback_run(bs, input, SPEEDLAB_BYTES,
                                            output, SPEEDLAB_BYTES, &written);
        }
        if (err != ESP_OK || written != SPEEDLAB_BYTES) {
            printf("BS_SPEEDLAB attach=%s k=%u warmup=%s written=%u\n",
                   name, k, esp_err_to_name(err), (unsigned)written);
            continue;
        }

        int64_t started = esp_timer_get_time();
        unsigned done = 0;
        for (; done < SPEEDLAB_RUNS; ++done) {
            written = 0;
            err = bitscrambler_loopback_run(bs, input, SPEEDLAB_BYTES,
                                            output, SPEEDLAB_BYTES, &written);
            if (err != ESP_OK || written != SPEEDLAB_BYTES) break;
        }
        int64_t elapsed = esp_timer_get_time() - started;
        bool zeros = true;
        for (size_t i = 0; i < SPEEDLAB_BYTES; ++i) {
            if (output[i] != 0u) { zeros = false; break; }
        }
        if (done != SPEEDLAB_RUNS || !zeros || elapsed <= 0) {
            printf("BS_SPEEDLAB attach=%s k=%u status=INVALID runs=%u/%u written=%u output_zero=%u err=%s\n",
                   name, k, done, SPEEDLAB_RUNS, (unsigned)written, zeros,
                   esp_err_to_name(err));
            continue;
        }
        uint64_t bytes = (uint64_t)SPEEDLAB_BYTES * SPEEDLAB_RUNS;
        uint64_t pairs = bytes / 2u;
        /* Decimal MB/s and million executed bundles/s, both x100. These
         * include driver call/reset overhead and are lower bounds. */
        uint64_t mbps_x100 = bytes * 100u / (uint64_t)elapsed;
        uint64_t bundles_mhz_x100 = pairs * k * 100u / (uint64_t)elapsed;
        printf("BS_SPEEDLAB attach=%s k=%u us=%" PRId64 " in_MBps_x100=%" PRIu64
               " bundle_MHz_x100=%" PRIu64 " status=PASS\n",
               name, k, elapsed, mbps_x100, bundles_mhz_x100);
    }
    bitscrambler_free(bs);
}

void bs_speedlab_run(void)
{
    uint8_t *input = heap_caps_malloc(SPEEDLAB_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    uint8_t *output = heap_caps_malloc(SPEEDLAB_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!input || !output) {
        printf("BS_SPEEDLAB status=NO_DMA_MEMORY\n");
        if (input) heap_caps_free(input);
        if (output) heap_caps_free(output);
        return;
    }
    for (size_t i = 0; i < SPEEDLAB_BYTES; ++i) input[i] = (uint8_t)(i * 73u + 11u);
    for (size_t i = 0; i < sizeof(attachments) / sizeof(attachments[0]); ++i) {
        run_attachment(attachments[i].name, attachments[i].attach, input, output);
    }
    heap_caps_free(input);
    heap_caps_free(output);
}
