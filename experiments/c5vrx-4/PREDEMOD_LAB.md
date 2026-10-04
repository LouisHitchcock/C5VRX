# Pre-demodulation lab (#165)

Purpose: measure what happens to the I/Q *before* Q4/I4 and Phase8, so lane,
gain and filter policy can be chosen on evidence. The labs change nothing
persistent; the default-on corrections below are listed separately. Every PHY
write is reversible, verified and refused on an unpinned PHY archive. Credits: zerowidth/C5VRX PR #3 (sampling phase, DC
centre, 11p, R8), Logicenios/C5VRX (link edge-placement metric), ESPARGOS
esp-sdr (C5 RX filter-code control and noise-width curves, S31 PBUS DC loop)
and h0m3us3r/eSpDR (S3 DC DACs).

## Recovered facts (pinned `libphy.a`, IDF 6.0.2, esp-phy-lib 59c1234)

| Item | Finding | Consequence |
| --- | --- | --- |
| RX DC DACs | `phy_pbus_set_dco()` writes PBUS (block,bank) (2,1),(3,1),(2,2),(3,2); `phy_pbus_force_test()` writes `0x600A0884`; debug/work mode via `phy_pbus_force_mode()` | DC can be corrected before the ADC |
| RX DC calibration | `phy_set_rx_gain_cal_dc()` calibrates 5210, 5290, 5530, 5610, 5690, 5775, 5855 MHz when `phy_param[0x2a] != 0`, otherwise only 2432 MHz | No point above 5855 MHz; FPV upper band uses the nearest one |
| Temperature tracking | `phy_cal_param_track()` would redo RX DC/IQ cal, gain table and channel live | Correctly disabled (`CONFIG_ESP_PHY_DISABLE_PLL_TRACK`); boot DC is never refreshed, so it can drift as the board warms |
| RX filter caps | `phy_filter_dcap_set()` writes 0x67 regs 6..20 from `phy_param[0xF5..0xFC]` at RF init; `phy_11p_set(1,0)` writes 60 to regs 6..13 | 11p is mainly a narrower analog filter; sweep relative to the per-chip calibrated value |
| BW gear | `phy_wifi_fbw_sel()` writes only digital `0x600A0874` | Runtime BW switching may not move the analog filter; replaced by the fixed analog BW below |
| Dump banks | `HP_SYSTEM_SRAM_USAGE[11:8]` hands whole 128-KiB blocks to the MAC dump | Bank rotation would cost ~256 KiB: not pursued |

## Default-on corrections

- **Digital DC recentring** (`%` opt-out, NVS `dc_recenter`). The V5 observer
  accumulates the raw cell-centre mean of settled, unclipped windows per
  gain/lane/profile epoch. Every 250 ms a low-priority task evaluates >=600
  windows; two evaluations agreeing within 0.15 cell and a move of >=0.12 cell
  (clamped to 3 cells, deadband back to the pristine table, at most one rewrite
  per 2 s) rewrite both odd decoder banks with `predemod_decoder_word()`. At
  zero offset that word equals the generated table bit-exactly. It corrects
  decode geometry only: samples that already folded or clipped stay lost, and
  quadrant/trajectory sign bits are unchanged. HISTORY decode and native AGC
  refuse it; a program reload is detected and the applied state reset.
- **First-lock sampling-phase check** (`&` opt-out, NVS `sphase_auto`). Once
  per boot, at the first V5 HOLD with coherence >=80 %, 48 windows are
  measured; only >=5000 ppm mid-transition reads trigger the `@` scan.

