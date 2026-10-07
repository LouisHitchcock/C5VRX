/* C5VRX-4: calibration responsibilities. */
#include "video_internal.h"

static bool bw_noise_measure(unsigned windows, unsigned *width_khz, int *q_phase, int *clip_pm);
static bool bw_quiet(const char *stage, unsigned width, int q_phase, int clip_pm);
static void bw_skirt_stage(uint8_t code0, unsigned target);
static void bw_edge_stage(void);
static void agc_witness_autocheck(void);
static bool predemod_quiet_owner(void);
static void predemod_dg3_map_service(void);
static void predemod_dc_service(void);
static void predemod_bw_autocal(void);
#define BW_EDGE_SKIRTS 4u

#define BW_EDGE_CODES 9u

#define BW_AUTO_RETRY_US    60000000

#define BW_AUTO_QUIET_TICKS 12u

#define SPHASE_AUTO_PPM    5000u

#define DC_MIN_GAP_US      10000000

#define DC_LIMIT_MCELLS    3000

#define DC_STEP_MCELLS     120

#define DC_AGREE_MCELLS    150

#define DC_MIN_WINDOWS     600u

#define WITNESS_WINDOWS 24u

#define BW_EDGE_CANDIDATES (2u * BW_EDGE_CODES + BW_EDGE_SKIRTS)

#define BW_SKIRT_STAGES 5u

#define BW_CAL_QUIET_CLIP  50

#define BW_CAL_QUIET_QPHASE 34 /* V5 coherence < 25 */

#define BW_CAL_STEP        4

#define BW_CAL_WINDOWS     96u

/* Digital DC recentring evidence, reset whenever gain, lane or profile move. */
static portMUX_TYPE s_dc_mux = portMUX_INITIALIZER_UNLOCKED;

static int64_t s_dc_sum_i, s_dc_sum_q;

static uint32_t s_dc_windows, s_dc_epoch;

/* Sampling phase (#165 P0). PARLIO RX runs PLL_F240M/6 while MODEM_DIAG
 * changes every 3 ticks of 240 MHz; their relation is fixed at reset. Holding
 * the divider one count higher for ~1 us moves the RX edge by a few 4.17-ns
 * ticks (zerowidth PR #3 does this on its PARLIO TX sample clock). The ring
 * loses a few samples once; producer/consumer block separation is kept. */
static portMUX_TYPE s_slip_mux = portMUX_INITIALIZER_UNLOCKED;

/* ---- Fixed optimal analog bandwidth -------------------------------------
 * Replaces the BW20/BW40 gear, whose phy_wifi_fbw_sel() only moves the
 * digital filter. Built on ESPARGOS esp-sdr's C5 BANDWIDTH control (GPL-3.0,
 * commit ac627b0b): an absolute 6-bit code in the RX0 capacitor DAC (BBTOP
 * 0x67 regs 6/7), curves from median noise FFTs, mode 0 = 11-23 MHz, mode 1 =
 * 22-48 MHz, a mode change needs a full channel setup. zerowidth/C5VRX PR #3
 * measured the hardware benefit of a narrower filter (phy_11p_set on R8:
 * sync-tip noise ~102-105 -> ~87-91 kHz); C5VRX's own WIFI_BW20 test lost
 * detail and chroma, so the target stays at a 24 MHz FPV FM channel.
 * Which mode C5VRX's BW40-config/secondary-NONE tune lands in is not proven,
 * so this chip's noise width is measured with no carrier at maximum gain for
 * the calibrated bytes and codes 0..60 (64-point PSD over the observer
 * regions), the result is matched against both esp-sdr curves, and the
 * narrowest code still >= bw_target is kept (the widest if none reaches it).
 * Measured once (automatically, see predemod_task) and stored; '=' repeats
 * it. The VTX must be off. */

static const char *s_bw_cal_result = "never";

static unsigned s_bw_cal_runs;

static unsigned s_bw_last_nbw_khz;

static int s_bw_mode_fit = -1;

static unsigned s_bw_fit_err_khz, s_bw_calibrated_width_khz;

/* ---- Default-on pre-demodulation correction (#165) ----------------------
 * Low-priority task, never in the 40 MS/s path. (1) Digital DC recentring:
 * average the raw I/Q centre per gain/lane epoch, require two agreeing
 * evaluations, move >=0.12 cell, at most every 2 s, and rewrite only the
 * static decoder banks. (2) One sampling-phase check at the first stable
 * carrier lock; it slips the RX clock only when mid-transition reads are
 * clearly present (zerowidth PR #3 saw ~3 bad boots in 10). */

/* Every decoder-bank rewrite is a live LUT write (HDZero, 2026-10-04): DC
 * drifts thermally over minutes, so 10 s between writes loses nothing. */

static predemod_dc_filter_t s_dc_filter;

static uint32_t s_dc_filter_epoch, s_dc_evaluations, s_dc_refusals;

static int s_dc_measured[2];

static int64_t s_dc_last_write_us;

/* First-boot fixed-BW calibration: only while uncalibrated, after 3 s of
 * table-maximum listening with no carrier (the V5 no-carrier state), and at
 * most once a minute if a carrier interrupts it. */

static bool s_bw_autocal_tried;

