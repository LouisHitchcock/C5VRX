#include "video_internal.h"

static void dco_context(dco_table_blob_t *t, uint16_t freq, uint8_t lo, uint8_t hi);
static bool dco_same_context(const dco_table_blob_t *a, const dco_table_blob_t *b);
static void dco_table_select(uint16_t freq, uint8_t lo, uint8_t hi);
static bool dco_capture_noise_like(void);
static bool dco_noise_measurable(void);
static void rx_recal_now(const char *tag);
#define DCO_RESEARCH_US 120000000LL
/* No carrier = no sync fragment for this long at the table maximum. The idle
 * raster alone is not enough: board 2026-10-06, VTX off at G83, the
 * uncorrected DC (~1.1 fine cell) made receiver noise read q 57-65, so the
 * raster never entered and the search that removes that DC never ran. */
#define DCO_NO_SYNC_US  3000000LL
#define CAL_QUIET_US    5000000LL
int64_t s_quiet_since_us, s_quiet_eval_us;
static bool s_quiet_last;
uint16_t s_rx_recal_freq;
uint32_t s_rx_recal_runs;
/* Envelope test (predemod_envelope_ratio_x100): noise ~100 with or without
 * receiver DC, a carrier at 0 dB SNR ~146 in the host model. */
#define DCO_NOISE_RATIO_X100 115u
volatile int64_t s_last_idle_sync_us;

/* Context identity (review 2026-10-07): codes are reused only under the same
 * frequency, gain table, IQ lane policy, analog filter and IQ-scale context.
 * No boot epoch: a valid measurement survives a reboot. */

#define DCO_TABLE_VERSION 4u /* v4: junk codes below the measurable floor dropped */
RTC_FAST_ATTR dco_table_blob_t s_dco_tab;
static RTC_FAST_ATTR int64_t s_dco_found_us[ARC_VENDOR_GAIN_MAX + 1u];
uint32_t s_dco_searches, s_dco_holds, s_dco_loads, s_dco_saves, s_dco_carrier_refusals;
bool s_dco_dirty;

extern unsigned char phy_param[];

/* Time-spread capture for the carrier test (24 windows, ~6 KB). */
/* CPU-only calibration state lives in LP RAM (RTC_FAST_ATTR): the menu needs
 * 15 x 1536 B of DMA-capable SRAM, and these buffers had cut that to 17 KB
 * (board 2026-10-07: "menu unavailable: ESP_ERR_NO_MEM"). */
RTC_FAST_ATTR uint8_t s_dco_env_buf[24u * RX_PROBE_REGIONS * RX_PROBE_REGION_BYTES];
unsigned s_dco_env_ratio;
uint32_t s_dco_broken_iq, s_dco_hold_aborts;
static unsigned s_dco_hold_bad_ticks;
static uint8_t s_dco_hold_banned[ARC_VENDOR_GAIN_MAX + 1u];
static volatile bool s_dco_ab_off;   /* Ctrl-T RAM A/B switch (dco_ab_toggle) */

/* A DC search needs noise wide enough to dither the quantizer: with less
 * than ~0.6 step of noise nearly every sample sits in two cells per axis,
 * the mean saturates at +-500 mcells and the search flips between its
 * bounds (board 2026-10-07: G60-G66 stored codes from -499 <-> +480
 * iterations). Measurable = at least three cells per axis each hold >= 5 %
 * of a fresh capture. Noise falls with gain, so the first unmeasurable gain
 * ends the sweep for this boot. */
static uint8_t s_dco_floor;            /* lowest measurable gain this boot (0 = unknown) */
static uint32_t s_dco_unmeasurable;

/* rf.c post-gain hook: re-hold the new gain's pair right after every gain
 * write and PHY restore, instead of up to one 250 ms service tick later (and
 * never during DG3_SETTLE). At the range edge V5 writes often, and the edge-BW
 * gear's PHY restore drops the hold: board 2026-10-07, R8, VTX off, DC with
 * the codes "held" from the tick only was a median of ~134 LSB, ~11 LSB with
 * a hook. Same conditions as the service hold, banned gains included (the
 * service's broken-IQ guard still releases and bans); never while a search
 * or lab owns the PHY, so searches start from the vendor row. */
static uint32_t s_dco_hook_calls, s_dco_hook_holds, s_dco_hook_us_max;
static uint64_t s_dco_hook_us_total;

/* Ctrl-T (0x14): RAM A/B switch for the per-gain DC correction (no reboot). */

