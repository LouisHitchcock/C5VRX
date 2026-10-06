/* C5VRX by Twotoz and contributors: live 10-bit band-power / SNR meter.
 *
 * rf_enable_continuous_modem() leaves the RF dump writer running forever
 * (dump-first, TX_START never fires) with the HP SRAM port held by the CPU:
 * the writer feeds MODEM_DIAG, its SRAM writes go nowhere. A reading hands the
 * port to the MAC for ~40 us (HP_SRAM_USAGE[11:8] = 2, the vendor adctrig
 * value), so the writer fills part of the 64 KiB bank at 0x40830000 with full
 * 10-bit I/Q words, then hands it back and reads the bank
 * (docs/continuous-iq-findings.md: bank A only, readable once ownership is
 * returned). DUMP_CTRL, the selector and the DIAG routing are never written.
 *
 * There is no spare 64 KiB on this build (~38 KB heap free while receiving),
 * so the bank is the first 64 KiB of the menu/idle raster (video.c), which is
 * idle and rebuilt on every entry whenever live video owns TX. A reading
 * refuses while the menu or the idle raster is up, or while a render runs, and
 * is discarded if a render happened before the analysis finished.
 *
 * Method and lessons from FPVGateC5MK's band-power meter (Louis Hitchcock);
 * this is a fresh implementation for C5VRX. See SNR_METER.md. */
#include "snr_meter_hw.h"
#include "snr_meter.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "c5vrx4.h"
#include "rf.h"

#define REG32(a)          (*(volatile uint32_t *)(uintptr_t)(a))
#define DUMP_CTRL         0x600a9004u
#define HP_SRAM_USAGE     0x60095004u
#define CTRL_ENABLE       0x80000000u
#define SRAM_OWNER_MASK   0x00000f00u
#define SRAM_OWNER_MAC    0x00000200u
#define SRAM_DUMP_ALLOC   0x00010000u
#define DUMP_BANK_ADDR    0x40830000u
/* 40 us = ~3200 words at 80 MS/s: room for 2048 plus the edge trim. */
#define WINDOW_US         40u
/* Words at both ends of the run may straddle an ownership switch. */
#define EDGE_TRIM         16u
#define PERIOD_US         200000
#define FLOOR_READINGS    16u
#define FLOOR_VERSION     1u
#define GAIN_SLOTS        90u

typedef struct {
    uint8_t version;
    uint8_t reserved[3];
    int16_t floor_cdb[GAIN_SLOTS];   /* in-band floor, centi-dB; INT16_MIN = none */
} snr_floor_store_t;

static snr_floor_store_t s_floor;
static bool s_floor_loaded;
static bool s_periodic;
static int64_t s_next_us;
static snr_filter_t s_filter;
static unsigned s_filter_gain = 0xffu;
static bool s_filter_snr;
static uint16_t s_filter_mhz;
static uint32_t s_seq;

static void floor_load(void)
{
    if (s_floor_loaded) return;
    s_floor_loaded = true;
    if (c5vrx4_blob_load("snr_floor", &s_floor, sizeof(s_floor)) &&
        s_floor.version == FLOOR_VERSION) return;
    memset(&s_floor, 0, sizeof(s_floor));
    s_floor.version = FLOOR_VERSION;
    for (unsigned g = 0; g < GAIN_SLOTS; ++g) s_floor.floor_cdb[g] = INT16_MIN;
}

static float floor_for_gain(unsigned gain)
{
    floor_load();
    if (gain >= GAIN_SLOTS || s_floor.floor_cdb[gain] == INT16_MIN) return 0.0f;
    return powf(10.0f, (float)s_floor.floor_cdb[gain] / 1000.0f);
}

typedef enum {
    CAP_OK = 0, CAP_UNRESERVED, CAP_WRITER_OFF, CAP_RASTER_BUSY, CAP_DISTURBED, CAP_SHORT,
} cap_status_t;

static const char *cap_name(cap_status_t s)
{
    static const char *const names[] = {"ok", "bank_not_reserved", "dump_writer_off",
                                        "raster_in_use", "raster_rendered", "short_window"};
    return names[s];
}