bool predemod_collect(unsigned windows, predemod_window_t *out)
{
    uint8_t sample[RX_PROBE_REGIONS * RX_PROBE_REGION_BYTES];
    int64_t si = 0, sq = 0;
    memset(out, 0, sizeof(*out));
    for (unsigned tries = 0; tries < windows * 3u && out->windows < windows; ++tries) {
        /* Two ticks: the window analysis below costs about one, so a 1-tick
         * wait kept this task runnable nearly all the time and, with the V5
         * observer, IDLE on CPU0 starved (board 2026-10-07: task watchdog
         * here during the boot SPHASE scan). */
        vTaskDelay(2);
        if (!rx_probe_copy_completed(sample)) continue;
        for (unsigned r = 0; r < RX_PROBE_REGIONS; ++r)
            out->glitches += predemod_glitches(sample + r * RX_PROBE_REGION_BYTES,
                                               RX_PROBE_REGION_BYTES, 6);
        out->samples += RX_PROBE_REGIONS * (RX_PROBE_REGION_BYTES - 2u);
        int di, dq;
        predemod_dc_mcells(sample, sizeof(sample), &di, &dq);
        si += di; sq += dq;
        out->m = analyze_control_window(sample, sizeof(sample), 0);
        ++out->windows;
    }
    if (!out->windows) return false;
    out->dc_i = (int)(si / (int64_t)out->windows);
    out->dc_q = (int)(sq / (int64_t)out->windows);
    return out->windows * 2u >= windows;
}

unsigned predemod_ppm(uint32_t glitches, uint32_t samples)
{
    return samples ? (unsigned)((uint64_t)glitches * 1000000u / samples) : 0u;
}

void predemod_print(const char *tag, const char *stage, int extra,
                           const predemod_window_t *w)
{
    unsigned step = 64u >> rf_get_iq_lanes();
    printf("%s stage=%s arg=%d freq=%u G=%u lane=%u windows=%u glitch_ppm=%u "
           "dc_mcells=%d/%d dc_codes=%d/%d P50=%d Q_phase=%d outer_pm=%d origin_pm=%d "
           "video=hardware_pending\n",
           tag, stage, extra, rf_get_frequency_mhz(), s_current_gain, rf_get_iq_lanes(),
           w->windows, predemod_ppm(w->glitches, w->samples), w->dc_i, w->dc_q,
           w->dc_i * (int)step / 1000, w->dc_q * (int)step / 1000, w->m.p_median,
           w->m.q_phase, w->m.clip_permille, w->m.origin_permille);
}

bool predemod_pause(const char *tag, analog_agc_mode_t *saved_mode)
{
    /* These labs use RX and gain only: the idle raster may keep TX. */
    if (rf_native_agc_active() ||
        (s_menu_active && !IDLE_RASTER_ACTIVE()) ||
        s_pre_q4_probe_active || s_rssi_probe_active) {
        printf("%s refused=other_lab_menu_or_native_owner\n", tag);
        return false;
    }
    *saved_mode = s_agc_mode;
    s_rssi_probe_active = true;
    s_agc_mode = ANALOG_AGC_MANUAL;
    vTaskDelay(pdMS_TO_TICKS(100));
    return true;
}

void predemod_resume(analog_agc_mode_t saved_mode)
{
    ++s_profile_generation;
    s_agc_mode = saved_mode;
    s_rssi_probe_active = false;
}

void rx_clock_slip(uint32_t us)
{
    portENTER_CRITICAL(&s_slip_mux);
    uint32_t div = PCR.parl_clk_rx_conf.parl_clk_rx_div_num;
    HAL_FORCE_MODIFY_U32_REG_FIELD(PCR.parl_clk_rx_conf, parl_clk_rx_div_num, div + 1u);
    esp_rom_delay_us(us);
    HAL_FORCE_MODIFY_U32_REG_FIELD(PCR.parl_clk_rx_conf, parl_clk_rx_div_num, div);
    portEXIT_CRITICAL(&s_slip_mux);
}

static bool bw_noise_measure(unsigned windows, unsigned *width_khz, int *q_phase, int *clip_pm)
{
    float psd[PREDEMOD_FFT_N] = {0};
    uint8_t sample[RX_PROBE_REGIONS * RX_PROBE_REGION_BYTES];
    unsigned got = 0;
    int q_sum = 0, clip_max = 0;
    for (unsigned tries = 0; tries < windows * 3u && got < windows; ++tries) {
        vTaskDelay(1);
        if (!rx_probe_copy_completed(sample)) continue;
        for (unsigned r = 0; r < RX_PROBE_REGIONS; ++r)
            predemod_psd_accumulate(sample + r * RX_PROBE_REGION_BYTES, psd);
        control_metrics_t m = analyze_control_window(sample, sizeof(sample), 0);
        q_sum += m.q_phase;
        if (m.clip_permille > clip_max) clip_max = m.clip_permille;
        ++got;
    }
    if (got * 2u < windows) return false;
    *width_khz = predemod_psd_width_khz(psd);
    s_bw_last_nbw_khz = predemod_psd_nbw_khz(psd);
    *q_phase = q_sum / (int)got;
    *clip_pm = clip_max;
    return true;
}

static bool bw_quiet(const char *stage, unsigned width, int q_phase, int clip_pm)
{
    bool quiet = q_phase < BW_CAL_QUIET_QPHASE && clip_pm < BW_CAL_QUIET_CLIP;
    if (!quiet)
        printf("BW_CAL stage=%s refused=carrier_or_overload Q_phase=%d clip_pm=%d width_khz=%u\n",
               stage, q_phase, clip_pm, width);
    return quiet;
}

/* Second stage (2026-10-04). PARLIO keeps every second sample of the ~80 MS/s
 * bus unfiltered, so what the 40 MS/s view loses is the noise bandwidth, not
 * the -3 dB width: a single RC stage at 24 MHz still folds its skirt into the
 * band (host model: +0.2..1.1 dB at 24 MHz, +1.8..3.2 dB at 35..48 MHz,
 * 1st..3rd order). Narrow regs 8..13 as well, re-open regs 6/7 until the
 * width covers the target again, and keep the lowest measured noise
 * bandwidth (folded noise included) if it is >= 0.3 dB better. If those
 * registers are not in this receive path, nothing changes and skirt 0 stays.
 * Called with the stored single-stage code applied, VTX off. */

