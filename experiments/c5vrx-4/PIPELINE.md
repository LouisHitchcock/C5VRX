# C5VRX-4 target pipeline: range first, inside one ESP32-C5

Status (2026-10-01): **design specification for the C5VRX-4 experiment.**
No firmware, generator, LUT, PHY setting or board change is made here. This
document merges and replaces two research PRs: #136 (loss budget, model,
fine tap, DAC usage) and #140 (PHY-first architecture, FFT/CFO limits,
span-75 audit, acceptance criteria). It tells the C5VRX-4 build what to
become next and in which order to prove it.

Numbers marked *model* come from an offline model built on the repository's
own signal generator (`tools/hc_estimator_bench.py`) and the live Phase8 LUT.
They are not hardware results. Numbers marked *calc* are analytic.

## 0. Scope and goal

- **Hardware:** one XIAO ESP32-C5, native USB, the existing IQ and DAC pins and
  the existing resistor network. No FPGA, external DSP, second receiver,
  overclock or extra capture lanes. A 5.8 GHz antenna on the existing U.FL is
  allowed and measured separately. A passive output filter is an explicit,
  separately measured option.
- **Gain owner:** native hardware AGC. Firmware may change validated native
  policy, but never selects or forces a gain index.
- **Objective:** at least **6 dB more allowed RF attenuation at equal
  full-colour picture quality**, without losing close-range detail, sync or
  fade recovery. This is a target, not a prediction. Report three outcomes
  separately: sensitivity at matched quality, strong-signal grain/false
  colour, and recovery after fades and overload.

## 1. Decisions in one page

