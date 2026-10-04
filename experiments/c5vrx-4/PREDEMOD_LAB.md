# Pre-demodulation lab (#165)

Purpose: measure what happens to the I/Q *before* Q4/I4 and Phase8, so lane,
gain and filter policy can be chosen on evidence. Nothing here changes the
production policy. Every PHY write is reversible, verified and refused on an
unpinned PHY archive. Credits: zerowidth/C5VRX PR #3 (sampling phase, DC
centre, 11p, R8), Logicenios/C5VRX (link edge-placement metric), ESPARGOS
esp-sdr (C5 filter-cap curve, S31 PBUS DC loop) and h0m3us3r/eSpDR (S3 DC DACs).

## Recovered facts (pinned `libphy.a`, IDF 6.0.2, esp-phy-lib 59c1234)

| Item | Finding | Consequence |
| --- | --- | --- |
| RX DC DACs | `phy_pbus_set_dco()` writes PBUS (block,bank) (2,1),(3,1),(2,2),(3,2); `phy_pbus_force_test()` writes `0x600A0884`; debug/work mode via `phy_pbus_force_mode()` | DC can be corrected before the ADC |
| RX DC calibration | `phy_set_rx_gain_cal_dc()` calibrates 5210, 5290, 5530, 5610, 5690, 5775, 5855 MHz when `phy_param[0x2a] != 0`, otherwise only 2432 MHz | No point above 5855 MHz; FPV upper band uses the nearest one |
| Temperature tracking | `phy_cal_param_track()` would redo RX DC/IQ cal, gain table and channel live | Correctly disabled (`CONFIG_ESP_PHY_DISABLE_PLL_TRACK`); boot DC is never refreshed, so it can drift as the board warms |
| RX filter caps | `phy_filter_dcap_set()` writes 0x67 regs 6..20 from `phy_param[0xF5..0xFC]` at RF init; `phy_11p_set(1,0)` writes 60 to regs 6..13 | 11p is mainly a narrower analog filter; sweep relative to the per-chip calibrated value |
| BW gear | `phy_wifi_fbw_sel()` writes only digital `0x600A0874` | Runtime BW switching may not move the analog filter; verify with `$`/`B` |
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
- `$` steps regs 6..13 by +4/+8/+16/+24 codes and to 60 (11p-equivalent), one
  second per stage, then restores the calibrated bytes.

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