/* One reading. On CAP_OK, r holds the analysis of the freshest words. */
static cap_status_t capture(snr_result_t *r, uint32_t *window_words)
{
    static int8_t reserved = -1;
    *window_words = 0;
    if (reserved < 0) reserved = video_raster_region_ok() ? 1 : 0;
    if (!reserved) return CAP_UNRESERVED;
    if (!(REG32(DUMP_CTRL) & CTRL_ENABLE)) return CAP_WRITER_OFF;

    /* Interrupts off from the idle check to the hand-back (~150 us: sentinel
     * fill plus the window): no render can start in between, and nothing on
     * this core touches the bank while the MAC owns it. */
    volatile uint32_t *bank = (volatile uint32_t *)(uintptr_t)DUMP_BANK_ADDR;
    uint32_t gen, gen_after, mstatus;
    __asm__ __volatile__("csrrc %0, mstatus, %1" : "=r"(mstatus) : "r"(0x8u) : "memory");
    const bool idle = video_raster_idle(&gen);
    if (idle) {
        for (size_t k = 0; k < SNR_RING_WORDS; ++k) bank[k] = SNR_SENTINEL;
        __asm__ __volatile__("fence iorw, iorw" ::: "memory");
        const uint32_t usage = REG32(HP_SRAM_USAGE);
        REG32(HP_SRAM_USAGE) = (usage & ~SRAM_OWNER_MASK) | SRAM_OWNER_MAC | SRAM_DUMP_ALLOC;
        __asm__ __volatile__("fence iorw, iorw" ::: "memory");
        esp_rom_delay_us(WINDOW_US);
        REG32(HP_SRAM_USAGE) = usage;
        __asm__ __volatile__("fence iorw, iorw" ::: "memory");
    }
    if (mstatus & 0x8u) __asm__ __volatile__("csrs mstatus, %0" : : "r"(0x8u) : "memory");
    if (!idle) return CAP_RASTER_BUSY;

    size_t start;
    size_t n = snr_fresh_run(bank, SNR_RING_WORDS, SNR_SENTINEL, &start);
    *window_words = (uint32_t)n;
    if (n < 2u * EDGE_TRIM + SNR_MIN_SAMPLES) return CAP_SHORT;
    start = (start + EDGE_TRIM) % SNR_RING_WORDS;
    n -= 2u * EDGE_TRIM;
    const bool analysed = snr_analyze(bank, SNR_RING_WORDS, start, n, r);
    if (!video_raster_idle(&gen_after) || gen_after != gen) return CAP_DISTURBED;
    return analysed ? CAP_OK : CAP_SHORT;
}

static void print_reading(const char *tag, const snr_result_t *r, uint32_t window_words,
                          float filtered, bool filtered_is_snr)
{
    const unsigned gain = r->gain_min;
    const float floor_in = r->gain_min == r->gain_max ? floor_for_gain(gain) : 0.0f;
    const float snr = snr_estimate_db(r->in_band, floor_in);
    printf("%s seq=%" PRIu32 " t_ms=%lld mhz=%u off_khz=%d gain=%u",
           tag, ++s_seq, (long long)(esp_timer_get_time() / 1000),
           (unsigned)rf_get_frequency_mhz(), rf_get_frequency_offset_khz(), gain);
    if (r->gain_max != r->gain_min) printf("..%u", r->gain_max);
    printf(" in_db=%.2f edge_db=%.2f total_db=%.2f lo_db=%.2f hi_db=%.2f peak_bin=%d",
           (double)snr_db(r->in_band), (double)snr_db(r->edge), (double)snr_db(r->total),
           (double)snr_db(r->lower), (double)snr_db(r->upper), r->peak_bin);
    if (floor_in > 0.0f)
        printf(" floor_db=%.2f over_db=%.2f snr_db=%.2f", (double)snr_db(floor_in),
               (double)(snr_db(r->in_band) - snr_db(floor_in)), (double)snr);
    else
        printf(" floor_db=na over_db=na snr_db=na");
    if (!isnan(filtered))
        printf(" filt_%s=%.2f", filtered_is_snr ? "snr_db" : "in_db", (double)filtered);
    printf(" dc_i=%.2f dc_q=%.2f clips=%u n=%u seg=%u window=%" PRIu32 "\n",
           (double)r->dc_i, (double)r->dc_q, r->clips, (unsigned)r->samples,
           (unsigned)r->segments, window_words);
}

