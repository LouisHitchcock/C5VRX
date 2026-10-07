# Span50 detector (replaces Unwrap75) - draft

Status: **field-confirmed improvement, implementation in progress.**

## Result

Board test 2026-10-07 on #181 with all v4 programs replaced by C5VRX-3's
Phase8 span50 program (`main/fm_phase8_hr_live.bsasm`, 2 bundles per pair,
20 MS/s unique DAC, no unwrap): operator report "really clean and the range
is a LOT better" than span75/Unwrap75.

## Why

Span75 resamples at 13.33 MS/s without anti-aliasing: FM discriminator noise
between 6.67 MHz and the tap's analog filter edge (~10 MHz, measured nbw
20.7 MHz) folds into the picture. Span50's Nyquist (10 MHz) sits at that
filter edge, so almost nothing folds. The unwrap only existed because a
75 ns span holds just +-6.67 MHz in +-180 degrees; 50 ns holds +-10 MHz.

`tools/detector_study/alias_spans.py` (repo aliasing model, fixed C/N0
82 dB-Hz, channel +-10 MHz), band SNR dB 1-2 / 2-3 / 3-4 MHz:

| detector | 1-2 | 2-3 | 3-4 |
|---|---|---|---|
| span75 | 13.4 | 9.0 | 5.7 |
| **span50** | **14.4** | **10.2** | **7.5** |
| span25 | 6.4 | 5.4 | 4.2 |
| adj40 (40 MS/s, not feasible) | 15.1 | 10.7 | 8.0 |

## Design study: beyond adj40 within the BitScrambler budget

`tools/detector_study/designs.py`: bounded CVBS with sync and carrier
offset, scored against the TRUE video (delay, gain and offset fitted), so
noise, distortion and lost detail all count; clicks = |error| > 40 IRE.
Only designs that fit 2 bundles per pair, one lookup per bundle, a 1024x16
LUT with 2 spare address bits per lookup.

SINAD dB / clicks per 1000:

| C/N | span50 | **hc0+clamp** | adj40 |
|---|---|---|---|
| 2 dB | 1.2 / 164 | **2.1 / 131** | 1.1 / 164 |
| 4 dB | 2.8 / 82 | **4.1 / 56** | 2.7 / 87 |
| 6 dB | 5.9 / 16 | **6.6 / 16** | 5.7 / 18 |
| 8 dB | 9.0 / 2.0 | 9.0 / 2.5 | 8.7 / 2.5 |
| 14 dB | 14.9 | 14.9 | 14.7 |

- **hc0**: the four origin cells decode to the edge of their quadrant nearest
  the previous phase's quadrant (2 spare decode-address bits; the decode LUT
  is replicated 4x today).
- **clamp**: the map clips the delta to the learned sync-to-white window
  plus a margin, so a click is not a full-scale spike.
- Trained MMSE maps (`designs2.py`, previous-output or confidence context in
  the 2 spare map bits) halve the clicks near threshold but cost 3-5 dB at
  strong signal (they learn to shrink) - rejected.

## Chosen design: HC50

Further designs tested (`designs2.py`, `sweep.py`): trained MMSE maps with
previous-output or confidence context, identity-inside/trained-outside
maps, and an exact "hold previous output" bound. All lose to a plain clamp:
at strong signal two noisy steps regularly land just outside the window, a
clamp moves them to the edge, a hold/MMSE replaces them with a stale value.

Sweep of the clamp margin (4..24 bins) and the HC edge offset (1..16):
**hc edge 4, margin 16** is best or tied everywhere.

| C/N | adj40 SINAD / clicks | **HC50** SINAD / clicks |
|---|---|---|
| 2 dB | 1.1 / 164 | **2.1 / 131** |
| 4 dB | 2.7 / 87 | **4.1 / 54** |
| 6 dB | 5.7 / 18 | **6.6 / 15** |
| 8 dB | 8.7 / 2.5 | **9.2 / 2** |
| 10 dB | 10.8 | **11.2** |
| 14 dB | 14.7 | **14.9** |

HC50 = span50 Phase8 (20 MS/s, no unwrap) + origin cells decoded 4 bins
inside the quadrant edge nearest the previous phase's quadrant + map clamp
16 bins outside the learned sync-to-white window. About 1 dB of threshold
extension over the (infeasible) ideal adjacent detector.

## What fits the 2-bundle loop (exact hardware model, `hw50.py`)