/* Lab '~': vendor RX DC/IQ calibration at the actual receive frequency
 * (rx_recal.c, ESPARGOS esp-sdr route verified on C5VRX's PHY pin). The
 * vendor calibrates 5 GHz DC only up to 5855 MHz and IQ at 5520 MHz; this
 * measures at the tuned channel instead. Lab only (external research,
 * 2026-10-07: candidate, no RF dB measured). DC is logged before/after.
 * Lab only: run automatically it changed the gain per index and put a fine
 * grain on strong pictures (board 2026-10-07, predemod_dco_service). */

static void dco_context(dco_table_blob_t *t, uint16_t freq, uint8_t lo, uint8_t hi)
{
    const arc_gain_table_t *table = rf_get_arc_gain_table();
    t->version = DCO_TABLE_VERSION;
    t->freq = freq;
    t->lo = lo;
    t->hi = hi;
    t->band5 = table && table->band5;
    t->lane_mode = c5vrx4_fixed_lane();
    t->iq_scale_sel = phy_param[650];   /* phy_rxiq_scale_set() selector */
    t->filter_code = (int8_t)phy_rx_lab_filter_code();
    t->filter_skirt = (int8_t)phy_rx_lab_filter_skirt();
    /* The hold forces only the fine DC DACs (PBUS bank 2); the vendor
     * calibration also sets the coarse ones (bank 1). Codes measured after
     * our recalibration do not fit the stock calibration (board 2026-10-07:
     * NVS codes loaded before a recal left q at 55-64). */
    t->recal = s_rx_recal_freq == freq;
}

static bool dco_same_context(const dco_table_blob_t *a, const dco_table_blob_t *b)
{
    return a->version == b->version && a->freq == b->freq && a->lo == b->lo && a->hi == b->hi &&
           a->band5 == b->band5 && a->lane_mode == b->lane_mode && a->iq_scale_sel == b->iq_scale_sel &&
           a->filter_code == b->filter_code && a->filter_skirt == b->filter_skirt &&
           a->recal == b->recal;
}

static void dco_table_select(uint16_t freq, uint8_t lo, uint8_t hi)
{
    memset(&s_dco_tab, 0, sizeof(s_dco_tab));
    memset(s_dco_found_us, 0, sizeof(s_dco_found_us));
    dco_context(&s_dco_tab, freq, lo, hi);
    dco_table_blob_t saved;
    if (c5vrx4_blob_load("dco_tab", &saved, sizeof(saved)) && dco_same_context(&saved, &s_dco_tab)) {
        s_dco_tab = saved;          /* last measured codes; re-searched when idle */
        ++s_dco_loads;
    }
    dco_context(&s_dco_tab, freq, lo, hi);
}

static bool dco_capture_noise_like(void)
{
    const size_t w = RX_PROBE_REGIONS * RX_PROBE_REGION_BYTES;
    unsigned got = 0;
    for (unsigned tries = 0; tries < 72u && got < 24u; ++tries) {
        vTaskDelay(1);
        if (rx_probe_copy_completed(s_dco_env_buf + got * w)) ++got;
    }
    if (got < 16u) return false;
    /* Quiet means receiver noise, not a broken capture: dead IQ (most
     * samples in the origin cells) or railing IQ also has a flat envelope
     * statistic (board 2026-10-07: DC searches started with the VTX on while
     * the IQ was dead/railing). */
    unsigned origin = 0, rail = 0, n = got * w;
    for (unsigned k = 0; k < n; ++k) {
        int i = predemod_i(s_dco_env_buf[k]), q = predemod_q(s_dco_env_buf[k]);
        origin += (i == 0 || i == -1) && (q == 0 || q == -1);
        rail += i == -8 || i == 7 || q == -8 || q == 7;
    }
    s_dco_env_ratio = predemod_envelope_ratio_x100(s_dco_env_buf, n);
    if (origin * 2u > n || rail * 5u > n) { ++s_dco_broken_iq; return false; }
    return s_dco_env_ratio <= DCO_NOISE_RATIO_X100;
}

