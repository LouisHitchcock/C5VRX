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
/* Fixed analog bandwidth (default on, '^' opts out with a reboot): the RX RC
 * filter capacitors (BBTOP 0x67 regs 6..13) get one calibrated offset from the
 * per-chip baseline, chosen from the measured receiver-noise width so the
 * full -3 dB width is as close as possible to (not below) bw_target (24 MHz).
 * The digital path stays BW40 and the BW20/BW40 gear is retired. The offset is
 * measured once without a carrier ('=' repeats it) and kept in NVS. */
#define C5VRX4_BW_UNCALIBRATED UINT8_MAX
bool c5vrx4_fixed_bw_enabled(void);
uint8_t c5vrx4_bw_offset(void);
unsigned c5vrx4_bw_width_khz(void);
unsigned c5vrx4_bw_target_khz(void);
bool c5vrx4_bw_store(uint8_t offset, unsigned width_khz);
/* u: experimental automatic CVBS level servo, opt-in/reboot. */
bool c5vrx4_level_enabled(void);
