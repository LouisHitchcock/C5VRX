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

## Open in this PR

- [ ] Generator: span50 Phase8 program with CVBS150 scaling, STATIC/HISTORY/
      mask variants, hc0 decoder and clamp window (from learned levels).
- [ ] Sync flywheel / line repair on the span50 formula (off in the test).
- [ ] Remove the temporary verify bypass in `CMakeLists.txt`.
- [ ] Board test of the final program; range comparison.