static void print_psd(const snr_result_t *r)
{
    /* RF orientation, -40..+38.75 MHz in 1.25 MHz bins, centi-dB. */
    printf("SNR_PSD bin_khz=%u from_bin=-32 cdb=", (unsigned)(SNR_BIN_HZ / 1000.0));
    for (int f = -32; f < 32; ++f) {
        int k = SNR_SPECTRUM_SIGN * f;
        unsigned idx = (unsigned)(k < 0 ? k + (int)SNR_FFT : k);
        printf("%s%d", f == -32 ? "" : ",", (int)lroundf(100.0f * snr_db(r->psd[idx])));
    }
    printf("\n");
}

static void run_floor(void)
{
    floor_load();
    float sum = 0.0f;
    unsigned used = 0, gain = 0, mixed = 0;
    for (unsigned k = 0; k < FLOOR_READINGS * 2u && used < FLOOR_READINGS; ++k) {
        snr_result_t r;
        uint32_t window;
        cap_status_t st = capture(&r, &window);
        if (st != CAP_OK) {
            /* With no carrier the idle raster takes TX after 2 s and holds
             * the bank; '_' turns it off for the floor measurement. */
            printf("SNR_FLOOR refused=%s window=%" PRIu32 "%s\n", cap_name(st), window,
                   st == CAP_RASTER_BUSY ? " hint=idle_raster_or_menu_up_use_'_'_to_opt_out" : "");
            return;
        }
        if (r.gain_min != r.gain_max || (used && r.gain_min != gain)) { ++mixed; continue; }
        gain = r.gain_min;
        sum += r.in_band;
        ++used;
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    if (used < FLOOR_READINGS / 2u || gain >= GAIN_SLOTS) {
        printf("SNR_FLOOR refused=gain_moving used=%u mixed=%u\n", used, mixed);
        return;
    }
    const float mean = sum / (float)used;
    s_floor.floor_cdb[gain] = (int16_t)lroundf(1000.0f * log10f(mean));
    const bool stored = c5vrx4_blob_store("snr_floor", &s_floor, sizeof(s_floor));
    printf("SNR_FLOOR gain=%u floor_db=%.2f readings=%u mixed=%u stored=%u "
           "note=VTX_must_be_off\n", gain, (double)snr_db(mean), used, mixed, stored);
}

bool snr_meter_console(int key)
{
    if (key == '7') {
        snr_result_t r;
        uint32_t window;
        cap_status_t st = capture(&r, &window);
        if (st != CAP_OK) {
            printf("SNR refused=%s window=%" PRIu32 "\n", cap_name(st), window);
            return true;
        }
        print_reading("SNR", &r, window, NAN, false);
        print_psd(&r);
        return true;
    }
    if (key == '8') {
        run_floor();
        return true;
    }
    if (key == '9') {
        s_periodic = !s_periodic;
        snr_filter_reset(&s_filter);
        s_next_us = 0;
        printf("SNR_PERIODIC on=%u period_ms=%u\n", s_periodic, PERIOD_US / 1000);
        return true;
    }
    return false;
}

void snr_meter_tick(void)
{
    if (!s_periodic) return;
    const int64_t now = esp_timer_get_time();
    if (now < s_next_us) return;
    s_next_us = now + PERIOD_US;
    snr_result_t r;
    uint32_t window;
    cap_status_t st = capture(&r, &window);
    if (st != CAP_OK) {
        printf("SNR_ROW refused=%s window=%" PRIu32 "\n", cap_name(st), window);
        return;
    }
    /* Filter the SNR when this gain has a floor (gain changes then keep the
     * series continuous), otherwise the raw in-band level, restarted on any
     * gain or channel change. */
    const float floor_in = r.gain_min == r.gain_max ? floor_for_gain(r.gain_min) : 0.0f;
    const bool use_snr = floor_in > 0.0f;
    const uint16_t mhz = rf_get_frequency_mhz();
    if (use_snr != s_filter_snr || mhz != s_filter_mhz || (!use_snr && r.gain_min != s_filter_gain))
        snr_filter_reset(&s_filter);
    s_filter_snr = use_snr;
    s_filter_mhz = mhz;
    s_filter_gain = r.gain_min;
    const float v = use_snr ? snr_estimate_db(r.in_band, floor_in) : snr_db(r.in_band);
    print_reading("SNR_ROW", &r, window, snr_filter_push(&s_filter, v), use_snr);
}
