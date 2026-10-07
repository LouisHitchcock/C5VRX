# C5VRX-4 sync flywheel

C5VRX by Twotoz and the C5VRX contributors.

This extends the C5VRX-3 flywheel on branch `feat/sync-flywheel` (a PLL line
tracker that repairs broken pulses in the raw ring before the TX read). The
detection, synthesis, vertical interval and re-lock are new for the C5VRX-4
span75 pipeline.

**Goal (operator decision 2026-10-05).** The goggles must never see a moment
without valid PAL/NTSC sync: never "no sync", never the other standard.
HDZero's TP2825 drops lock after ~5 polls (~0.5 s) without V/H lock, and
switches standard after 2 polls with lock in the other one (`HDZERO.md`).

The live path still recovers the transmitted waveform. The flywheel only
supplies the sync pulses the RF lost. AGENTS.md records the exception for
C5VRX-4. Hardware acceptance is pending.

## How it works (`sync_flywheel.c`)

**Detection without demodulation.**
- The 75 ns endpoint phase step is `phase[raw[k]] − phase[raw[k−3]]` (Phase8
  bins per span). Sync is about −38 and blanking about 0.
- It is averaged over three spans for detection, and checked span by span
  (what the TX decodes) before a pulse is kept.
- Each evaluation is a few table lookups. It is valid for any span alignment,
  because the TX grouping is not tagged.
- Host model: ~32 evaluations per clean line, ~50–65 at a weak signal or in a
  fade.

**Line tracking.**
- Acquisition: a step histogram over ~8 lines gives the threshold. Two
  sync-width runs one line apart give PAL (2560 samples) or NTSC (2542.2).
- PLL: phase gain 1/4, frequency gain 1/64, period limited to ±0.3 %. The
  gains drop to 1/16 and 1/256 while pulses are noisy.
- Only pulses with both edges intact (188 ± 12 samples) steer the PLL. A
  pulse whose start the noise ate does not pull the timing late.
- Sync and blanking levels are learned from clean lines.

**Repair.** It runs once locked and with the field phase known.
- A line whose pulse is missing, malformed or not clean span by span is
  rebuilt at the PLL position.
- The rebuild covers 4.7 µs sync plus 1.2 µs of the front porch at blanking,
  so noise cannot move the leading edge.
- While any recent pulse was noisy (rebuild mode), every pulse is rebuilt,
  because a sampled check cannot see every 75 ns span. Rebuild mode ends after
  64 clean lines.
- Synthesis uses a constant phase step per 25 ns sample on strong cells
  (radius 4–6.5 steps). Every 3-byte TX grouping therefore decodes to the
  learned sync or blanking step. The wrap residual to the next real sample is
  spread over the run, so the edges carry no glitch.

**Vertical interval.**
- The first real broad pulse (at a line start or half a line later) fixes the
  field phase on the PLL line grid. The following fields are counted as
  312/313 lines (NTSC 262/263) with a half-line flag, so the field timing
  follows the H-locked grid instead of drifting.
- Every half-line slot is checked:
  - PAL: 5 pre-equalizing, 5 broad, 5 post-equalizing pulses;
  - NTSC: 6 of each.
- A missing or noisy slot is rebuilt as a whole half line.
- A missing first broad pulse with real broad pulses half a line away is a
  parity error: the field shifts instead of getting a second V sync.

**Coasting and re-lock.**
- Without any real sync the flywheel keeps the raster going. After 2 s with
  no carrier the idle raster takes over (`idle_raster.h`). After ~2.5 s the
  flywheel acquires anew.
- While coasting, the previous line is scanned every 8 lines for a real sync
  elsewhere. Two matching finds move the flywheel there in one step: a VTX
  that returns at another phase gives one timing jump, never a double sync.

**Left alone:**
- the colour burst (HDZero Goggles 2 uses colour-carrier detect for its
  PAL/NTSC switch);
- picture content;
- clean pulses.

## Firmware integration (`main/video.c`)

- **Task.** `sync_fw`, priority 3, woken every 100 µs by the V5 timer. The
  timer runs at 100 µs only when the flywheel is enabled. The other V5 tasks
  keep their 200 µs cadence.
