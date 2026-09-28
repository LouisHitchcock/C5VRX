#include "bs_phase8_hr_probe.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "driver/bitscrambler.h"
#include "driver/bitscrambler_loopback.h"
#include "hal/bitscrambler_peri_select.h"

BITSCRAMBLER_PROGRAM(s_phase8_hr_probe, "bs_phase8_hr_probe");

/* Representatives cover each quadrant, axis-adjacent cells and all LUT banks. */
static const uint8_t s_raw[16] = {
    0x70, 0x73, 0x77, 0x37, 0x07, 0x0b, 0x0f, 0x3f,
    0x8f, 0xcf, 0xff, 0xfb, 0xf7, 0xb7, 0x87, 0x83,
};
static const uint8_t s_phase8[16] = {
    3, 18, 32, 46, 61, 196, 223, 250,
    131, 134, 160, 187, 67, 86, 96, 110,
};
static uint8_t s_input[256] __attribute__((aligned(4)));
static uint8_t s_output[256] __attribute__((aligned(4)));
static bool s_ran;
static bool s_pass;
static size_t s_written;
static unsigned s_mismatches;
static esp_err_t s_err;

static unsigned sample_index(unsigned pair)
{
    return (pair * 7u + pair / 16u) & 15u;
}

void bs_phase8_hr_probe_report(void)
{
    printf("BS_PHASE8_HR status=%s written=%u mismatches=%u err=%s\n",
           !s_ran ? "NOT_RUN" : s_pass ? "PASS" : "FAIL",
           (unsigned)s_written, s_mismatches, esp_err_to_name(s_err));
}

void bs_phase8_hr_probe_run(void)
{
    for (unsigned pair = 0; pair < 128; ++pair) {
        s_input[2 * pair] = 0;
        s_input[2 * pair + 1] = s_raw[sample_index(pair)];
    }
    memset(s_output, 0xa5, sizeof(s_output));

    bitscrambler_handle_t bs = NULL;
    size_t written = 0;
    esp_err_t err = bitscrambler_loopback_create(&bs,
                                                  SOC_BITSCRAMBLER_ATTACH_I2S0,
                                                  sizeof(s_input));
    if (err == ESP_OK) err = bitscrambler_load_program(bs, s_phase8_hr_probe);
    if (err == ESP_OK)
        err = bitscrambler_loopback_run(bs, s_input, sizeof(s_input),
                                       s_output, sizeof(s_output), &written);
    if (bs) bitscrambler_free(bs);

    unsigned mismatches = 0;
    for (unsigned pair = 2; pair < 128; ++pair) {
        int current = s_phase8[sample_index(pair - 1)];
        int previous = s_phase8[sample_index(pair - 2)];
        int delta = ((current - previous + 128) & 255) - 128;
        uint8_t expected = (uint8_t)(((80 + 3 * delta) & 255) >> 2);
        mismatches += s_output[2 * pair] != expected;
        mismatches += s_output[2 * pair + 1] != expected;
    }
    s_ran = true;
    s_pass = err == ESP_OK && written == sizeof(s_output) && mismatches == 0;
    s_written = written;
    s_mismatches = mismatches;
    s_err = err;
    bs_phase8_hr_probe_report();
}