| # | Decision | Why |
|---|---|---|
| D1 | **Measure before building:** attenuator baseline against the goggle's own receiver, stock vs RHCP antenna | No sensitivity number exists anywhere in the repo; "bad range" can be 4 dB or 20 dB |
| D2 | **Stop span-75 / Phase6.** Return C5VRX-4 to 40 MS/s in, 20 MS/s unique out, two bundles per pair | ±6.67 MHz wrap, -3.28 dB PAL chroma (*calc*), identity DAC map, operator saw a less clean picture |
| D3 | **Native AGC, re-acquisition stopped in the PHY:** detect once, hold, re-acquire on a real level event | The packet AGC re-acquires every ~25-50 us with saturated 2.4-3.4 us walks |
| D4 | **First instrument: identify `DIAG[20..31]`** as per-sample gain index and AGC state | Makes native AGC observable live without the crashing MAC-dump capture |
| D5 | **Fixed fine tap `I{9,7,6,5}/Q{9,7,6,5}`** under native AGC | Coarse tap delivers effectively I3/Q3 at the native level; fine is an exact 2x rescale there |
| D6 | **PHY pre-detection filter near Carson bandwidth, switched in only near the edge** | Largest chip-internal threshold lever: about +2.7 dB (*model*) |
| D7 | **Two-stage history-conditioned decoder with a calibrated pair transfer** (HC family) | Fewer clicks (~1.5-2 dB), and the pair LUT gives a calibrated, saturating DAC transfer for free |
| D8 | **Use the DAC:** calibrated transfer from measured deviation and measured loaded DAC voltages | Phase8 FULL puts the whole video swing on ~17 of 64 codes |
| D9 | **Hardware phase/frequency source (#134/#135): bounded probe, not the critical path** | Highest upside, low prior probability; FFT/CFO paths are packet-gated or too slow |

Realistic expectation: **+4-5 dB from chip-internal changes** (roughly
1.6-1.8x distance), **+3-6 dB more from an RHCP antenna**. Reaching the
6 dB objective without the antenna needs D6 to work on hardware *and* D7 to
reach its offline result. Nominal gains overlap and must not simply be added.

## 2. Hard limits

| Limit | Value | Source |
|---|---|---|
| Capture | 8 PARLIO RX lines at 40 MS/s (diagnostic bus runs at 80 MS/s; every second sample) | `SOC_PARLIO_RX_UNIT_MAX_DATA_WIDTH 8`, IDF v6.0.2; docs/continuous-iq-findings.md |
| Bus contents | `DIAG[0..9]` = Q[9:0], `DIAG[10..19]` = I[9:0]; `DIAG[20..31]` toggle only with a carrier, identity unproven | docs/continuous-iq-findings.md, docs/phase5-360-comprehensive-findings.md |
| DSP | one BitScrambler (RX and TX are not two engines), 8 instruction slots, one bundle per DMA clock, one 2 KiB LUT (1024x16 / 512x32 / 2048x8), lookup result in the next bundle | C5 datasheet, TRM ch. 44, RESEARCH.md |
| Cadence | 2 bundles per 50 ns output pair; more bundles only by lowering the unique output rate | RESEARCH.md |
| Ring | 32 KiB, 819.2 us wrap, TX ~409.6 us behind RX; state survives every DMA boundary | docs/continuous-iq-findings.md |
| CPU | 240 MHz ≈ six cycles per input sample before loads; cannot touch samples at rate (sync flywheel: ~0.5 us per code) | docs/range-max.md |
| MAC dump | autonomous writer works, but CPU/GDMA see a stale view while active; full-word capture overwrote live BSS | docs/continuous-iq-findings.md, docs/native-agc-v2.md |
| Output | 6-bit resistor DAC, 40 MHz byte geometry, `[D,D]` hold | README |

These constrain transport and resources, not the formula. The Golden transfer,
Phase8 format, LUT layout and legacy state encodings are not requirements
(AGENTS.md).

## 3. Where the range goes

Reference: an analog FPV receiver with ~18 MHz IF, limiter-discriminator,
video LPF and an RHCP antenna.

| Loss vs reference | Estimate | Evidence | Fix in this design |
|---|---|---|---|
| Linear dual-band antenna vs RHCP VTX | 3-6 dB | physics (3 dB polarization) + unknown antenna gain | D1 antenna A/B |
| Pre-detection noise bandwidth 40 MHz vs ~18 MHz | 2.5-3 dB | *model*; BW20 as fixed mode failed on hardware | D6 |
| Endpoint discriminator clicks | 1.5-2 dB | offline HC bench (docs/range-max.md) | D7 |
| Q4 quantization at the edge | ~1 dB | *model*; Widrow argument in range-max.md | D5 (+0.8 dB) |
| Noise figure (Wi-Fi LNA) | 0-3 dB | unknown | not chip-fixable |
| Self-desense from DAC/PARLIO | not dominant | docs/pre-q4-lab.md `S` runs, weak evidence | - |
| Unexplained remainder | **unknown** | needs D1 | find before tuning |

### Model: detector threshold

Fixed receiver noise σ = 0.56 coarse step per axis (measured at maximum gain),
carrier radius varied, synthetic PAL CVBS, 4 MHz per 100 IRE, CFO 150 kHz,
two seeds x 40 lines. Entries are output SNR (100 IRE over rms error in
0-5 MHz, active video) / clicks per mille (|error| > 40 IRE, unfiltered).

| C/N (40 MHz) | Live P8, coarse | P8, fine tap | Endpoint, 10-bit IQ | ±9 MHz filter + adjacent, 10-bit | 2nd-order PLL, 10-bit | ±12 MHz filter + adjacent |
|---|---|---|---|---|---|---|
| 2.9 dB | 6.0 / 556 | 7.3 / 529 | 8.0 / 517 | **11.5 / 363** | <0 | 8.6 / 525 |
| 4.9 dB | 9.9 / 454 | 11.3 / 417 | 12.0 / 403 | **15.0 / 255** | 11.8 / 380 | 12.6 / 421 |
| 6.9 dB | 14.0 / 346 | 15.1 / 305 | 15.4 / 287 | **17.2 / 160** | 14.8 / 263 | 15.7 / 319 |
| 8.9 dB | 16.7 / 234 | 17.5 / 192 | 17.9 / 177 | 18.8 / 83 | 16.8 / 156 | 17.8 / 215 |
| 12.9 dB | 20.6 / 57 | 16.1 / 183 (folds) | 21.6 / 32 | 21.1 / 11 | 19.8 / 28 | 20.6 / 57 |
| 17.6 dB | 24.0 / 1.8 | 11.7 / 534 (folds) | 25.1 / 0.6 | 22.5 / 1.1 | 21.9 / 1.3 | 22.3 / 4.2 |

At a 15 dB output SNR line:

- Live coarse needs ~7.6 dB C/N, fine tap ~6.8 dB (**+0.8 dB**), full 10-bit
  IQ ~6.7 dB. Quantization costs only ~1 dB at the edge.
- A **±9 MHz channel filter needs ~4.9 dB (+2.7 dB)** and halves the clicks.
  It costs ~1.5 dB SNR on a strong signal (sideband truncation), so it must
  be switched in near the edge, not fixed.
- ±12 MHz gains almost nothing. The filter has to be close to Carson bandwidth.
- The PLL does not beat the plain discriminator at this deviation.
- With fixed maximum gain the fine tap folds above ~3.5 coarse cells. With
  native AGC (trapped ~135 raw) it does not (section 5.4).

Model limits: flat AWGN, ideal FIR (the real PHY filter shape is unknown), no
multipath, no gain transitions, one picture set. The scoring has a half-sample
reference offset that biases all candidates alike, so only relative numbers
are used.

## 4. Target pipeline

```text
 Antenna: stock vs 5.8 GHz RHCP on U.FL (measured, D1)
   |
 Vendor RF init + calibration (pinned libphy hash, vendor fallback)
   |
 Validated ADC/filter tuple: BW40 when strong, ~Carson filter near the edge   (D6)
   |
 Native AGC: CALIBRATE -> ACQUIRE -> TRACK (hold) -> RECOVER                  (D3)
   |   observed live through DIAG[20..31] gain/state lanes                     (D4)
   |
 Stable vendor DC/IQ correction before the tap
   |
 MODEM_DIAG fixed fine tap I{9,7,6,5}/Q{9,7,6,5}                              (D5)
   |
 PARLIO RX 40 MS/s -> 32 KiB cyclic ring (unchanged transport)
   |
 TX BitScrambler, 2 bundles per pair, one 1024x16 LUT:                        (D7)
   lookup 1: (raw byte, previous-state quadrant) -> 5-bit phase state
   lookup 2: (previous state, current state)     -> calibrated 6-bit DAC code (D8)
   |
 PARLIO TX 40 MHz, [D,D] -> 20 MS/s unique CVBS -> 6-bit DAC
   |
 Optional, separately measured: passive ~5.5 MHz LPF with 6.0/6.5 MHz trap
```

Not in the pipeline: CPU gain control, firmware sample HOLD/replacement/masking,
live lane switching, span or rate changes, a PLL, runtime LUT replacement,
regenerated sync, external processors.

The diagram gives ownership and function, not proven ordering inside the PHY.
Tap position and filter/correction placement have to be measured (section 5.3).

## 5. Stages

### 5.1 Baseline and antenna (D1)

Without this every later claim is a guess. See section 8, round 1.

### 5.2 Native AGC: stop re-acquisition at the source (D3, D4)

**Problem, measured (docs/native-agc-v2.md):** the C5 AGC is an 802.11 packet
AGC. On a continuous FM carrier it detects, walks the gain for 2.4-3.4 us
with saturated samples (the thin black/rainbow dashes), traps a *different*
gain (the line-to-line texture), aborts and re-detects every ~25-50 us. About
6-13 % of samples are acquisition garbage. No demodulator can recover them.
The paced experiment (1 ms period, 20 us window) was reported "een stuk
beter", which confirms the direction: fewer acquisitions.

**Step 1: identify `DIAG[20..31]` (D4).** `DIAG[0..19]` equal dump-word bits
0..19. The dump word carries the gain index in bits 20..27 and the AGC FSM
state in 28..31 (ESPARGOS esp-sdr layout, seen in the RF dump). `DIAG[20..31]`
toggle only with a carrier, which fits. Use the `phy_phase_tap_probe`
correlation method: route `DIAG[20..27]` and then `[24..31]` to the 8 PARLIO
pins in a bounded capture and align against dump bits. If it holds, PARLIO
can record per-sample gain and state at 40 MS/s, without MAC-dump ownership.
That turns every policy question below into a measurement.

**Step 2: find what ends each "packet".** Record the FSM sequence around an
abort. Candidates: SIG/header failure, a length or timeout limit, a
power-change restart, packet-detect / rx-sense / CCA thresholds (`7068`,
`7010/7014/7044`, `701C`). Earlier sweeps changed one field per boot and only
looked at the trapped radius, never at why the FSM left the trapped state.
For every candidate from #135, recover arguments, callers, MMIO writes and
packet side effects before use. A symbol name is not an interface.

**Step 3: detect once, then hold.** Suppressing detection was found to freeze
the gain. Not yet tried: allow one acquisition, then suppress detection while
keeping a hardware power-change or overload restart if one exists. Result:
hardware gain choice, zero switching while the level is steady, and hardware
re-acquisition on a real fade.

**Supervisor (firmware, no gain writes):**

| State | Behaviour |
|---|---|
| CALIBRATE | vendor ownership for retune, channel, bandwidth or offset changes |
| ACQUIRE | native hardware converges; filter, lane map and program fixed |
| TRACK | validated hold policy; zero PHY writes while healthy |
| RECOVER | release tracking at once on a trusted overload/level event or expired acquisition evidence; no-carrier must keep re-acquisition possible and never freeze a low gain |

**Fallback, operator decision required:** the paced gate (`7030[29]`) with
its open window placed in vertical blanking, so the walk lands on lines the
display does not show. Hardware still chooses the gain, but it is a CPU-timed
gate. That conflicts with the continuous-native invariant in AGENTS.md and
with the rule against firmware gain workarounds. Accept it only after proving
resume, strong-to-weak and weak-to-strong recovery and no new periodic
artifacts. The worst case waits almost one period before an opportunity:
measure real latency, not the nominal timer.

Related, measure once D4 works: the rx-comp level fields (`702C[7:0]`,
`70A0[31:24]`) moved the trapped radius 92-156 in the sweep, but the live
check was inconclusive. A trapped radius of ~180-200 raw would use the fine
window better. `7034[30:24]=127` is an encoded setting, not a calibrated speed.
Isolate it from everything else.

### 5.3 Pre-detection filter through the PHY (D6)

The BitScrambler cannot filter complex IQ: a two-sample prefilter needs a
16-bit address. The filter has to be the PHY's.

Method: for every vendor-valid ADC/filter/BW tuple (including the 5830 MHz
configuration boundary), sweep a tone and modulated video and measure the
complex response, group delay, DC/IQ effects and equivalent noise bandwidth
`ENBW = ∫|H(f)|² df / |H(0)|²` **at the MODEM_DIAG tap**. Choose the lowest
ENBW that keeps FM sidebands, real CFO, sync, PAL/NTSC chroma and detail.

Three things the earlier BW20 test (Hanover bars) did not control:

1. **Tap effect.** First check whether BW20 changes the tap at all: VTX off,
   `noise_r2` at BW40 vs BW20. If the noise does not drop, stop.
2. **Carrier centring.** A ±9 MHz filter with 1-2 MHz CFO clips one set of
   sidebands. Centre first with a burst-confirmed porch reference (AFC V2,
   `feat/native-agc-v2`), not with a spectral centroid: the mean FM frequency
   depends on picture content.
3. **Edge-only use.** Switch in only when native gain is at maximum and
   coherence falls, read from the D4 gain lanes, and only in CALIBRATE/ACQUIRE
   transitions. A narrow monochrome survival mode must be reported separately
   from full-colour range.

`phy_rx_filter_mode` has more states (0/4/8, coupled to the ADC rate). Test
them only as whole vendor tuples, never as a free coefficient.

### 5.4 Fixed fine tap (D5)

Native AGC traps at ~135 raw, spread 80-440 (docs/native-agc-v2.md). For
|v| < 256, bit 8 of a 10-bit two's-complement value equals bit 9. **So for
almost every trapped sample the coarse tap `{9,8,7,6}` carries only 3 useful
bits per axis**, which is radius ~2.1 cells. The fine tap `{9,7,6,5}` is an
exact 2x rescale inside ±256: same angle, same LUT geometry, radius ~4.2
cells. Only acquisition samples fold, and those are saturated garbage on the
coarse tap too.

*Model*, native level (radius 135 raw), strong signal:

| C/N | Tap | Luma rms (IRE) | Chroma-band rms (IRE) |
|---|---|---|---|
| 25 dB | coarse | 4.15 | 5.18 |
| 25 dB | fine | 2.79 | 3.82 |
| 35 dB | coarse | 4.00 | 4.69 |
| 35 dB | fine | 2.75 | 3.57 |

This agrees with the earlier false-colour simulation (radius 2.5 -> 3.9 IRE,
radius 4.0 -> 2.4 IRE).

**Gate (lighter than a full-word proof, which crashes the normal build):**
measure the fold rate on fine lanes from the D4 gain lanes plus `E` rows at
close and far range. Accept when the folds sit inside acquisition walks and
trapped samples stay inside ±256. If D4 is not available, an operator A/B of
P8 FULL vs P8 FINE under native AGC at two RF levels is the fallback evidence
(P8 FINE exists on `feat/native-agc-v2`).

The lane map is fixed per run. No live lane switching: native AGC already
normalizes the level, routing eight lanes is not atomic, and a second actuator
would fight the first. Ultrafine `{9,6,5,4}` and region-aware decoding of
folded codes come back only if D3 narrows the trapped spread.

### 5.5 Decoder: history-conditioned, two lookups (D7)

Contract (layout established by `main/fm_hc.bsasm`; field sharing as in Golden):

| Bundle | Result used | Next lookup | Stream work |
|---|---|---|---|
| phase/pair | current 5-bit state | previous state + current state | retain current state |
| emit/decode | 6-bit calibrated DAC code | next raw byte + previous-state quadrant | read 16, write 16 `[D,D]`, loop |

Both 1024-address functions share the same words: decoder state in bits 8..12,
DAC code in bits 0..5.

Estimator, derived offline and frozen per accepted receive profile:

- Decoder: for raw cell region R (fine-lane geometry; a signed cell `s` is the
  interval `[32s, 32s+31]`, not a point) and previous quadrant h, compute
  `p(φ | R, h)` from the measured noise/amplitude model at the native radius
  distribution, then pick the state that minimizes expected circular phase
  loss. Reliable cells keep their unbiased state.
- Pair table: signed circular state step -> physical frequency -> video level
  -> nearest *measured loaded* DAC voltage, with saturation outside the
  validated video envelope and never an unsigned wrap. All 32x32 pairs are
  defined.
- Training: content-independent FM/noise model plus independent held-out
  picture/seed/CFO sets. **No MMSE pair table:** it learned picture content
  and biased sync and luminance.

Offline evidence: HC has ~40 % fewer clicks than Phase8 at C/N 5-12 dB
(~1.5-2 dB) and beats Golden everywhere; Phase8 is slightly finer on a strong
signal (luma 3.0 vs 3.4 IRE) (docs/range-max.md). With the fine tap the
strong-signal gap must be measured again.

**Optional upside, HC8:** an 8-bit MAP phase decoder addressed by
`(previous quadrant, raw)`, with the difference on Counter A as in live
Phase8. Live Phase8 already feeds LUT address bits 24..25 from low bits of the
retained term, so the quadrant could go there. Open problems: it needs a
second lookup for the calibrated transfer (D8) and a dataflow proof against
the counter load. Pursue only if the 5-bit decoder loses on strong-signal
detail.

### 5.6 Output: use the DAC (D8)

Phase8 FULL maps ±128 bins (±20 MHz per 50 ns) to 64 codes to avoid a modulo
cliff. One code is 312.5 kHz, about 7.8 IRE at the bench's 40 kHz/IRE, so
sync-to-white uses ~17 codes. That alone is a ~2.3 IRE rms quantization floor,
and the display amplifies it along with the small video level. *Model*, strong
signal, saturating transfer:

| Input | Slope (code/bin) | Codes used by video | Luma rms (IRE) |
|---|---|---|---|
| 10-bit IQ | 0.25 (live) | 15 | 1.52 |
| 10-bit IQ | 0.50 | 29 | 1.08 |
| fine tap, r = 135 | 0.25 (live) | 19 | 2.08 |
| fine tap, r = 135 | 0.50 | 33 | 1.71 |

The D7 pair table provides this transfer without an extra lookup. Inputs it
needs, all measured rather than assumed:

- real VTX deviation, polarity and CFO spread (from `z` windows or learned
  sync/porch levels);
- all 64 **loaded** DAC voltages and transition glitches with real GPIO drive,
  tolerances and termination (the C5VRX-4 nominal inversion maps every code
  to itself, so it calibrates nothing);
- the final level into 75 ohm.

Reconstruction (*calc*): branches 8200/3900/2000/1000/470/240 ohm in parallel
are 122.36 ohm; with the 200 ohm shunt and a 75 ohm display, 37.73 ohm. With
470 pF that is a single pole at ~8.98 MHz, not a video filter. The output
carries FM noise up to 10 MHz (power rising with f²), the RTC6705 audio
subcarriers at 6.0/6.5 MHz and Q4 harmonics. A passive LC low-pass at
~5.5 MHz with a 6.0-6.5 MHz trap is what analog receivers use. It changes the
tested DAC network, so it needs an explicit decision, PAL chroma at 4.43 MHz
must survive, and peaks near 6 MHz must first be checked against picture
level/CFO (they can be Q4 harmonics rather than audio).

### 5.7 Hardware phase/frequency source (D9, issues #134/#135)

The highest-upside same-chip idea: a packet-independent phase byte, short-lag
frequency or post-filter IQ on the diagnostic bus would remove the Cartesian
Q4 bottleneck. Keep it bounded, because the evidence is against it:

- Public CSI and LLTF dumps come from Wi-Fi training fields; analog FM has none.
- An FFT is too slow for video (*calc*): 64 points at 20 MHz is a 3.2 us
  aperture (312.5 kHz non-overlapped rate). Use FFT/power only as an observer
  for blockers, occupied bandwidth and scanning.
- A lag-T CFO detector is unambiguous only for |f| < 1/(2T) (0.8 us ->
  ±625 kHz). Never subtract tracked FM as "CFO".
- The earlier phase-tap probe found no phase source in its tested selector states.

A candidate source is promoted only if, on non-Wi-Fi FM, it shows: 0-5 MHz
video transfer including PAL chroma; at least 20 MS/s useful cadence with no
packet dead time; signed span covering CFO plus peak deviation; at most eight
lanes, consumable by PARLIO; correct alignment and no hidden resets. Use a
fixed probe budget; stop if no selector qualifies.

## 6. What happens to the current C5VRX-4 prototype

| Prototype part | Verdict | Reason |
|---|---|---|
| Span-75 Phase6, 13.33 MS/s `[D,D,D]` | **replace** with the 2-bundle 20 MS/s shape | wrap ±6.67 MHz vs ±10; PAL chroma detector+hold -3.28 dB vs -1.43 dB (*calc*, ~1.86 dB worse); less phase precision; operator: less clean than C5VRX-3 |
| Coarse tap `I[9:6]/Q[9:6]` | **replace** with fixed fine tap | effectively I3/Q3 under native AGC |
| Nominal DAC inversion (LUT[256..319]) | **replace** with measured loaded voltages | identity for all 64 codes |
| Forced native ownership, own NVS namespace | keep | matches D3 |
| Paced gate 1000/20 us, profile 127 | keep as comparison baseline only | evidence that fewer acquisitions help; not the target policy |
| `~` / `T` controls, isolated build | keep | test instrumentation |

## 7. Rejected or deferred

| Candidate | Verdict | Reason |
|---|---|---|
| PLL / tracking demodulator | drop | *model*: no gain at video deviation; does not fit the BitScrambler |
| Channel filter in the BitScrambler | impossible | needs a 16-bit pair address |
| More than 8 IQ lanes | impossible on this design | PARLIO RX is 8 lines; one BitScrambler cannot merge a second capture |
| Live lane switching | deferred | not atomic, second actuator |
| One-bundle recurrent LUT layouts (8+2, 6+4, 5+5 bits) | deferred | fit capacity but lose input or history; explore only if D7 fails |
| CPU sync flywheel | parked | covers ~1 line in 6 (docs/range-max.md) |
| Direct Gain V5 | reference only | firmware gain controller; main's default, not this design |
| External LNA, diversity, FPGA | out of scope | separate hardware designs |

## 8. Experiment plan

### Round 1: one board, a few attenuators (decides direction)

Equipment: VTX, SMA attenuators (10/20/30 dB and 1 dB steps), the goggle's own
receiver as reference. One change per A/B.

| Step | Test | Stop rule / output |
|---|---|---|
| 1 | Sensitivity baseline: attenuation at "usable picture" and "sync lost", C5VRX-3 and goggle RX, 3 repeats | the dB gap; if far beyond section 3, look for a defect first (antenna/cable, CFO, RF stage held low) |
| 2 | Stock vs RHCP antenna at the step-1 threshold | antenna dB |
| 3 | `DIAG[20..31]` identity (bounded capture + dump correlation) | no match after full alignment search -> policy work continues blind, by picture A/B |
| 4 | Fine vs coarse tap under native AGC, two RF levels | fine visibly worse -> check fold rate |
| 5 | BW40 vs BW20 noise at the tap, VTX off | unchanged -> drop D6 |
| 6 | Abort cause, then detect-once-hold trial | no hold mechanism -> operator decides on the blanking-aligned paced fallback |

### Round 2: acceptance (before calling C5VRX-4 better)

- **Range:** extra allowed attenuation at matched full-colour quality, 1 dB
  steps near threshold, at least five ascending and descending sweeps, spread
  and calibration uncertainty reported. Objective 6 dB.
- **Clean signal:** luma/chroma response within 1 dB of the reference, loaded
  levels within 5 %, burst phase within 5° after delay alignment, fine detail
  visibly kept.
- **Grain:** at least 3 dB less flat-area luma/chroma-band noise where grain is
  visible, not by removing detail. False-colour tones scored apart from random noise.
- **Recovery:** ±10/20 dB level steps; native recovery within 2 ms where the
  carrier stays decodable; display relock measured separately; deep loss never
  traps a low gain.
- **Transport:** 30 minutes plus retunes and menu transitions; counters plus
  coherent input/output evidence.
- **Conditions:** PAL and NTSC, dark and flat colours, multiburst, motion, CFO
  extremes, an adjacent analog VTX, 5 GHz Wi-Fi. Label single-board results as such.

Fix criteria before measuring, publish raw captures and failed comparisons, and
never mix a gain-owner change into a demodulator comparison.

## 9. Open operator decisions

1. Is the blanking-aligned paced gate acceptable as a fallback if no hardware
   hold exists (5.2)?
2. Is a passive LC output filter allowed as a measured option (5.6)?
3. Is the antenna counted in the 6 dB objective or reported separately (0)?

## Sources

Repository: docs/native-agc-v2.md, docs/native-agc-paced.md, docs/range-max.md,
docs/continuous-iq-findings.md, docs/arc-receive-chain.md,
docs/fix-cvbs-jitter-and-static.md, docs/pr-derived-findings.md,
docs/pre-q4-lab.md, docs/phase8-range-envelope.md, RESEARCH.md, DESIGN.md,
`main/fm_hc.bsasm`, `main/fm_phase8_hr_live.bsasm`, `generate_pipeline.py`.
External: [C5 soc_caps (IDF v6.0.2)](https://github.com/espressif/esp-idf/blob/v6.0.2/components/soc/esp32c5/include/soc/soc_caps.h),
[C5 datasheet](https://documentation.espressif.com/esp32-c5_datasheet_en.html),
[C5 CSI description](https://docs.espressif.com/projects/esp-idf/en/v5.5.4/esp32c5/api-guides/wifi.html#wi-fi-channel-state-information),
[ESPARGOS esp-sdr](https://github.com/ESPARGOS/esp-sdr),
[XIAO ESP32-C5 wiki](https://wiki.seeedstudio.com/xiao_esp32c5_getting_started/).