- **Positions.** Absolute byte positions are tracked across ring wraps with
  the timer (40 bytes/µs).
- **Write window.**
  - Writes go only beyond the TX descriptor in flight (+512 bytes).
  - The flywheel never analyses or writes the newest completed RX descriptor.
    That is the one every control observer copies (V5 NO_CARRIER coherence,
    idle raster sync, level servo, AFC), so they keep seeing the unmodified
    reception.
  - Host model: synthetic pulses in pure noise would otherwise raise the
    coherence from ~17–21 to ~26–30, above the no-carrier threshold of 25.
  - This leaves a ~150–180 µs window per line, hence the 100 µs wake.
- **Budget.** The work per wake is limited to ~30 µs, from the learned
  ns/evaluation.
- **Native AGC acquisition mask.** It uses the mask phase table, and
  synthesized bytes keep data bit 0 at the unflagged polarity.
- **Paused** during the menu, the idle raster, PHY labs, the RSSI probe, the
  gain sweep and the pre-Q4 probe (labs measure raw noise).
- Internal SRAM is not cached on the C5, so no cache maintenance is needed.

## Controls

| Key | Action |
|---|---|
| `w` | Toggle the flywheel (NVS `c5vrx4/sync_fw`, default on; it was off after the first hardware run starved the task watchdog, USB console and menu, until the 2026-10-06 fix: 200 us tick, 50 us budget, priority 4 so its expiring data is processed on time, phase table in RAM), reboot |
| `!` | `SYNC_FW` line: lock and standard, lines/clean/repaired/slots/rebuilt/missed, V syncs found/coasted, parity fixes, re-locks, acquisitions, skipped lines, floor skips, levels, period, noisy mode, `last_us`/`max_us`, budget, ns/evaluation |

## Host evidence (`tools/test_sync_flywheel.c`)

The test generates synthetic PAL and NTSC CVBS (H sync, equalizing and broad
pulses, burst, picture). It is FM-modulated at 40 MS/s into fine-lane Q4
bytes with a 150 kHz CFO. It runs through the live ring model:
- 4092-byte RX descriptors;
- a 16 KiB TX lag;
- the newest-descriptor ceiling.

The transmitted bytes are decoded exactly like the C5VRX-4 TX program
(endpoint phase, trajectory class, DAC LUT), for all three span alignments.

| Scenario | Input pulses intact | Output pulses at sync level, right width |
|---|---|---|
| Clean, strong | all | all; **0 bytes changed** |
| Deep fade (0.5 field pure noise) + 2 fields of destroyed sync | PAL 819/1600, NTSC 702/1358 | **all**; edge ≤ 15 samples (coasting over 2 fields with no edge at all, ~3 ppm) |
| Weak carrier for 6 fields | 0–2 of 1600 / 1358 | **all**; edge ≤ 9 samples |
| VTX gone, back at another phase | — | all after one re-lock; no double sync |
| Native AGC mask | — | no synthesized byte carries the flag |

The test also checks, after every flywheel call, that the newest completed
descriptor still holds the received bytes.

## Hardware gates

1. **CPU.** `!` → `SYNC_FW last_us/max_us/ns_per_eval/skipped`. On a clean
   carrier, expect a few µs per wake. `skipped` and `floor_skips` should stay
   near zero. Watch the task watchdog and USB.
2. **Clean carrier.**
   - `repaired`, `slots` and `rebuilt` must stay at 0, and the picture must be
     unchanged.
   - Compare with `w` off.
3. **HDZero.**
   - Walk to the range edge, and cover the antenna for 0.5–2 s.
   - With goggle logging on, there must be no `switch` lines and no loss of
     lock while `SYNC_FW repaired` grows.
   - Compare with `w` off.
4. **No-carrier behaviour.**
   - VTX off: V5 must still reach NO_CARRIER / maximum gain.
   - The idle raster must still enter after 2 s.