static void bw_skirt_stage(uint8_t code0, unsigned target)
{
    static const uint8_t skirts[BW_SKIRT_STAGES] = {0, 8, 16, 24, 32};
    unsigned nbw[BW_SKIRT_STAGES] = {0}, width[BW_SKIRT_STAGES] = {0};
    bool quiet[BW_SKIRT_STAGES] = {false};
    uint8_t code[BW_SKIRT_STAGES] = {0};
    unsigned measured = 0;
    for (unsigned k = 0; k < BW_SKIRT_STAGES; ++k) {
        if (!phy_rx_lab_filter_set_skirt(skirts[k])) { printf("BW_SKIRT skirt=%u refused=filter_write\n", skirts[k]); break; }
        int c = code0;
        unsigned w = 0; int q = 0, clip = 0;
        for (;;) {
            if (!phy_rx_lab_filter_set_code(c)) { c = -1; break; }
            vTaskDelay(pdMS_TO_TICKS(20));
            if (!bw_noise_measure(BW_CAL_WINDOWS, &w, &q, &clip)) { c = -1; break; }
            if (w >= target || c == 0) break;
            c = c > 4 ? c - 4 : 0; /* lower code = wider */
        }
        if (c < 0) { printf("BW_SKIRT skirt=%u refused=sample_or_write\n", skirts[k]); break; }
        code[k] = (uint8_t)c; width[k] = w; nbw[k] = s_bw_last_nbw_khz;
        quiet[k] = q < BW_CAL_QUIET_QPHASE && clip < BW_CAL_QUIET_CLIP;
        ++measured;
        printf("BW_SKIRT skirt=%u code=%d width_khz=%u nbw_khz=%u excess_db_x10=%d Q_phase=%d clip_pm=%d\n",
               skirts[k], c, w, nbw[k], predemod_nbw_excess_db_x10(nbw[k], w), q, clip);
        if (clip >= BW_CAL_QUIET_CLIP) break; /* a carrier or overload appeared */
    }
    int choice = predemod_skirt_choose(nbw, width, quiet, measured, target);
    bool stored = false;
    if (choice >= 0) {
        stored = phy_rx_lab_filter_set_code(code[choice]) && phy_rx_lab_filter_set_skirt(skirts[choice]) &&
                 c5vrx4_bw_store(code[choice], width[choice]) && c5vrx4_bw_skirt_store(skirts[choice], nbw[choice]);
        printf("BW_SKIRT chosen skirt=%u code=%u width_khz=%u nbw_khz=%u gain_vs_single_db_x10=%d stored=%u\n",
               skirts[choice], code[choice], width[choice], nbw[choice],
               predemod_nbw_excess_db_x10(nbw[0], nbw[choice]), stored);
    }
    if (!stored) {
        /* Back to the single-stage code that was just stored. */
        bool ok = phy_rx_lab_filter_set_code(code0) && phy_rx_lab_filter_set_skirt(0) &&
                  c5vrx4_bw_skirt_store(0, measured ? nbw[0] : 0);
        printf("BW_SKIRT kept=single_stage code=%u restore_verified=%u\n", code0, ok);
    }
}

/* Edge profile stage (2026-10-05). VTX off, maximum gain, normal code and
 * skirt stored. Sweeps the analog code with the digital filter in BW40 and
 * BW20 (skirt kept) and stores the lowest-noise-bandwidth setting that still
 * covers PREDEMOD_EDGE_TARGET_KHZ, keeps receiver noise incoherent for V5
 * NO_CARRIER and is >= 0.5 dB better than normal (predemod_edge_choose). The
 * V5 gear uses it only at the range edge. Measured, so it does not depend on
 * whether the digital filter or the analog mode sits ahead of the tap.
 * Ends on BW40 + the normal code; bw_enbw = best candidate nbw (1 = none
 * valid, 0 = never measured). */

/* Second-stage candidates (regs 8..13) at the normal code and BW40. On the
 * first board (2026-10-06) regs 6/7 and the digital BW20/BW40 choice did not
 * move the measured width (19.4 MHz, tap ahead of the digital filter), but
 * this stage did (skirt 16: 15.6 MHz wide, nbw 20.9 -> 16.9 MHz). */