static bool dco_noise_measurable(void)
{
    uint8_t buf[RX_PROBE_REGIONS * RX_PROBE_REGION_BYTES];
    unsigned hi_[16] = {0}, hq[16] = {0}, n = 0;
    for (unsigned tries = 0; tries < 24u && n < 8u * sizeof(buf); ++tries) {
        vTaskDelay(1);
        if (!rx_probe_copy_completed(buf)) continue;
        for (unsigned k = 0; k < sizeof(buf); ++k) {
            ++hi_[predemod_i(buf[k]) & 15];
            ++hq[predemod_q(buf[k]) & 15];
        }
        n += sizeof(buf);
    }
    if (n < 4u * sizeof(buf)) return false;
    unsigned ci = 0, cq = 0;
    for (unsigned c = 0; c < 16u; ++c) {
        ci += hi_[c] * 20u >= n;
        cq += hq[c] * 20u >= n;
    }
    return ci >= 3u && cq >= 3u;
}

void dco_post_gain(uint8_t g)
{
    ++s_dco_hook_calls;
    if (s_dco_ab_off || !c5vrx4_hw_dco_enabled() || rf_native_agc_active() ||
        s_rx_profile != RX_PROFILE_DIRECT_GAIN || s_agc_mode != ANALOG_AGC_ACTIVE ||
        s_rssi_probe_active || s_pre_q4_probe_active) return;
    if (g < s_dco_tab.lo || g > s_dco_tab.hi || g < s_dco_floor || !s_dco_tab.e[g].valid || s_dco_hold_banned[g] ||
        s_dco_tab.freq != rf_get_frequency_mhz()) return;
    const int64_t t0 = esp_timer_get_time();
    if (phy_rx_lab_dco_hold_quiet(s_dco_tab.e[g].code[0], s_dco_tab.e[g].code[1]) == ESP_OK)
        ++s_dco_hook_holds;
    const uint32_t us = (uint32_t)(esp_timer_get_time() - t0);
    s_dco_hook_us_total += us;
    if (us > s_dco_hook_us_max) s_dco_hook_us_max = us;
}

void dco_hook_print(const char *tag)
{
    printf("%s correction=%s gain_writes=%lu hook_holds=%lu hold_us_avg=%lu hold_us_max=%lu "
           "service_holds=%lu gain=%u\n", tag, s_dco_ab_off ? "off" : "on",
           (unsigned long)s_dco_hook_calls, (unsigned long)s_dco_hook_holds,
           (unsigned long)(s_dco_hook_holds ? s_dco_hook_us_total / s_dco_hook_holds : 0u),
           (unsigned long)s_dco_hook_us_max, (unsigned long)s_dco_holds, s_current_gain);
}

void dco_ab_toggle(void)
{
    s_dco_ab_off = !s_dco_ab_off;
    if (s_dco_ab_off) (void)phy_rx_lab_dco_release();
    rf_set_rx_gain(true, s_current_gain); /* vendor row, or re-held by the hook */
    dco_hook_print("DCO_AB");
}

static void rx_recal_now(const char *tag)
{
    if (!rx_recal_supported()) { printf("%s refused=unverified_PHY_binary\n", tag); return; }
    if (rf_native_agc_active()) { printf("%s refused=native_agc\n", tag); return; }
    analog_agc_mode_t saved;
    if (!predemod_pause(tag, &saved)) return;
    ++s_rx_recal_runs;
    const uint16_t mhz = rf_get_frequency_mhz();
    const uint8_t gain = s_current_gain;
    (void)phy_rx_lab_dco_release();
    vTaskDelay(pdMS_TO_TICKS(30));
    predemod_window_t w;
    bool ok0 = predemod_collect(64, &w);
    int before[2] = {w.dc_i, w.dc_q};
    phy_rx_lab_begin("rx_recal");
    int64_t t0 = esp_timer_get_time();
    rx_recal_run(mhz);
    int64_t took = esp_timer_get_time() - t0;
    phy_rx_lab_end();
    /* Full restore: retune (restore lock, filters, AGC patch, vendor table
     * capture -> new arc generation) and the gain that was active. */
    esp_err_t err = rf_set_channel(rf_get_channel_index());
    rf_set_rx_gain(true, gain);
    vTaskDelay(pdMS_TO_TICKS(50));
    bool ok1 = predemod_collect(64, &w);
    phy_rx_lab_dco_invalidate();
    /* The per-gain table holds ABSOLUTE DC-DAC codes (forced in PBUS debug
     * mode), independent of the vendor's own codes: it stays valid. */
    s_rx_recal_freq = mhz;
    predemod_resume(saved);
    s_cal_settle_until_us = esp_timer_get_time() + CAL_SETTLE_US;
    printf("%s mhz=%u gain=%u took_us=%lld restore=%s dc_before_mcells=%d/%d dc_after_mcells=%d/%d "
           "valid=%u/%u runs=%lu hardware_acceptance=pending\n",
           tag, mhz, gain, took, esp_err_to_name(err), before[0], before[1], w.dc_i, w.dc_q, ok0, ok1,
           (unsigned long)s_rx_recal_runs);
}