- **Fixed analog bandwidth** (`^` opt-out, NVS `fixed_bw`; `=` recalibrates).
  Prior work this extends, with its scope kept:
  - ESPARGOS esp-sdr (GPL-3.0, commit `ac627b0b`,
    `main/common/rx_bandwidth.h`, `main/families/c5_c6_c61/receiver.c`): the C5
    RX0 capacitor DAC is BBTOP 0x67 regs 6/7, set as an absolute 6-bit code
    with the upper bits kept; approximate full noise widths from median noise
    FFTs (2300/5500 MHz, 80 MS/s IQ10): mode 0 code 0..60 = 23..11 MHz, mode 1
    code 0..60 = 48..22 MHz (24 MHz near code 52); changing mode needs a full
    channel setup. Their board, not ours.
  - zerowidth/C5VRX PR #3: `phy_11p_set(1,0)` (code 60 in regs 6..13 plus other
    fields) on R8 lowered sync-tip noise from ~102-105 to ~87-91 kHz with less
    picture noise; 11p mode 1 was much worse.
  - C5VRX: `WIFI_BW20` lost resolution and destabilised chroma
    (`docs/fix-cvbs-jitter-and-static.md`, `docs/static-reduction-and-filtering.md`);
    the PR #25 gear left production; any new narrowing needs a controlled A/B
    (`docs/pr-derived-findings.md`).

  What C5VRX-4 adds: C5VRX tunes with BW40 configured and secondary channel
  NONE, so which esp-sdr mode applies is unproven. With Direct Gain paused,
  table-maximum gain and digital BW40, the noise width of this chip is measured
  for the calibrated bytes (carrier pre-check), absolute codes 0, 4 .. 60, and
  the calibrated bytes again (post-check): a 64-point Hann PSD (625 kHz bins),
  96 observer windows x 4 regions, -3 dB full width against the median of the
  1.25..5 MHz bins (DC excluded). Our 40-MS/s view reads anything wider than
  40 MHz as 40 MHz. The widths are fitted to both esp-sdr curves (`!` reports
  the mode and mean error) and the narrowest code still >=`bw_target` (24 MHz,
  NVS MHz) is kept, or code 0 (widest) when none reaches it. Narrowed noise is
  more coherent sample to sample, so the choice steps wider until the noise
  Q_phase stays <34 (V5 coherence 25) and V5 keeps reading noise as
  NO_CARRIER. Average Q_phase >=34 in the pre/post-check, or clipping >=5 % at
  any stage, aborts without storing. Only regs 6/7 change; regs 8..13 and the
  upper bits keep the PHY calibration. Automatic once while uncalibrated,
  after 3 s of no-carrier table-maximum listening (retry at most once a
  minute); the stored code is re-applied at boot and in every tune/bandwidth
  transaction. Range and picture benefit are hardware-pending.

## Commands

- `!` prints `PREDEMOD` (lane policy, observer glitch ppm, receiver DC) and,
  on the pinned PHY, `PREDEMOD_PHY/DCO/FILTER` (DC-cal mode and point, the four
  DCO words, regs 6..13 and their calibration bytes).
- `@` pauses Direct Gain, measures the mid-transition glitch rate (a sample
  that jumps >=6 cells from both neighbours while they agree), slips the
  PARLIO RX divider for 1 us between nine positions, then keeps slipping until
  the rate is within 25 % (min. 300 ppm) of the cleanest position seen.
- `#` measures the I/Q centre over 64 windows, probes the bank-2 DC DACs by
  +-16 codes, solves the measured 2x2 response, corrects for up to four bounded
  steps (32 codes per step, 96 from the start), holds it for a one-second look,
  then restores every PBUS word and work mode.
- `$` steps regs 6..13 by +4/+8/+16/+24 codes and to 60 (11p-equivalent)
  relative to the calibrated baseline, one second per stage, then restores the
  bytes in force before the sweep (the fixed-BW code, if any).
- `=` runs the fixed-BW calibration above and prints `BW_CAL` per code with
  both esp-sdr reference widths; `!` adds `PREDEMOD_FILTER_BASE` and
  `PREDEMOD_BW` (stored/applied/calibrated code, measured widths, mode fit,
  target, gear state, failures, last result).

## Measurement procedure

1. Fixed gain, fixed lane, one channel; VTX off, then a stable VTX.
2. Reboot 20-30 times and run `!` and `@` each time: two or more repeatable
   glitch populations, or a large drop after slipping, make sampling phase P0.
3. `#` on several channels (including 5865/5917) and gains, VTX off and on,
   cold and warm: record before/after DC in ADC codes and picture noise.
4. `$` VTX off for noise P50 per stage, then VTX on for Q_phase, sync and
   colour by eye. Compare with `:` (full 11p).
5. Only after these: decide automatic slip, acquisition-time DCO correction,
   a default filter code and whether a finer fixed lane now wins.

Hardware acceptance is pending for every item; host tests cover only the
restore logic, bounds and the solver.