The BitScrambler sets each bundle's LUT address in the previous bundle; the
span50 loop already uses both bundles' address slots (raw decode address and
the counter load). A second lookup per pair - a clamp/map table - does not
fit; three bundles per pair fall back to 13.33 MS/s, span75's problem.

The live span50 program uses only the pair endpoints and computes
`(p_cur - p_prev) mod 256`: a full 2-pi click inside one span vanishes. That is
why the field program already beats the ideal adjacent detector:

| design (SINAD dB / clicks per 1000) | 2 dB | 4 dB | 6 dB | 8 dB | 14 dB |
|---|---|---|---|---|---|
| adj40 (ideal, infeasible) | 1.1 / 164 | 2.7 / 87 | 5.7 / 18 | 8.7 / 2 | 14.7 |
| hw50 (field program) | 1.7 / 141 | 3.6 / 57 | 6.3 / 14 | 9.1 / 2 | 14.8 |
| **hc50p6** | **1.8 / 134** | **3.8 / 51** | **6.4 / 13** | 9.1 / 2 | **14.9** |
| hw50 x2 gain (more DAC levels) | 0.2 / 213 | 0.7 / 187 | 1.8 / 126 | 4.0 / 58 | 13.1 |

**HC50 (built):** `tools/gen_hc50.py` -> `main/fm_hc50.bsasm`, the same two-bundle
program with a new LUT only. Endpoints are rounded to Phase6 so the minus
term's low two bits carry the decoded quadrant; the program already routes
those bits to the bank select, so each decode sees the previous quadrant and
origin cells decode 4 bins inside the quadrant edge facing it. Verified
bit-exact in the repo's BitScrambler emulator (20,255 pairs, 0 mismatches).
On the board in this PR's test state (all v4 programs = HC50).

## 80 -> 40 MS/s PARLIO 2:1 decimation (no fix needed)

PARLIO takes every second sample of the 80 MS/s MODEM_DIAG bus, unfiltered.
`decim.py` (fixed C/N0, so folded noise counts) compares it with an ideal
20 MHz anti-alias filter before the 2:1, HC50 SINAD / clicks at 4 dB C/N:

| analog channel | PARLIO 2:1 | ideal decimation |
|---|---|---|
| +-10 MHz | 3.8 / 51 | 3.8 / 52 |
| +-20 MHz | 3.8 / 52 | 4.2 / 41 |
| +-30 MHz | 3.7 / 53 | 6.4 / 14 |

Board, VTX off, G83 (`tap_psd_board.py`, 160 x 64-sample segments), noise
PSD at the tap relative to 0-4 MHz: 0.0 dB to 8 MHz, -0.6 at 8-10, -5.5 at
10-12, -10.6 at 12-14, then the 4-bit quantization floor (-13..-15 dB). The
analog filter edge is ~+-10 MHz and steep: it is the anti-alias filter, and
the 2:1 decimation costs 0.0-0.1 dB. Keep ANALOG BW at FIXED (+-10 MHz); a
wider analog filter would make the 2:1 cost real.

## In-band spurs per channel (board scan, VTX off)

`spur_scan_board.py`: all 48 channels at maximum gain, noise PSD at the tap
(0.625 MHz bins, DC bin excluded), largest in-band (+-9.5 MHz) bin over the
median. 46 channels: 0.9-2.7 dB (noise scatter, no spur). Two channels carry
the same narrow spur at **5920 MHz = 148 x 40 MHz** (crystal harmonic or the
PLL's integer-boundary spur):

| channel | spur offset | over noise (0.625 MHz bin) |
|---|---|---|
| R8 5917 MHz | +3.1 MHz | 9.4 dB |
| E7 5925 MHz | -5.0 MHz | 8.9 dB |

A CW interferer inside the FM channel raises the effective noise (~1 dB here)
and beats with the video near the range edge. 5840 and 5880 MHz (146/147 x
40 MHz) show nothing. Avoid R8 and E7 until mitigated.

## Open in this PR

- [ ] v4 generator: HC50 as the native program (CVBS level scaling, STATIC/
      HISTORY/mask variants) instead of the swapped C5VRX-3-style program.
- [ ] Sync flywheel / line repair on the span50 formula (off in the test).
- [ ] Remove the temporary verify bypass in `CMakeLists.txt`.
- [ ] Board test of the final program; range comparison.