static void bw_edge_stage(void)
{
    static const uint8_t codes[BW_EDGE_CODES] = {0, 8, 16, 24, 32, 40, 48, 56, 60};
    static const uint8_t skirts[BW_EDGE_SKIRTS] = {8, 16, 24, 32};
    unsigned nbw[BW_EDGE_CANDIDATES] = {0}, width[BW_EDGE_CANDIDATES] = {0};
    bool valid[BW_EDGE_CANDIDATES] = {false};
    const unsigned normal = c5vrx4_bw_nbw_khz();
    bool carrier = false;
    unsigned best_nbw = 0;
    for (unsigned dig = 0; dig < 2u && !carrier; ++dig) {
        apply_rf_bandwidth(dig == 0u); /* restore applies the normal code + skirt */
        for (unsigned k = 0; k < BW_EDGE_CODES; ++k) {
            unsigned i = dig * BW_EDGE_CODES + k, w = 0;
            int q = 0, clip = 0;
            if (!phy_rx_lab_filter_set_code(codes[k])) {
                printf("BW_EDGE digital=%s code=%u refused=filter_write\n", dig ? "BW20" : "BW40", codes[k]);
                continue;
            }
            vTaskDelay(pdMS_TO_TICKS(20));
            if (!bw_noise_measure(BW_CAL_WINDOWS, &w, &q, &clip)) continue;
            width[i] = w;
            nbw[i] = s_bw_last_nbw_khz;
            valid[i] = q < BW_CAL_QUIET_QPHASE && clip < BW_CAL_QUIET_CLIP;
            if (valid[i] && w >= PREDEMOD_EDGE_TARGET_KHZ && (!best_nbw || nbw[i] < best_nbw))
                best_nbw = nbw[i];
            printf("BW_EDGE digital=%s code=%u width_khz=%u nbw_khz=%u gain_db_x10=%d Q_phase=%d clip_pm=%d valid=%u\n",
                   dig ? "BW20" : "BW40", codes[k], w, nbw[i],
                   -predemod_nbw_excess_db_x10(nbw[i], normal), q, clip, valid[i]);
            /* A carrier switched on mid-sweep shows up on wide settings too. */
            if (clip >= BW_CAL_QUIET_CLIP) carrier = true;
        }
    }
    apply_rf_bandwidth(true);
    for (unsigned k = 0; k < BW_EDGE_SKIRTS && !carrier; ++k) {
        unsigned i = 2u * BW_EDGE_CODES + k, w = 0;
        int q = 0, clip = 0;
        if (!phy_rx_lab_filter_set_skirt(skirts[k])) {
            printf("BW_EDGE skirt=%u refused=filter_write\n", skirts[k]);
            continue;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
        if (!bw_noise_measure(BW_CAL_WINDOWS, &w, &q, &clip)) continue;
        width[i] = w;
        nbw[i] = s_bw_last_nbw_khz;
        valid[i] = q < BW_CAL_QUIET_QPHASE && clip < BW_CAL_QUIET_CLIP;
        if (valid[i] && w >= PREDEMOD_EDGE_TARGET_KHZ && (!best_nbw || nbw[i] < best_nbw))
            best_nbw = nbw[i];
        printf("BW_EDGE skirt=%u code=%u width_khz=%u nbw_khz=%u gain_db_x10=%d Q_phase=%d clip_pm=%d valid=%u\n",
               skirts[k], c5vrx4_bw_code(), w, nbw[i],
               -predemod_nbw_excess_db_x10(nbw[i], normal), q, clip, valid[i]);
        if (clip >= BW_CAL_QUIET_CLIP) carrier = true;
    }
    (void)phy_rx_lab_filter_set_skirt((int)c5vrx4_bw_skirt());
    unsigned w = 0;
    int q = 0, clip = 0;
    bool restored = phy_rx_lab_filter_code() == (int)c5vrx4_bw_code();
    if (!carrier && bw_noise_measure(BW_CAL_WINDOWS, &w, &q, &clip))
        carrier = !bw_quiet("EDGE_POSTCHECK", w, q, clip);
    int choice = carrier ? -1 : predemod_edge_choose(nbw, width, valid, BW_EDGE_CANDIDATES,
                                                     PREDEMOD_EDGE_TARGET_KHZ, normal);
    const bool by_skirt = choice >= (int)(2u * BW_EDGE_CODES);
    const int edge_code = choice < 0 ? -1 : by_skirt ? (int)c5vrx4_bw_code() : (int)codes[choice % BW_EDGE_CODES];
    const bool edge_bw20 = !by_skirt && choice >= (int)BW_EDGE_CODES;
    bool stored = choice >= 0
        ? c5vrx4_bw_edge_store((uint8_t)edge_code, edge_bw20, nbw[choice])
        : c5vrx4_bw_edge_store(C5VRX4_BW_UNCALIBRATED, false, carrier ? 0u : (best_nbw ? best_nbw : 1u));
    stored = c5vrx4_bw_edge_skirt_store(by_skirt ? skirts[choice - (int)(2u * BW_EDGE_CODES)]
                                                 : C5VRX4_BW_EDGE_SKIRT_NONE) && stored;
    printf("BW_EDGE chosen=%s code=%d digital=%s skirt=%d nbw_khz=%u normal_nbw_khz=%u gain_db_x10=%d "
           "target_khz=%u stored=%u restore_verified=%u\n",
           carrier ? "aborted_carrier" : choice >= 0 ? "edge_profile" : "none_0p5dB_better",
           edge_code, edge_bw20 ? "BW20" : "BW40",
           by_skirt ? (int)skirts[choice - (int)(2u * BW_EDGE_CODES)] : -1,
           choice >= 0 ? nbw[choice] : 0u, normal,
           choice >= 0 ? -predemod_nbw_excess_db_x10(nbw[choice], normal) : 0,
           PREDEMOD_EDGE_TARGET_KHZ, stored, restored);
}

/* Returns true when a code was measured and stored. Caller context: a task
 * that may block for a few seconds (console or predemod_task). */
bool lab_run_bw_calibration(bool automatic)
{
    ++s_bw_cal_runs;
    if (!c5vrx4_fixed_bw_enabled()) { printf("BW_CAL refused=fixed_bw_disabled ('^')\n"); return false; }
    /* The AUTO gear's narrow setting is left first; a manual BW20 is refused. */
    if (rf_fixed_bw_edge_active()) bw_set_edge(false);
    if (!s_current_bw40 && s_rf_bw_mode == RF_BW_MODE_AUTO) apply_rf_bandwidth(true);
    if (!s_current_bw40) { printf("BW_CAL refused=digital_bw20_selected\n"); return false; }
    if (phy_rx_lab_filter_calibrated_code() < 0) {
        printf("BW_CAL refused=no_calibrated_filter_bytes_or_unpinned_PHY\n");
        s_bw_cal_result = "unsupported";
        return false;
    }
    analog_agc_mode_t saved_mode;
    if (!predemod_pause("BW_CAL", &saved_mode)) { s_bw_cal_result = "busy"; return false; }
    const arc_gain_table_t *table = rf_get_arc_gain_table();
    const uint8_t saved_gain = s_current_gain;
    lab_apply_vendor_gain(table->max_index);
    vTaskDelay(pdMS_TO_TICKS(50));
    const uint8_t previous = c5vrx4_bw_code();
    const int restore = previous == C5VRX4_BW_UNCALIBRATED ? PHY_RX_LAB_FILTER_CALIBRATED : (int)previous;
    const unsigned target = c5vrx4_bw_target_khz();
    printf("BW_CAL begin mode=%s freq=%u gain=%u lane=%u target_khz=%u calibrated_code=%d previous=%d "
           "reference=ESPARGOS_esp-sdr_ac627b0b\n",
           automatic ? "auto" : "manual", rf_get_frequency_mhz(), s_current_gain, rf_get_iq_lanes(),
           target, phy_rx_lab_filter_calibrated_code(),
           previous == C5VRX4_BW_UNCALIBRATED ? -1 : (int)previous);
    uint8_t codes[60 / BW_CAL_STEP + 1];
    unsigned widths[60 / BW_CAL_STEP + 1];
    int noise_q[60 / BW_CAL_STEP + 1];
    unsigned count = 0, width = 0;
    int q_phase = 0, clip_pm = 0;
    phy_rx_lab_begin("BW_CAL");
    /* Carrier pre-check on the calibrated bytes; narrower noise is legitimately
     * more coherent, so Q_phase is judged only here and in the re-check. */
    bool applied = phy_rx_lab_filter_set_code(PHY_RX_LAB_FILTER_CALIBRATED);
    bool quiet = false;
    if (applied) {
        vTaskDelay(pdMS_TO_TICKS(20));
        applied = bw_noise_measure(BW_CAL_WINDOWS, &width, &q_phase, &clip_pm);
        if (applied) {
            s_bw_calibrated_width_khz = width;
            printf("BW_CAL code=calibrated(%d) width_khz=%u nbw_khz=%u Q_phase=%d clip_pm=%d\n",
                   phy_rx_lab_filter_calibrated_code(), width, s_bw_last_nbw_khz, q_phase, clip_pm);
            quiet = bw_quiet("CALIBRATED", width, q_phase, clip_pm);
        }
    }
    for (int code = 0; quiet && applied && code <= 60; code += BW_CAL_STEP) {
        applied = phy_rx_lab_filter_set_code(code);
        if (!applied) { printf("BW_CAL code=%d refused=filter_write\n", code); break; }
        vTaskDelay(pdMS_TO_TICKS(20));
        if (!bw_noise_measure(BW_CAL_WINDOWS, &width, &q_phase, &clip_pm)) { applied = false; break; }
        printf("BW_CAL code=%d width_khz=%u nbw_khz=%u excess_db_x10=%d ref_mode0_khz=%u ref_mode1_khz=%u "
               "Q_phase=%d clip_pm=%d\n",
               code, width, s_bw_last_nbw_khz, predemod_nbw_excess_db_x10(s_bw_last_nbw_khz, width),
               predemod_bw_reference_khz(0, (unsigned)code),
               predemod_bw_reference_khz(1, (unsigned)code), q_phase, clip_pm);
        quiet = bw_quiet("SWEEP", width, 0, clip_pm);
        codes[count] = (uint8_t)code;
        widths[count] = width;
        noise_q[count++] = q_phase;
    }
    /* A VTX switched on mid-sweep would bias the later widths: re-check on
     * the calibrated bytes before trusting the result. */
    if (quiet && applied && phy_rx_lab_filter_set_code(PHY_RX_LAB_FILTER_CALIBRATED)) {
        vTaskDelay(pdMS_TO_TICKS(20));
        quiet = bw_noise_measure(BW_CAL_WINDOWS, &width, &q_phase, &clip_pm) &&
                bw_quiet("POSTCHECK", width, q_phase, clip_pm);
    }
    bool stored = false;
    if (quiet && applied && count) {
        s_bw_mode_fit = predemod_bw_mode_fit(codes, widths, count, &s_bw_fit_err_khz);
        int choice = predemod_bw_choose(widths, count, target);
        const char *reason = "narrowest_at_or_above_target";
        if (choice < 0) { choice = 0; reason = "target_unreachable_widest_code"; }
        /* V5 must still recognise receiver noise as NO_CARRIER (coherence
         * < 25) so a lost VTX returns to the high-gain survival state. */
        while (choice > 0 && noise_q[choice] >= BW_CAL_QUIET_QPHASE) {
            --choice;
            reason = "limited_by_v5_noise_coherence";
        }
        stored = phy_rx_lab_filter_set_code(codes[choice]) && c5vrx4_bw_store(codes[choice], widths[choice]);
        printf("BW_CAL chosen code=%u width_khz=%u noise_Q_phase=%d target_khz=%u %s "
               "esp_sdr_mode_fit=%d fit_error_khz=%u stored=%u\n",
               codes[choice], widths[choice], noise_q[choice], target, reason,
               s_bw_mode_fit, s_bw_fit_err_khz, stored);
        s_bw_cal_result = stored ? "stored" : "store_failed";
        if (stored) {
            bw_skirt_stage(codes[choice], predemod_skirt_target_khz(target, widths[choice]));
            bw_edge_stage();
        }
    } else {
        s_bw_cal_result = !applied ? "filter_or_sample_failure" : "carrier_present";
    }
    if (!stored) {
        /* Back to what was in force before the calibration. */
        bool ok = phy_rx_lab_filter_set_code(restore) &&
                  phy_rx_lab_filter_set_skirt((int)c5vrx4_bw_skirt());
        printf("BW_CAL kept_previous=%d skirt=%u restore_verified=%u\n", restore, c5vrx4_bw_skirt(), ok);
    }
    phy_rx_lab_end();
    lab_apply_vendor_gain(saved_gain);
    predemod_resume(saved_mode);
    printf("BW_CAL done result=%s code=%d\n", s_bw_cal_result, phy_rx_lab_filter_code());
    return stored;
}

void bw_status_print(void)
{
    uint8_t stored = c5vrx4_bw_code();
    printf("PREDEMOD_BW fixed=%u stored_code=%d width_khz=%u nbw_khz=%u skirt=%u applied_skirt=%d "
           "target_khz=%u applied_code=%d "
           "calibrated_code=%d calibrated_width_khz=%u esp_sdr_mode_fit=%d fit_error_khz=%u "
           "digital=%s gear=%s edge_code=%d edge_digital=%s edge_skirt=%d edge_nbw_khz=%u edge_active=%u "
           "apply_failures=%lu calibrations=%u last=%s\n",
           c5vrx4_fixed_bw_enabled(), stored == C5VRX4_BW_UNCALIBRATED ? -1 : (int)stored,
           c5vrx4_bw_width_khz(), c5vrx4_bw_nbw_khz(), c5vrx4_bw_skirt(), phy_rx_lab_filter_skirt(),
           c5vrx4_bw_target_khz(), phy_rx_lab_filter_code(),
           phy_rx_lab_filter_calibrated_code(), s_bw_calibrated_width_khz, s_bw_mode_fit,
           s_bw_fit_err_khz, s_current_bw40 ? "BW40" : "BW20",
           s_rf_bw_mode != RF_BW_MODE_AUTO ? "manual" :
           bw_fixed_calibrated() ? (c5vrx4_bw_edge_code() == C5VRX4_BW_UNCALIBRATED ? "off_no_edge" : "edge")
                                 : "digital_bw20",
           c5vrx4_bw_edge_code() == C5VRX4_BW_UNCALIBRATED ? -1 : (int)c5vrx4_bw_edge_code(),
           c5vrx4_bw_edge_digital() ? "BW20" : "BW40",
           c5vrx4_bw_edge_skirt() == C5VRX4_BW_EDGE_SKIRT_NONE ? -1 : (int)c5vrx4_bw_edge_skirt(),
           c5vrx4_bw_edge_nbw_khz(),
           rf_fixed_bw_edge_active(),
           (unsigned long)rf_fixed_bw_failures(), s_bw_cal_runs, s_bw_cal_result);
}

bool lab_run_agc_witness(bool automatic)
{
    ++s_witness_runs;
    if (!rf_native_agc_active()) {
        printf("AGC_WITNESS refused=native_agc_only (N selects native, reboot)\n");
        s_witness_result = "not_native";
        return false;
    }
    if (s_menu_active || s_rssi_probe_active || s_pre_q4_probe_active) {
        printf("AGC_WITNESS refused=other_lab_or_menu\n");
        s_witness_result = "busy";
        return false;
    }
    /* Heap for the run only: a static 4 KiB window cost every boot the RAM
     * that video_start's tasks need (ESP_ERR_NO_MEM at boot). */
    uint8_t *window = malloc(CONTROL_SAMPLE_BYTES);
    if (!window) {
        printf("AGC_WITNESS refused=no_memory\n");
        s_witness_result = "no_memory";
        return false;
    }
    agc_witness_t w;
    agc_witness_init(&w);
    s_rssi_probe_active = true;   /* observers and the level servo stand aside */
    c5vrx4_suspend();             /* no pacing gate: every native acquisition */
    vTaskDelay(pdMS_TO_TICKS(5));
    for (unsigned bit = 0; bit < 4u; ++bit) {
        const uint8_t diag[8] = {20, 21, 22, 23, 24, 25, 26, (uint8_t)(28u + bit)};
        rf_route_diag_capture(diag);
        for (unsigned n = 0; n < WITNESS_WINDOWS; ++n) {
            vTaskDelay(pdMS_TO_TICKS(2)); /* > one 32-KiB ring lap after routing */
            uint8_t *src = get_completed_rx_sample_window(CONTROL_SAMPLE_BYTES);
            sync_dma_m2c(src, CONTROL_SAMPLE_BYTES);
            memcpy(window, src, CONTROL_SAMPLE_BYTES);
            agc_witness_add(&w, window, CONTROL_SAMPLE_BYTES, bit);
        }
    }
    free(window);
    rf_restore_iq_routes();
    c5vrx4_resume();
    ++s_profile_generation;
    s_rssi_probe_active = false;
    agc_witness_result_t r;
    bool ok = agc_witness_choose(&w, &r);
    s_witness_last = r;
    printf("AGC_WITNESS mode=%s windows=%lu acquisitions=%lu acq_per_ms=%u.%u acq_us=%u.%u "
           "acq_share_pm=%u walk_min_gain=%u trapped_gain=%u..%u\n",
           automatic ? "auto" : "manual", (unsigned long)w.windows, (unsigned long)w.acquisitions,
           r.acq_per_ms_x10 / 10u, r.acq_per_ms_x10 % 10u, r.acq_us_x10 / 10u, r.acq_us_x10 % 10u,
           r.acq_share_pm, r.gain_min_acq, r.trapped_min, r.trapped_max);
    for (unsigned bit = 0; bit < 4u; ++bit)
        printf("AGC_WITNESS state_bit=DIAG[%u] p1_acq_pm=%lu p1_trapped_pm=%lu\n", 28u + bit,
               w.acq[bit] ? (unsigned long)((uint64_t)w.ones_acq[bit] * 1000u / w.acq[bit]) : 0ul,
               w.trapped[bit] ? (unsigned long)((uint64_t)w.ones_trapped[bit] * 1000u / w.trapped[bit]) : 0ul);
    if (!ok) {
        s_witness_result = w.acquisitions < 8u ? "no_acquisitions_carrier_needed" :
                           r.bit < 0 || r.separation_pm < 700u ? "no_separating_bit" :
                           "flag_not_clean_enough";
        printf("AGC_WITNESS result=%s stored=0\n", s_witness_result);
        return false;
    }
    uint8_t flag = (uint8_t)((unsigned)r.bit | (r.invert ? 0x80u : 0u));
    bool stored = c5vrx4_agc_flag_store(flag);
    s_witness_result = stored ? "stored" : "store_failed";
    printf("AGC_WITNESS result=%s flag=DIAG[%d]%s separation_pm=%u active_acq_pm=%u "
           "active_trapped_pm=%u lead_samples=%u lag_samples=%u lookahead_ok=%u "
           "applies=after_reboot\n", s_witness_result, 28 + r.bit, r.invert ? "_inverted" : "",
           r.separation_pm, r.active_acq_pm, r.active_trapped_pm, r.lead_samples,
           r.lag_samples, r.lead_samples >= 2u);
    return stored;
}

/* First native carrier without a stored witness: calibrate once, reboot to
 * apply (program and lane route are chosen per boot). At most three tries
 * per boot, one a minute; never in Direct Gain mode. */
static void agc_witness_autocheck(void)
{
    static unsigned carrier_ticks, tries;
    static int64_t last_try_us;
    if (!rf_native_agc_active() || !c5vrx4_agc_mask_enabled() ||
        c5vrx4_agc_flag() != C5VRX4_AGC_FLAG_UNKNOWN || tries >= 3u ||
        s_menu_active || s_rssi_probe_active) { carrier_ticks = 0; return; }
    uint8_t sample[RX_PROBE_REGIONS * RX_PROBE_REGION_BYTES];
    if (!rx_probe_copy_completed(sample)) return;
    control_metrics_t m = analyze_control_window(sample, sizeof(sample), 0);
    if (m.q_phase < 40) { carrier_ticks = 0; return; }
    if (++carrier_ticks < 12u) return; /* ~3 s of carrier at the 250 ms tick */
    int64_t now = esp_timer_get_time();
    if (last_try_us && now - last_try_us < 60000000) return;
    last_try_us = now;
    carrier_ticks = 0;
    ++tries;
    if (lab_run_agc_witness(true)) {
        printf("AGC_WITNESS rebooting to apply the acquisition mask ('|' opts out)\n");
        fflush(stdout);
        vTaskDelay(pdMS_TO_TICKS(150));
        esp_restart();
    }
}

static bool predemod_quiet_owner(void)
{
    return !s_menu_active && !s_rssi_probe_active &&
           !s_pre_q4_probe_active && !phy_rx_lab_busy() && !rf_native_agc_active() &&
           s_rx_profile == RX_PROFILE_DIRECT_GAIN && s_agc_mode == ANALOG_AGC_ACTIVE;
}

/* Persist V5's measured gain map: at most every 2 minutes, only when the
 * map changed and V5 is holding (flash wear and no write mid-transition). */
static void predemod_dg3_map_service(void)
{
    static int64_t last_us;
    static uint32_t last_learned;
    const int64_t now = esp_timer_get_time();
    if (now - last_us < 120000000LL || rf_native_agc_active() ||
        s_direct_gain_v3.state != DG3_HOLD || s_direct_gain_v3.learned == last_learned) return;
    last_us = now;
    last_learned = s_direct_gain_v3.learned;
    dg3_map_blob_t blob;
    /* The observer task owns the controller; a torn copy only costs one
     * slightly stale entry, which the next save replaces. */
    if (!direct_gain_v3_export_map(&s_direct_gain_v3, &blob)) return;
    if (s_dg3_saved_valid && !memcmp(&blob, &s_dg3_saved, sizeof(blob))) return;
    /* Context identity (review 2026-10-06): a map is a measurement of this
     * channel and IQ lane; another context must not import it. */
    blob.freq_mhz = rf_get_frequency_mhz();
    blob.lane_mode = c5vrx4_fixed_lane();
    if (c5vrx4_blob_store("dg3_map", &blob, sizeof(blob))) {
        s_dg3_saved = blob;
        s_dg3_saved_valid = true;
        ++s_dg3_map_saves;
        printf("DG3_MAP saved=%lu imports=%lu model_mismatches=%lu\n",
               (unsigned long)s_dg3_map_saves, (unsigned long)s_dg3_map_imports,
               (unsigned long)s_direct_gain_v3.model_mismatches);
    }
}

static void predemod_dc_service(void)
{
    /* Disabled (board 2026-10-06): live LUT read-back returned random words
     * (want 0x750b, got 0x215f/0x334d/0x003f) while the TX engine runs, so a
     * live decoder rewrite can land on the wrong index. The hardware DC
     * correction above replaces it at the range edge. */
    if (true) return;
    if (!c5vrx4_dc_recenter_enabled() || c5vrx4_history_enabled() ||
        !predemod_quiet_owner() || !c5v4_level_hw_lut_verified()) return;
    portENTER_CRITICAL(&s_dc_mux);
    uint32_t windows = s_dc_windows, epoch = s_dc_epoch;
    int64_t si = s_dc_sum_i, sq = s_dc_sum_q;
    if (windows >= DC_MIN_WINDOWS) { s_dc_sum_i = s_dc_sum_q = 0; s_dc_windows = 0; }
    portEXIT_CRITICAL(&s_dc_mux);
    if (windows < DC_MIN_WINDOWS) return;
    if (epoch != s_dc_filter_epoch) {
        memset(&s_dc_filter, 0, sizeof(s_dc_filter));
        s_dc_filter_epoch = epoch;
    }
    int measured[2] = {(int)(si / (int64_t)windows), (int)(sq / (int64_t)windows)};
    s_dc_measured[0] = measured[0]; s_dc_measured[1] = measured[1];
    ++s_dc_evaluations;
    int applied[2], next[2];
    c5v4_decoder_dc(applied);
    if (!predemod_dc_decide(&s_dc_filter, measured, applied, DC_AGREE_MCELLS,
                            DC_STEP_MCELLS, DC_LIMIT_MCELLS, next)) return;
    int64_t now = esp_timer_get_time();
    if (s_dc_last_write_us && now - s_dc_last_write_us < DC_MIN_GAP_US) return;
    s_dc_last_write_us = now;
    if (!c5v4_decoder_recenter(next[0], next[1])) {
        if (++s_dc_refusals == 1) printf("C5V4_DC_RECENTER refused=lut_write_or_mode\n");
        return;
    }
    printf("C5V4_DC_RECENTER applied_mcells=%d/%d lane=%u gain=%u measured=%d/%d\n",
           next[0], next[1], rf_get_iq_lanes(), s_current_gain, measured[0], measured[1]);
}

/* First boot: the fixed-BW calibration needs 3 s of live no-carrier
 * listening, so the idle raster waits until it has been tried once. */
bool bw_autocal_waiting(void)
{
    return !s_bw_autocal_tried && c5vrx4_fixed_bw_enabled() &&
           /* Also once for a code stored before the second stage existed
            * (no measured noise bandwidth yet). */
           (c5vrx4_bw_code() == C5VRX4_BW_UNCALIBRATED || !c5vrx4_bw_nbw_khz() ||
            !c5vrx4_bw_edge_nbw_khz()) &&
           phy_rx_lab_filter_calibrated_code() >= 0;
}

static void predemod_bw_autocal(void)
{
    static unsigned quiet_ticks;
    static int64_t last_try_us;
    const arc_gain_table_t *table = rf_get_arc_gain_table();
    bool want = c5vrx4_fixed_bw_enabled() &&
                (c5vrx4_bw_code() == C5VRX4_BW_UNCALIBRATED || !c5vrx4_bw_nbw_khz() ||
                 !c5vrx4_bw_edge_nbw_khz()) &&
                phy_rx_lab_filter_calibrated_code() >= 0 &&
                table && predemod_quiet_owner() &&
                (s_current_bw40 || s_rf_bw_mode == RF_BW_MODE_AUTO) &&
                s_direct_gain_v3.current_gain == table->max_index &&
                s_direct_gain_v3.state != DG3_SETTLE && s_v3_coherence < 25;
    if (!want) { quiet_ticks = 0; return; }
    if (++quiet_ticks < BW_AUTO_QUIET_TICKS) return;
    int64_t now = esp_timer_get_time();
    if (last_try_us && now - last_try_us < BW_AUTO_RETRY_US) return;
    last_try_us = now;
    quiet_ticks = 0;
    s_bw_autocal_tried = true;
    (void)lab_run_bw_calibration(true);
}

/* Own task, not the console: the BW calibration runs for over a minute and
 * the console must keep draining USB meanwhile (else host writes and
 * esptool time out). 4 KiB: -fcallgraph-info worst case is 2240 B. */
void predemod_task(void *arg)
{
    (void)arg;
    rf_set_post_gain_hook(dco_post_gain);
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(250));
        if (s_menu_bw_cal_request && !s_menu_active) {
            s_menu_bw_cal_request = false;
            (void)lab_run_bw_calibration(false);
        }
        if (s_menu_witness_request && !s_menu_active) {
            s_menu_witness_request = false;
            if (lab_run_agc_witness(false) && c5vrx4_agc_mask_enabled()) {
                printf("AGC_WITNESS rebooting to apply the acquisition mask\n");
                fflush(stdout);
                vTaskDelay(pdMS_TO_TICKS(150));
                esp_restart();
            }
        }
        predemod_dc_service();
        predemod_dco_service();
        predemod_dg3_map_service();
        predemod_gain_readback_service();
        predemod_dc_drift_service();
        flight_log_service();
        static unsigned hb_ticks;
        if (++hb_ticks >= 20u) {
            hb_ticks = 0;
            printf("HB predemod t_s=%lld dco_held=%u dco_valid=%u searches=%lu\n",
                   esp_timer_get_time() / 1000000, phy_rx_lab_dco_held(),
                   phy_rx_lab_dco_valid(), (unsigned long)s_dco_searches);
            dco_hook_print("HB dco_hook");
        }
        predemod_sphase_autocheck();
        predemod_bw_autocal();
        agc_witness_autocheck();
        agc_mask_observe();
    }
}