5. **Re-lock.** Power-cycle the VTX: one short timing jump, no rolling.
6. **Native mode with the mask.** Same as 3, plus `AGC_MASK` unchanged.

## Board result 2026-10-06: default off again

With the priority-4 / RAM-table build the flywheel locked (NTSC, V syncs
found), but on a strong clean carrier (P50 37, Q 99 %) it re-acquired 862
times and rebuilt 2916 pulses in 14 s while handling only ~60 % of the
lines: it fell behind, re-acquired at a new phase and wrote synthetic syncs
off the real ones - black streaks in a clean picture (operator). Default
off until the redesign below.

## Fade-gated repair (2026-10-06, default on)

The flywheel no longer decides on its own when to write:

- **Fade window, detected by the flywheel itself.** Each run samples up to
  32 spans of the new raw data: valid FM video makes 75 ns endpoint steps
  between sync - 12 and white + 12 bins (white = 7/3 of the sync depth above
  blanking); receiver noise lands outside ~40 % of the time. At >= 15 % the
  stretch, widened by one line on each side, is a fade; only lines inside
  it may be written. Independent of the gain owner (native AGC too), no
  hold after recovery, no shared deadline field. The first version used
  V5's coherence < 75 (held 30 ms): review 2026-10-06 showed noise-free Q4
  at radius 4-5 with full 4.667 MHz deviation reads coherence 66-72, so
  valid video opened it. Integral host test: raw IQ -> detector ->
  flywheel; noise-free radius 4 and 5 give 0 detections and 0 bytes
  written; the deep fade is repaired; nothing is written before it or in
  the clean stretch after it. Sync-only damage under a clean picture is
  not a fade and is left alone.
- **Stable lock first.** No write until 64 clean lines after a (re)lock, so
  a fresh lock at a wrong phase can never write.
- **Maintenance tracking.** Outside a window a stable lock measures one line
  in eight (the VTX crystal keeps the period) and always follows the
  vertical interval: ~10 instead of ~31 evaluations per line.
- **Stalls keep the phase.** A stable tracker that falls behind (board:
  7 ms CPU stalls) jumps whole lines on its period instead of re-acquiring
  at a new phase - the cause of the black streaks.

Host evidence (`tools/test_sync_flywheel.c`): clean signal without a window
0 bytes changed, locked with the field phase, 9.8 (PAL) / 10.4 (NTSC)
evaluations per line; 7 ms stalls every 30 ms: one acquisition, 5-6 phase-
keeping jumps, 0 bytes changed; the PAL/NTSC fade scenes with the window
open only around the fades: every H/V pulse on the output for every span
alignment, no byte changed before the first fade. The line repair runs in
the same window (its first dropout line may lack a scored source when the
window opens on it). `!` status: `stable`, `fade_window`, `fade_opens`,
`jumps`, `sampled`.

## Line repair (2026-10-06)

