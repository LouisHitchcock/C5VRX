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


## Range labs (2026-10-04)

Idea source: [FPVGateC5RX](https://github.com/RaceFPV/FPVGateC5RX) provenance
and extended-tuning notes (facts only, no code). Every behaviour below is our
own reading of the pinned libphy.

| Key | Lab | What it does |
|---|---|---|
| `'` | sigRSSI A/B | Saves the eleven AGC words that `phy_check_sigrssi_en(1)` rewrites (0x600A7008/0C/10/18/30/48/90/B0/C4/EC/150; 0x7030 is the BB-AGC gate). It enables sigRSSI mode, samples `phy_get_sigrssi()` (= `(int8_t)(0x600A706C >> 8)`) at 1 ms for 1 s, then restores and verifies every word. Prints `SIGRSSI` min/p10/p50/p90/max/mean dBm, plus `RANGELAB` Q rows before, during and after. Refused while the BB-AGC gate is held. |
| `"` | PHY tracking A/B | Calls `phy_param_track_tot(1,0)`: TX-power tracking, `phy_i2c_correct`, and `phy_cal_param_track`. The last one recalibrates RX DC, IQ and the gain table when the temperature changed, then calls `phy_chip_set_chan` on the stored frequency. `phy_set_freq` stores that frequency too, so the tune is kept. This is not reversible: it is the maintenance call the Wi-Fi driver would normally make periodically. |

Questions to answer on hardware:
1. Does sigRSSI follow the carrier at the range edge, where `Q_phase` no
   longer separates carrier from noise? If so, it is a better NO_CARRIER,
   idle-raster and diversity metric.
2. In **native** mode, does the sigRSSI configuration stop the ~25–50 us
   packet re-acquisitions? Watch the `RANGELAB` rows and, while masking,
   `agc_flag_share_pm`. If it does, native AGC without line noise becomes
   possible; an earlier walk test reached further with native AGC.
3. After the board has warmed up, does tracking change noise, Q or DC?

Modelled and rejected as range levers, with fine lanes, 4-bit and the 75 ns
span: a modulo-2π decoder instead of Unwrap75 (identical click rate), a
click clamp to black (no fewer false syncs), and digital IQ-imbalance
correction (helps only at strong signal: 9.3° → 4.4° at 3 dB / 8° imbalance,
nothing at the edge).

## libphy RX-path audit for lost dB (2026-10-04)

Our own disassembly of the pinned `libphy.a` (hash above) and `librftest.a`.
The question was where the vendor RX path costs sensitivity through
filtering, notches or calibration. Evidence labels: **static** means read
from the archive; nothing here is a hardware measurement yet.

| Suspect | Static finding | Expected effect |
|---|---|---|
| Spur notch | `phy_spur_coef_cfg` (slot 0 at the nearest 40 MHz harmonic, ±20 MHz in BW40, bit 13 of `0x600A7C14+4·slot`) has **no caller** in `libphy`, `libpp`, `libnet80211` or the ROM linker scripts. Only `librftest` `set_spur_reg` calls `phy_spur_cal`/`phy_spur_reg_write`, and C5VRX does not link the RF test library. | No loss expected. `/` prints slots 0–3 so hardware can confirm bit 13 is clear. |
| Unfiltered 2:1 decimation | PARLIO takes every second sample of the ~80 MS/s MODEM_DIAG bus (`docs/continuous-iq-findings.md`), so all noise out to ±40 MHz folds into the ±20 MHz view. | Host model: pre-detection noise vs. a 24 MHz brick wall. With a 35–48 MHz analog width: +1.8 to +3.2 dB (1st–3rd order). With a 24 MHz analog width: +0.2 to +1.1 dB. Fixed analog BW (`=`) already recovers most of it. |
| Digital filter ahead of the tap? | `phy_rx_filter_mode(m)` = `0x600A0430[21:18]`. On 5 GHz, `phy_rfpll_set_adc_rate` writes mode 0 (mode 4 if the stored width is BW20), then mode 8 with ADC rate 1 above 5830 MHz. The earlier BW20 rejection (`docs/static-reduction-and-filtering.md`: narrower, detail and chroma lost) hints that some digital width control precedes the tap. | Unknown, measured by `/`. A digital filter ahead of the tap would be a steep, free pre-detection filter. Its value is the remaining 0.2–1 dB plus adjacent-pilot rejection, and only if a mode near 24 MHz exists. |
| ADC rate at 5830 MHz | `phy_adc_rate_set(r)` writes I2C block 0x66 host 0 reg 4 bit 2 = !r and `0x600A0448[1:0]` = r,r. On 5 GHz the selector is 0 up to 5830 MHz and 1 above (R6/R7/R8, F7/F8, A1/A2, B8, E-high). The `analog-lock-phy-lab.md` row "ADC selector remains 1" reads only the >5830 state. | Unknown. If the rate changes the bus clock, PARLIO's free-running 40 MHz could sample asynchronously on one side of 5830 MHz (watch `glitch_ppm`). `/` measures the other rate on the current channel. |
| LO buffer cap | `phy_get_dcap_degen(f)` = 10 + (5880 − f)/36, clamped 10..31. It is written to I2C block 0x63 (PLL/LO block) reg 21 [4:0] by `phy_set_freq_i2c_new`. Above 5880 MHz it pins at 10. | The vendor's own truncating formula gives 9 at 5917/5945 MHz: one code short. Expected < 0.1 dB. Not worth a lab. |
| ADC dither | `phy_get_adc_rand` is an empty return. `phy_set_adc_rand` is a DCO calibration routine, not dither. | Nothing to remove. |
| RX IQ / DC cal points | `phy_set_rx_gain_cal_iq` loops 7 points on 5 GHz. RX DC tops out at 5855 MHz (known). | Small. IQ correction helps only at strong signal; DC recentring covers the rest. |
| Packet-detector tweaks | `phy_rx_11b_opt`, `phy_rx_pkdet_dc_cal`, `phy_rx_sense_set`, the CCA/NF-auto bits: packet-detection and AGC-trigger registers. | Not in the MODEM_DIAG sample path. They matter only for native AGC acquisition (covered by the mask). |
| Temperature tracking | Disabled by config (`"` exercises it). | Measured by `"`. |

### `/` digital RX filter / ADC-rate lab

Direct Gain only, at fixed gain. Run it twice on the same channel: VTX off
(noise width), then a steady weak VTX (clicks).

1. Prints `DFILT begin` with the vendor mode, ADC selector and word, the ADC
   I2C byte and spur slots 0–3.
2. `BASELINE`.
3. `MODE` 0..15, with only `0x600A0430[21:18]` changed and the ADC rate
   unchanged.
4. `MODE_RESTORED`.
5. `ADC_ALT`: the other rate through `phy_adc_rate_set`, current mode.
6. `ADC_ALT_VENDOR_MODE`: the mode the vendor pairs with that rate (0 or 8).
7. Exact restore of both words and the I2C byte, verified, then `RESTORED`.
   A failed verification reboots.

Each row gives the 64-point PSD `width_khz`, mid-transition `glitch_ppm`,
P50, Q_phase, winding (`wind_pm`, `strong_wind_pm`) and clipping over 48
observer windows.

Reading the result:

- **All widths equal:** the tap sits before the digital filter. The analog
  RC filter and fixed BW are then the only pre-detection filter, and digital
  modes are a dead end.
- **Some modes narrower:** a digital filter is ahead of the tap. Pick the
  narrowest mode still ≥ ~24 MHz and compare `wind_pm` against baseline with
  a weak VTX. Fewer clicks at equal Q is real range. Then A/B the picture
  (chroma, detail) before any default change.
- **`ADC_ALT` `glitch_ppm` much higher or lower than baseline:** the rate
  changes the bus clock relative to PARLIO. Repeat on 5825 vs. 5845 MHz.