void predemod_correction_print(void)
{
    /* The digital recentring is disabled in code (live LUT refused): report
     * the request, not a correction (external audit, PR #174). */
    printf("PREDEMOD_AUTO dc_recenter_requested=%u dc_recenter_state=blocked_live_lut "
           "hw_dco=per_gain hw_dco_searches=%lu hw_dco_holds=%lu hw_dco_loads=%lu hw_dco_saves=%lu "
           "hw_dco_carrier_refusals=%lu hw_dco_env_ratio_x100=%u hw_dco_broken_iq=%lu hw_dco_hold_aborts=%lu "
           "quiet_s=%lld rx_recal_freq=%u rx_recal_runs=%lu "
           "temp_c=%.1f drift_avg_mcells=%d/%d drift_nudges=%lu "
           "measured_mcells=%d/%d evaluations=%lu refusals=%lu "
           "sphase_auto=%u sphase_checked=%u sphase_ppm=%u sphase_state=%s sphase_scans=%u\n",
           c5vrx4_dc_recenter_enabled(), (unsigned long)s_dco_searches, (unsigned long)s_dco_holds,
           (unsigned long)s_dco_loads, (unsigned long)s_dco_saves, (unsigned long)s_dco_carrier_refusals,
           s_dco_env_ratio, (unsigned long)s_dco_broken_iq, (unsigned long)s_dco_hold_aborts,
           s_quiet_since_us ? (esp_timer_get_time() - s_quiet_since_us) / 1000000 : 0LL,
           s_rx_recal_freq, (unsigned long)s_rx_recal_runs, (double)s_temp_c, s_drift_avg[0], s_drift_avg[1],
           (unsigned long)s_drift_nudges, s_dc_measured[0], s_dc_measured[1],
           (unsigned long)s_dc_evaluations, (unsigned long)s_dc_refusals,
           c5vrx4_sphase_auto_enabled(), s_sphase_auto_done,
           s_sphase_auto_done ? s_sphase_auto_ppm : 0u, sphase_state_name(), s_sphase_scans);
    c5v4_decoder_print();
}