void lab_run_rx_recal(void) { rx_recal_now("RX_RECAL"); }

void predemod_dco_service(void)
{
    if (s_dco_ab_off || !c5vrx4_hw_dco_enabled() || rf_native_agc_active() ||
        s_rx_profile != RX_PROFILE_DIRECT_GAIN || s_agc_mode != ANALOG_AGC_ACTIVE ||
        s_rssi_probe_active || s_pre_q4_probe_active ||
        phy_rx_lab_busy() || (s_menu_active && !IDLE_RASTER_ACTIVE())) return;
    const arc_gain_table_t *table = rf_get_arc_gain_table();
    if (!table) return;
    const uint16_t freq = rf_get_frequency_mhz();
    const uint8_t lo = rf_get_arc_survival_gain(), hi = table->max_index;
    if (freq != s_dco_tab.freq || lo != s_dco_tab.lo || hi != s_dco_tab.hi) {
        (void)phy_rx_lab_dco_release();
        phy_rx_lab_dco_invalidate();
        dco_table_select(freq, lo, hi);
        s_dco_floor = 0;                /* re-learned on the new channel */
    }
    const int64_t now = esp_timer_get_time();
    /* Automatic calibration only in CONFIRMED quiet (operator 2026-10-07:
     * everything automatic, nothing odd between antenna swaps): the carrier
     * test below runs at most once a second, and calibration starts only
     * after CAL_QUIET_US of uninterrupted quiet - a swap or a short loss of
     * signal never triggers it. */
    bool no_carrier = false;
    if (IDLE_RASTER_ACTIVE()) {
        no_carrier = true;
    } else if (now - s_last_idle_sync_us > DCO_NO_SYNC_US && now - s_quiet_eval_us < 1000000LL) {
        no_carrier = s_quiet_last;      /* rate-limited carrier test result */
    } else if (now - s_last_idle_sync_us > DCO_NO_SYNC_US) {
        /* Independent of V5's state and of the DC: with the DC uncorrected V5
         * hunted G82<->G83 on noise, and the two-capture DC agreement then
         * failed on the gain change (board 2026-10-07). The envelope test
         * removes the capture's own DC and is the carrier test. */
        s_quiet_eval_us = now;
        /* No sync is not proof of no carrier: a weak FM carrier below sync
         * detection would bias the DC estimate (review 2026-10-07). Real
         * receiver DC is constant; a carrier rotates and moves the mean, so
         * two estimates 100 ms apart must agree. */
        predemod_window_t a, b;
        if (predemod_collect(32, &a)) {
            vTaskDelay(pdMS_TO_TICKS(100));
            if (predemod_collect(32, &b)) {
                int di = a.dc_i - b.dc_i, dq = a.dc_q - b.dc_q;
                int mag = (abs(a.dc_i) + abs(a.dc_q) + abs(b.dc_i) + abs(b.dc_q)) / 2;
                int spread = abs(di) + abs(dq);
                (void)spread; (void)mag;
                no_carrier = dco_capture_noise_like();
                if (!no_carrier) ++s_dco_carrier_refusals;
            }
        }
        s_quiet_last = no_carrier;
    }
    if (!no_carrier) s_quiet_since_us = 0;
    else if (!s_quiet_since_us) s_quiet_since_us = now;
    no_carrier = no_carrier && now - s_quiet_since_us >= CAL_QUIET_US;
    /* No automatic vendor RX recalibration at the tuned frequency. Board
     * 2026-10-07, A/B with the VTX on: after the quiet-gated recal the same
     * gain index gave a far larger envelope (G62: P50 113 / 100 % clip, main
     * P50 23) and a fine grain over the whole strong picture; a boot that
     * skipped it gave main's gains (G31-33, P50 13-23) and a clean picture.
     * phy_set_rx_gain_table() reinstalls the gain memory with the fresh
     * corrections, so the recal changes the gain per index. '~' keeps it as
     * a lab. */
    if (no_carrier) {
        /* Next stale gain, maximum first, down to the measurable floor. */
        int target = -1;
        const int dco_low = s_dco_floor > lo ? s_dco_floor : lo;
        for (int g = hi; g >= dco_low; --g)
            if (!s_dco_found_us[g] || now - s_dco_found_us[g] > DCO_RESEARCH_US) { target = g; break; }
        if (target >= 0) {
            analog_agc_mode_t saved;
            if (!predemod_pause("DCO_AUTO", &saved)) return;
            const uint8_t restore = s_current_gain;
            (void)phy_rx_lab_dco_release();
            /* Only this search's result may count (review 2026-10-07: a failed
             * search returned the codes loaded for another gain's hold). */
            phy_rx_lab_dco_invalidate();
            rf_set_rx_gain(true, (uint8_t)target);
            vTaskDelay(pdMS_TO_TICKS(20));
            if (!dco_noise_measurable()) {
                /* This and every lower gain: no search, no stored codes. */
                for (int g = target; g >= (int)lo; --g) s_dco_tab.e[g].valid = 0;
                s_dco_floor = (uint8_t)(target + 1);
                s_dco_dirty = true;
                ++s_dco_unmeasurable;
                phy_rx_lab_dco_invalidate();
                rf_set_rx_gain(true, restore);
                predemod_resume(saved);
                printf("DCO_AUTO floor gain=%d unmeasurable (noise < quantizer step): searches G%d..G%u\n",
                       target, target + 1, hi);
                return;
            }
            phy_rx_lab_dco_result_t res;
            esp_err_t result = phy_rx_lab_dco_search(lab_dco_measure, lab_dco_quiet, &res);
            /* Only this search's own measured codes enter the cache; the
             * rollback status is separate (a correction can measure fine while
             * work mode does not read back at once). */
            bool found = res.measured;
            int codes[2] = {res.codes[0], res.codes[1]};
            if (found) {
                s_dco_tab.e[target] = (dco_entry_t){
                    {(int16_t)codes[0], (int16_t)codes[1]},
                    {(int16_t)res.residual_mcells[0], (int16_t)res.residual_mcells[1]}, 1u};
                s_dco_dirty = true;
            }
            s_dco_found_us[target] = now;   /* also a failed search waits 120 s */
            s_cal_settle_until_us = esp_timer_get_time() + CAL_SETTLE_US;
            phy_rx_lab_dco_invalidate();
            rf_set_rx_gain(true, restore);  /* work mode replays the row on a write */
            predemod_resume(saved);
            ++s_dco_searches;
            printf("DCO_AUTO search=%lu gain=%d result=%d found=%u codes=%d/%d freq=%u\n",
                   (unsigned long)s_dco_searches, target, (int)result, found,
                   found ? codes[0] : -1, found ? codes[1] : -1, freq);
            return;
        }
        /* The whole stage is fresh: persist once (flash wear: on change only). */
        if (s_dco_dirty && c5vrx4_blob_store("dco_tab", &s_dco_tab, sizeof(s_dco_tab))) {
            s_dco_dirty = false;
            ++s_dco_saves;
        }
    }
    const uint8_t g = s_current_gain;
    /* Hold guard: the held DC codes must never coincide with dead or railing
     * IQ. Three ticks of it with the hold active release the hold and keep
     * this gain unheld for the rest of the boot. */
    if (phy_rx_lab_dco_held()) {
        bool broken = s_v3_origin_pm >= 900 || s_v3_clip_pm >= 500;
        s_dco_hold_bad_ticks = broken ? s_dco_hold_bad_ticks + 1u : 0u;
        if (s_dco_hold_bad_ticks >= 3u && g <= ARC_VENDOR_GAIN_MAX) {
            (void)phy_rx_lab_dco_release();
            s_dco_hold_banned[g] = 1u;
            s_dco_hold_bad_ticks = 0;
            ++s_dco_hold_aborts;
            printf("DCO_HOLD abort gain=%u origin_pm=%d clip_pm=%d (hold disabled for this gain)\n",
                   g, s_v3_origin_pm, s_v3_clip_pm);
            return;
        }
    }
    if (g >= lo && g <= hi && g >= s_dco_floor && s_dco_tab.e[g].valid && !s_dco_hold_banned[g] &&
        !phy_rx_lab_dco_held() &&
        s_direct_gain_v3.state != DG3_SETTLE) {
        phy_rx_lab_dco_load(s_dco_tab.e[g].code[0], s_dco_tab.e[g].code[1]);
        if (phy_rx_lab_dco_set(true) == ESP_OK) ++s_dco_holds;
    }
}

volatile int64_t s_cal_settle_until_us;
