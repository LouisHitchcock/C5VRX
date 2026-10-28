/* C5VRX-5 by Twotoz and contributors: sampling-phase alignment of the
 * calibration task (boot check, startup qualification, carrier scans and
 * live repair). */
#include "calibration_internal.h"
#include "soc/pcr_struct.h"

/* ---- Sampling phase --------------------------------------------------------
 * PARLIO RX (PLL_F240M/6) and MODEM_DIAG (every 3 ticks of 240 MHz) have a
 * phase fixed at reset; about 3 boots in 10 sample mid-transition (zerowidth
 * PR #3). The fourth-difference excess separates them on noise and on a
 * carrier; a bad phase is slipped one RX tick at a time, only in the
 * startup phase of every boot. Live monitoring is passive; an automatic
 * correction is deferred until an exact bounded phase actuator and verified
 * rollback exist. The existing 1-us divider pulse changes RX/TX separation
 * and is not a proved single-position move.
 *
 * Verdict (sphase_logic.h, test/test_sphase.c): the absolute limit is the
 * fast path; above it only a scan whose positions differ is evidence of a
 * mid-transition read. Positions that all read alike are intrinsic noise
 * (NOISE_LIMITED): no repair, not a phase fault. The stored DC row is held
 * while the gain worker is paused for a scan, and the final qualification
 * runs after the status screen has handed TX back to live video, because
 * the TX context changes the noise the probe sees (board 2026-10-12). */
#define SPHASE_POSITIONS  9u
#define SPHASE_TRIES      12u
int s_sphase_excess = -1;
static int s_sphase_white = -1, s_sphase_dc[2];
static unsigned s_sphase_scans;
static sphase_scan_t s_sphase_last = {.best = 0x7fffffff, .worst = -1};
const char *s_sphase_state = "unverified";

static int sphase_excess(const window_stats_t *w)
{
    int e = dsp_hf4_excess_milli(w->hf4_d4, w->hf4_count);
    return e > 0 ? e : 0;
}

/* The gate metric: absolute excess (HF4/HF2 whiteness is logged only; a
 * normalized gate failed board repeat scans, phasefix6). */
static int sphase_metric(const window_stats_t *w) { return sphase_excess(w); }

int sphase_record(const window_stats_t *w)
{
    s_sphase_excess = sphase_excess(w);
    s_sphase_white = sphase_white_permille(w->hf4_d4, w->hf2_d2);
    s_sphase_dc[0] = w->dc_i;
    s_sphase_dc[1] = w->dc_q;
    return sphase_metric(w);
}

/* positions/windows/tries: startup 9 / 48 / 12 on any signal; on the
 * carrier 3 / 16 / 6 (one cycle, ~0.4 s with the control loop paused),
 * settling within the tight carrier margin of the cleanest position. */
static bool phase_slip(void)
{
    if (!s_probe_deadline) { rx_clock_slip(1); return true; }
    /* Close the splash-check/write race with main's TX handover. This
     * encloses only the existing one-microsecond register pulse, no waits. */
    vTaskSuspendAll();
    const bool allowed = menu_splash_active() && esp_timer_get_time() < s_probe_deadline;
    if (allowed) rx_clock_slip(1);
    (void)xTaskResumeAll();
    return allowed;
}

