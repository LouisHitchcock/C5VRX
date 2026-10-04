#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
/* Optional native tracking gate; entirely inert under Direct Gain V5.
 * Startup gain ownership comes from the separate c5vrx4 NVS namespace. */
void c5vrx4_start(void);
void c5vrx4_suspend(void);
void c5vrx4_resume(void);
bool c5vrx4_console(int key);
bool c5vrx4_history_enabled(void);
/* IQ lane policy, NVS c5vrx4/lane_mode, Z cycles it with a reboot. Fixed
 * fine {9,7,6,5} is the default: the lanes never switch at runtime. Fixed
 * ultrafine and protected adaptive V5 lanes remain comparisons. */
enum { C5VRX4_LANES_FINE = 0, C5VRX4_LANES_ULTRAFINE = 1, C5VRX4_LANES_ADAPTIVE = 2 };
#define C5VRX4_LANE_ADAPTIVE UINT8_MAX
uint8_t c5vrx4_lane_mode(void);
const char *c5vrx4_lane_mode_name(void);
/* RF lane set held for the whole session, or C5VRX4_LANE_ADAPTIVE. */
uint8_t c5vrx4_fixed_lane(void);
/* M: output transfer, NVS c5vrx4/cvbs_legacy (1 keeps LEGACY_FULL). */
enum { C5VRX4_CVBS_STD150 = 0, C5VRX4_CVBS_LEGACY = 1, C5VRX4_CVBS_150 = 2 };
unsigned c5vrx4_cvbs_mode(void);
const char *c5vrx4_cvbs_mode_name(void);
bool c5vrx4_cvbs_legacy_enabled(void);

bool c5vrx4_lane_window_ready(uint64_t now_us);
uint8_t c5vrx4_lane_target(uint8_t current, uint8_t requested, const uint8_t *sample, size_t bytes, uint64_t now_us);
void c5vrx4_lane_print(void);

/* Default-on pre-demodulation correction (#165), NVS opt-outs, reboot:
 * '%' digital DC recentring of the static Phase8 decoder (dc_recenter),
 * '&' one automatic sampling-phase check at the first carrier lock (sphase_auto). */
bool c5vrx4_dc_recenter_enabled(void);
bool c5vrx4_sphase_auto_enabled(void);
/* Fixed analog bandwidth (default on, '^' opts out with a reboot), after
 * ESPARGOS esp-sdr's C5 BANDWIDTH control: one absolute RX0 capacitor code in
 * BBTOP 0x67 regs 6/7, chosen from the receiver-noise width measured on this
 * chip so the full -3 dB width is the narrowest still >= bw_target (24 MHz),
 * or the widest available when none reaches it. The digital path stays BW40
 * and the BW20/BW40 gear is retired. Measured once without a carrier ('='
 * repeats it); NVS bw_code (255 = not yet measured) and bw_width. */
#define C5VRX4_BW_UNCALIBRATED UINT8_MAX
bool c5vrx4_fixed_bw_enabled(void);
uint8_t c5vrx4_bw_code(void);
unsigned c5vrx4_bw_width_khz(void);
unsigned c5vrx4_bw_target_khz(void);
bool c5vrx4_bw_store(uint8_t code, unsigned width_khz);
/* Native AGC acquisition mask (native mode only, '|' opts out with a
 * reboot). PARLIO data bit 0 (fine Q LSB) carries a MODEM_DIAG AGC state bit
 * found by the witness calibration ('*'); the static program holds the DAC
 * through every acquisition. agc_flag: bits 0..1 = state bit (DIAG[28+n]),
 * bit 7 = inverted, 255 = not calibrated. */
#define C5VRX4_AGC_FLAG_UNKNOWN UINT8_MAX
uint8_t c5vrx4_agc_flag(void);
bool c5vrx4_agc_flag_store(uint8_t flag);
bool c5vrx4_agc_mask_enabled(void);
/* Native + enabled + calibrated + STATIC decode: masked program and lane. */
bool c5vrx4_agc_mask_active(void);
/* No-carrier idle raster (default on, '_' opts out with a reboot): after 2 s
 * without any carrier or sync, the standalone raster emits clean black video
 * in the last live standard so strict goggles (HDZero/TP2825) stay locked;
 * live video returns at the first carrier or sync. NVS idle_raster. */
bool c5vrx4_idle_raster_enabled(void);
/* Last stable live standard (0 NTSC, 1 PAL, 255 unknown), NVS last_std:
 * the idle raster and an AUTO menu start in it after a reboot. */
#define C5VRX4_STD_UNKNOWN UINT8_MAX
uint8_t c5vrx4_last_standard(void);
void c5vrx4_last_standard_store(uint8_t standard);
/* u: experimental automatic CVBS level servo, opt-in/reboot. */
bool c5vrx4_level_enabled(void);