Operator request (Leon, 2026-10-06): repair static streaks from short
dropouts, VCR-style. This **alters picture content**: a dropout line shows an
older line instead of noise. It runs inside the flywheel and needs it on;
menu SETUP `LINE REPAIR` (NVS `c5vrx4/line_fix`, default on since the
operator's goggle check the same day, reboot).

**Acquisition at the real budget.** The first board run with the flywheel at
25 % CPU never locked: the preempted-run cost estimate (357 ns/eval) set the
budget to ~140 evaluations per run, and the stride-3 search needs ~1700
contiguous ones before RX overwrites the data. The search now probes every
45 samples and measures only around a low probe (~140 evaluations per
line); preempted runs no longer update the estimate, and the budget is at
least 256. `tools/test_sync_flywheel.c` locks and keeps every range-edge
pulse at 128 evaluations per descriptor.

- **Detection.** When line L starts, line L-1 is complete. 24 spans across
  its active picture (9.4 us after the sync to 1.5 us before the next line)
  are checked against the learned levels: video lies between half the sync
  depth below blanking and white (7/3 of the sync depth above blanking) plus
  a margin. Pure noise lands outside ~54 % of the time (~13 of 24); a clean
  line 0.
- **Decision.** Repair only a dropout: score >= 8, at least 7 worse than the
  source line, the source clean (<= 3), at most 6 lines in a row, never in
  the vertical interval or on a skipped line.
- **Source.** The line 2 (NTSC, 455 subcarrier cycles) or 4 (PAL, 1135
  cycles) lines earlier: the same colour-subcarrier phase, so the colour
  stays right. It must not be overwritten by RX (`intact_from`: one ring
  behind RX plus two descriptors); the copy goes only to bytes TX has not
  read.
- **Copy.** Bit-exact interior; only the first and last 48 samples (1.2 us,
  in the blanking edges) are re-phased onto the real neighbours, so neither
  seam carries a glitch. Mask mode keeps data bit 0 unflagged.
- **Cost.** 24 evaluations per line plus a ~2.4 KB copy per repaired line,
  inside the flywheel budget.

Host evidence (`tools/test_sync_flywheel.c`, `line_repair_scenario`):

| Case | Result |
| --- | --- |
| Clean signal, repair on | 0 lines repaired, output byte-identical |
| 8 dropouts x 1.6 lines, PAL | 16 repaired; active-picture error 17.3 -> 2.4 DAC codes/span |
| 8 dropouts x 1.6 lines, NTSC | 15 repaired; 17.1 -> 3.4 |
| Mask mode | repaired, no flagged byte written |
| Uniformly weak carrier (range edge) | 23 (PAL) / 11 (NTSC) lines swapped; error 13.44 -> 13.49 / 13.29 -> 13.30 (noise level) |

Rejected while building it (host model, same file):

- Re-phasing every copied byte through the strong cells: ~8-bin quantization,
  ~1 % worse at the range edge. Only the seams are re-phased now.
- Score >= 6 with no margin: at the range edge 224 lines were swapped for
  equally noisy older ones and the picture got 1.3 % worse.

## Decoder: fewer sparkles at the range edge (2026-10-06)

The decoder side of the same request, built after the operator pointed out
that a free table change is worth even a few tenths of a dB
(`generate_phase8.py`, no CPU or runtime cost):

- **Ambiguous (class-3) trajectories** output mid grey (+38 bins, +2 MHz)
  instead of blanking (black). They carry no usable delta; a constant near
  the mean picture level is a smaller error. The optimum is flat over
  25..50 bins. Sync detection is unaffected (blanking and grey are both
  above the sync threshold).
- **HISTORY near-origin prior** widened from radius^2 <= 2.6 / 8 bins
  correction to 9 / 16 (HISTORY decode only; STATIC phases unchanged).

Host model (post-detection model chain, 2^18 samples, Q4/I4 fine lane):

| C/N | SNR before -> after | Sparkles (\|err\| > 25 % FS) |
| --- | --- | --- |
| 3 dB | 1.48 -> 1.90 dB | -6.2 % |
| 5 dB | 3.71 -> 4.02 dB | -5.7 % |
| 7 dB | 6.09 -> 6.22 dB | -2.8 % |
| 9 dB | 8.19 -> 8.23 dB | -1.4 % |

Tried and not taken: the class-3 endpoint delta clamped to the video range
(+0.02 dB), and holding the previous value (not possible in the three-bundle
TX program without a branch; the grey constant gets most of the gain).

Hardware gates: goggle picture through hand-over-antenna dropouts with
repair on/off; `!` status `concealed` / `conceal_no_source` /
`conceal_late`; no task watchdog with flywheel + repair on.

## Limits

- The sync repair cannot recover the picture content of a damaged line; it
  keeps the raster valid. Line repair (above) replaces a dropout line with an
  older one, it does not recover it either.
- Timing when coasting through a long blackout follows the learned period: a
  slow drift of a few ppm, invisible to the goggles. The VTX picture can sit a
  fraction of a microsecond off until the PLL pulls it back.
- A real pulse that is noisy is replaced by a synthetic one at the PLL
  position. That is the intent, but it relies on the PLL tracking the real
  sync (it steers on intact-width pulses only).
- On-chip CPU cost, the 100 µs timer overhead, and TP2825 behaviour are not
  measured.