sphase_verdict_t sphase_scan_n(const char *tag, unsigned positions, unsigned windows, unsigned tries,
                                      bool carrier)
{
    if (!receiver_pause(tag)) return SPHASE_UNSETTLED;
    if (s_probe_deadline && (!menu_splash_active() || esp_timer_get_time() >= s_probe_deadline)) {
        receiver_resume(); return SPHASE_UNSETTLED;
    }
    if (!s_probe_deadline) dco_hold_for_scan();
    window_stats_t w;
    const bool fast = !carrier && positions == 3u;
    sphase_scan_reset(&s_sphase_last);
    for (unsigned pos = 0; pos < positions; ++pos) {
        if (s_probe_deadline && (esp_timer_get_time() >= s_probe_deadline || !menu_splash_active())) break;
        if (pos) { if (!phase_slip()) break; vTaskDelay(pdMS_TO_TICKS(20)); }
        if (collect(windows, &w) && (!fast || w.windows == windows))
            sphase_scan_add(&s_sphase_last, sphase_metric(&w));
    }
    sphase_verdict_t v = SPHASE_UNSETTLED;
    const bool contrast = carrier ? sphase_carrier_contrast(&s_sphase_last) : sphase_contrast(&s_sphase_last);
    if (fast && (s_sphase_last.positions != positions || !contrast)) {
        /* Three alike positions are not enough to establish NOISE_LIMITED;
         * the caller repeats the original nine-position scan. */
    } else if (s_sphase_last.valid && !contrast) {
        /* Every position reads alike: intrinsic noise, slipping cannot help. */
        if (collect(windows, &w))
            v = carrier ? (sphase_record(&w) < SPHASE_LIVE_LIMIT ? SPHASE_SETTLED : SPHASE_NOISE_LIMITED)
                        : sphase_classify(sphase_record(&w), SPHASE_LIMIT, &s_sphase_last);
    } else {
        /* Positions repeat every three ticks: stop on one near the cleanest. */
        for (unsigned n = 0; s_sphase_last.valid && n < tries; ++n) {
            if (s_probe_deadline && (esp_timer_get_time() >= s_probe_deadline || !menu_splash_active())) break;
            if (!collect(windows, &w) || (fast && w.windows != windows)) break;
            int e = sphase_metric(&w);
            const int margin = carrier ? sphase_carrier_margin(s_sphase_last.best) : sphase_margin(s_sphase_last.best);
            if (e <= s_sphase_last.best + margin) {
                if (fast) {
                    const int first = e;
                    if (!collect(windows, &w) || w.windows != windows) break;
                    e = sphase_metric(&w);
                    if (e > s_sphase_last.best + margin || abs(e - first) > margin) break;
                }
                (void)sphase_record(&w);
                v = SPHASE_SETTLED;
                break;
            }
            if (!phase_slip()) break;
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
    receiver_resume();
    printf("SPHASE scan tag=%s positions=%u best=%d worst=%d contrast=%u verdict=%s\n", tag,
           s_sphase_last.positions, s_sphase_last.best, s_sphase_last.worst, contrast, sphase_verdict_name(v));
    return v;
}

static sphase_verdict_t sphase_scan(void)
{
    const sphase_verdict_t quick = sphase_scan_n("SPHASE_QUICK", 3u, 48u, 3u, false);
    if (quick == SPHASE_SETTLED) return quick;
    return sphase_scan_n("SPHASE", SPHASE_POSITIONS, 48u, SPHASE_TRIES, false);
}

static sphase_live_t s_sphase_live = {.gap_us = SPHASE_LIVE_GAP_US};

bool sphase_on_carrier(void)
{
    return sphase_live_carrier(video_recent(), s_current_gain, rf_gain_table()->max_index);
}

/* One carrier scan: choose the cleanest of the three positions. repair:
 * started by an excursion (not the first, proactive choice of the boot). */
void sphase_carrier_scan(const char *stage, bool repair)
{
    ++s_sphase_scans;
    sphase_verdict_t v = sphase_scan_n(repair ? "SPHASE_REPAIR" : "SPHASE_CARRIER", 3u, 16u, 6u, true);
    const bool contrast = sphase_carrier_contrast(&s_sphase_last);
    sphase_live_done(&s_sphase_live, contrast, repair, esp_timer_get_time());
    s_sphase_state = v == SPHASE_SETTLED ? "settled" : v == SPHASE_NOISE_LIMITED ? "noise_limited" : "drifted";
    printf("SPHASE %s carrier_scan repair=%u verdict=%s contrast=%u excess_mc2=%d best=%d worst=%d scans=%u "
           "moved=%u false_alarms=%u next_gap_s=%lld\n", stage, repair, sphase_verdict_name(v), contrast,
           s_sphase_excess, s_sphase_last.best, s_sphase_last.worst, s_sphase_live.scans, s_sphase_live.moved,
           s_sphase_live.false_alarms, s_sphase_live.gap_us / 1000000LL);
}

/* Measure, classify and (startup only) repair with at most two scans. A
 * fresh context drops the previous scan: its positions were measured under
 * another TX/noise context and are no evidence here. */
void sphase_qualify(const char *stage, bool fresh_context)
{
    window_stats_t w;
    if (fresh_context) sphase_scan_reset(&s_sphase_last);
    dco_hold_for_scan();
    if (!collect(48, &w)) {
        s_sphase_state = "probe_failed";
        printf("SPHASE %s state=probe_failed\n", stage);
        return;
    }
    sphase_verdict_t v = sphase_classify(sphase_record(&w), SPHASE_LIMIT, &s_sphase_last);
    for (unsigned n = 0; v == SPHASE_UNSETTLED && n < 2u; ++n) {
        ++s_sphase_scans;
        v = sphase_scan();
    }
    s_sphase_state = sphase_verdict_name(v);
    printf("SPHASE %s state=%s excess_mc2=%d white_pm=%d dc_mcell=%d/%d scans=%u best=%d worst=%d\n",
           stage, s_sphase_state, s_sphase_excess, s_sphase_white, s_sphase_dc[0], s_sphase_dc[1],
           s_sphase_scans, s_sphase_last.valid ? s_sphase_last.best : -1, s_sphase_last.valid ? s_sphase_last.worst : -1);
}

const char *calibration_sphase_state(void) { return s_sphase_state; }
/* NOISE_LIMITED is not a phase fault: every position reads the same noise. */
bool calibration_sphase_settled(void)
{
    return !strcmp(s_sphase_state, "settled") || !strcmp(s_sphase_state, "noise_limited");
}

/* Live sampling-phase repair (operator decision 2026-10-26: a wrong phase
 * found later must fix itself). Board bench, b26ef545, VTX off: 3 of 12 slip
 * positions read mixed IQ bits (excess 4588-6597 against 44-62 clean, the IQ
 * 989-998 per mille clipped: no usable picture), and one boot in five stayed
 * there after the splash. The boot check only aligns once under the splash.
 * Here, three consecutive monitor blocks far above the limit run the existing
 * confirmed scan (three, then nine positions), which ends on the cleanest one.
 * A failed scan backs off 30 s, doubling to 16 min. */
#define PHASE_REPAIR_EXCESS 1500
#define PHASE_REPAIR_BLOCKS 3u
#define PHASE_REPAIR_BACKOFF_US 30000000LL
#define PHASE_REPAIR_BACKOFF_MAX_US 960000000LL
static uint32_t s_phase_repairs, s_phase_repair_fails;
void phase_service(void)
{
    static unsigned bad;
    static int64_t next_us, backoff_us = PHASE_REPAIR_BACKOFF_US;
    static uint32_t blocks;
    if (s_startup || s_boot_phase_pending || !quiet_owner() || rf_lane() != RF_LANE_ULTRAFINE) return;
    if (blocks == s_health_blocks + s_health_rejected) return;           /* no new monitor block */
    blocks = s_health_blocks + s_health_rejected;
    if (s_sphase_excess < PHASE_REPAIR_EXCESS) { bad = 0; return; }
    if (++bad < PHASE_REPAIR_BLOCKS) return;
    const int64_t now = esp_timer_get_time();
    if (now < next_us) return;
    bad = 0;
    const int before = s_sphase_excess;
    ++s_phase_repairs;
    const sphase_verdict_t v = sphase_scan();
    /* NOISE_LIMITED: every position reads the same noise, no slip and no
     * fault (it used to read unverified and stop the DC servo). It keeps the
     * failure backoff: another scan would only pause the receiver again. */
    s_sphase_state = v == SPHASE_SETTLED ? "settled" : v == SPHASE_NOISE_LIMITED ? "noise_limited" : "unverified";
    if (v == SPHASE_SETTLED) {
        backoff_us = PHASE_REPAIR_BACKOFF_US;
        next_us = now + 10000000LL;
    } else {
        ++s_phase_repair_fails;
        next_us = now + backoff_us;
        backoff_us = backoff_us * 2 > PHASE_REPAIR_BACKOFF_MAX_US ? PHASE_REPAIR_BACKOFF_MAX_US : backoff_us * 2;
    }
    printf("SPHASE live_repair excess_before=%d verdict=%s excess_after=%d repairs=%lu fails=%lu\n", before,
           sphase_verdict_name(v), s_sphase_excess, (unsigned long)s_phase_repairs, (unsigned long)s_phase_repair_fails);
}

uint8_t sphase_code(void)
{
    static const char *const names[] = {"unverified", "settled", "drifted", "unsettled", "noise_limited", "probe_failed"};
    for (unsigned k = 0; k < sizeof(names) / sizeof(names[0]); ++k)
        if (!strcmp(s_sphase_state, names[k])) return (uint8_t)k;
    return 0xffu;
}

void cal_sphase_print(void)
{
    printf("SPHASE state=%s excess_mc2=%d limit=%d white_pm=%d dc_mcell=%d/%d scans=%u "
           "best=%d worst=%d contrast=%u rx_div=%lu carrier_qualified=%u live_limit=%d carrier_checks=%u "
           "carrier_scans=%u moved=%u false_alarms=%u strikes=%u\n", s_sphase_state,
           s_sphase_excess, SPHASE_LIMIT, s_sphase_white, s_sphase_dc[0], s_sphase_dc[1], s_sphase_scans,
           s_sphase_last.valid ? s_sphase_last.best : -1, s_sphase_last.valid ? s_sphase_last.worst : -1,
           sphase_contrast(&s_sphase_last), (unsigned long)PCR.parl_clk_rx_conf.parl_clk_rx_div_num + 1ul,
           s_sphase_live.qualified, SPHASE_LIVE_LIMIT, s_sphase_live.carrier_checks, s_sphase_live.scans,
           s_sphase_live.moved, s_sphase_live.false_alarms, s_sphase_live.strikes);
}
