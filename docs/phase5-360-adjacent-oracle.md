# Phase5-360: exact adjacent oracle and live hardware gate

This experimental PR computes the exact Phase5-domain adjacent result from
completed raw-IQ snapshots. It **does not route this result to the live DAC**.
The existing 40 MS/s raw ring, two-bundle Golden/Phase5c BitScrambler, semantic
sync observer and 6-bit output remain unchanged.

## Exact reference

For three consecutive 25 ns raw Q4/I4 samples, decode phases `P`, `M`, `C`:

```
first = wrap32(M - P)
second = wrap32(C - M)
adjacent = first + second       # no second wrap
endpoint = wrap32(C - P)

if adjacent == endpoint:
    dac = calibrated_golden[P, C]
else:
    dac = clamp(20 + 6 * adjacent, 0, 63)  # calibrated slope, always 0/63
```

Each 25 ns step is interpreted by the Phase5 shortest arc. This cannot infer
physical jumps of 180 degrees or more within a single 25 ns interval, and
low-magnitude IQ still makes phase estimates unreliable. "Exact" describes
the stated Phase5-domain model, not an error-free analog FM reconstruction.

`tools/gen_phase5_360_oracle_lut.py` copies the raw-to-phase and calibrated DAC
maps from `fm.bsasm` and the live endpoint DAC map from
`fm_phase5_360.bsasm`. Build validation rejects stale copies. The host test
checks all 32,768 Phase5 triples against an independent wrap/sum expression:

- 24,576 triples have no winding and preserve the calibrated Golden byte.
- 8,192 triples need winding handling; their exact reference DAC differs from
  the live endpoint output in this exhaustive *state-space* enumeration.
- 5,494 triples cross the `DAC <= 8` sync-tip threshold relative to live.
- The current live `fm_phase5_360.bsasm` endpoint DAC table is byte-identical
  to `fm.bsasm` for all 1,024 endpoint pairs. Its worker branches also emit
  the same DAC bits. Its present name does not imply live adjacent FM.

The percentages above are not RF error rates: legal triplets are not equally
likely on a particular VTX, and noisy near-origin triples can make the oracle
worse than the endpoint path. Hardware observations must separate strong-IQ
events, sync-tip changes and actual visible defects.

## Firmware observation

The existing 50 ms supervisory task examines a completed ~102 us DMA window.
Its `?` diagnostics now report:

```
Phase5-360 oracle (read-only): DAC delta=... strong=... sync-tip flip=...
```

`DAC delta` is the fraction of sampled 50 ns endpoint intervals where the
ideal adjacent result differs from the current live DAC. `strong` is the same
fraction restricted to triples that pass the existing strong-IQ power filter.
`sync-tip flip` counts an oracle/live disagreement across DAC code 8. No
oracle value drives PHY gain, PAL/NTSC detection, the semantic sync observer,
the output ring or the live DAC.

For a useful capture, compare those counters and a user `L` lag mark with a
recording of the same live video. A high oracle correction rate alone does not
show that enabling it will improve the picture.

## Missing live schedule

The proven TX-only path has **two BitScrambler bundles per 50 ns output pair**.
Golden spends one LUT access decoding a raw 8-bit endpoint to Phase5 and the
other looking up its calibrated endpoint DAC. Full adjacent needs the 5-bit
middle phase too. The earlier two-stage exact Phase5-domain LIFT proof assumes
both new raw samples have already been decoded; it does not schedule their
raw-to-Phase5 conversion. A one-bit middle sign is insufficient: legal
triplets with the same sign can require opposite winding decisions.

The project's four-bundle TX-only Phase6 attempt produced an empty TX FIFO,
and concurrent RX+TX BitScramblers failed on this C5. Those are measured
negative results. Direct LUT factorization lower bounds in
`docs/golden360-feasibility.md` further constrain the known two-bundle routes,
without ruling out every possible counter/logic design.

The alternating-middle silicon probe does decode both new raw samples within
the two-bundle cadence, but that uses both LUT accesses. A further direct-LUT
continuation would have to retain all 1,024 distinct `(previous,middle)`
continuation rows (10 bits) and address the next raw-current byte (8 bits).
That 18-bit address exceeds even the LUT8 mode's 11 address bits. The new
exhaustive check in `tools/prove_golden360_capacity.py` confirms the row count
for the Golden-preserving adjacent target. This rules out that *specific*
direct final-lookup scheme; it does not rule out untested counter logic,
additional hardware, or a different signal-path architecture.

### Can the middle sample be compressed?

Yes, **after** both endpoint phases are known: for 992 of the 1,024 endpoint
pairs, the possible winding outcomes across all middle phases are exactly two
(`0` and one direction). The other 32 pairs always have winding `0`. Thus the
final correction for fixed endpoints is one bit at most. But that bit is a
predicate of *all three phases*, not an endpoint-independent middle-sample bit.
Before the current endpoint arrives, even fixing the previous phase leaves 32
distinct middle-phase continuation patterns across possible current phases.
An exact early summary still needs all five middle Phase5 bits. The exhaustive
counts are asserted by `tools/prove_golden360_capacity.py`.

There is an exact small **endpoint-conditioned interval circuit**. Let
`u = (M-P) & 31` and `e = wrap32(C-P)`. Then:

```
if e >= 0: k = -1 when 16 <= u <= 16+e, otherwise 0
if e <  0: k = +1 when 16+e < u < 16, otherwise 0
```

The proof checks this against both wrapped adjacent differences for all
32,768 triples. For the winding case, the exact adjacent sum is `e + 32*k`.
At Golden's P20/G2 slope this always clips to 63 for `k=+1` or 0 for `k=-1`;
otherwise emit the Golden calibrated DAC byte. An earlier oracle used
`20+2*adjacent` on winding events, a softer transfer inconsistent with Golden's
calibrated slope. This reduces the **final
decision** to an interval predicate, but the circuit still needs five bits of
middle phase until `C` is decoded, a modulo-32 subtraction, comparisons and
the calibrated output selection. No two-bundle single-C5 implementation of
those operations has been demonstrated.

A feasible additional-hardware design would decode each raw byte at 40 MS/s,
retain `P,M,C`, evaluate the two shortest-arc deltas and sum in a parallel
combinational datapath, then apply the calibrated Golden DAC for no winding.
It needs a third computation path beyond the two measured C5 TX LUT accesses,
or a proved C5 counter/bit-routing implementation of the same predicate and
DAC transfer. This is an architectural target, **not** a new live C5 mode.

`hardware/phase5_360/` now contains a concrete external-logic reference:
the generated 256x5 raw Phase5 ROM feeds an interval comparator; the final
DAC ROM takes exactly `{winding_flag, P[4:0], C[4:0]}` (11 address bits,
2048 entries). The exhaustive generator verifies 32,768 triples. The RTL
has not been synthesized, wired to a shared 40 MHz source clock, or tested
on an FPGA; the XIAO's existing DAC pins cannot be driven by both devices.
This is a plausible **additional hardware** escape, not an 11-bit C5-only
BitScrambler factorization.

The earlier Golden DAC overlay was dropped: C5 TX starts nominally half a
ring (8,192 output pairs) behind raw RX, so a two-bit correction would need
an independently proven clock/pair lock and a large delay buffer. The direct
pipeline registers phase decode, interval comparison and DAC lookup over
successive 25 ns clocks, emits one code each 50 ns with fixed latency, and
removes the C5 Golden stream from the external DAC timing path. Its source
model checks 18,000 continuous raw-IQ pairs; Verilog synthesis and silicon
timing remain open.

### Partial correction with fewer middle bits

For an optional conservative demodulator, a coarse middle token can correct
only when **every** Phase5 middle compatible with that token gives the same
nonzero winding for the endpoint pair. All ambiguous tokens emit Golden. The
exhaustive `tools/probe_middle_compression.py` checks the raw Q4 map and all
endpoint phases; its outputs are state-space coverage, not on-air rates:

| Middle token | Corrected raw winding states | Corrected Phase5 winding triples |
| --- | ---: | ---: |
| Best 1 direct raw bit | 0 / 65,536 | 0 / 8,192 |
| Best 2 direct raw bits (3, 7) | 16,384 / 65,536 | 2,304 / 8,192 |
| Best 3 direct raw bits (3, 6, 7) | 25,856 / 65,536 | 4,136 / 8,192 |
| Best 4 direct raw bits (2, 3, 6, 7) | 40,576 / 65,536 | 6,696 / 8,192 |
| Top 3 **decoded** Phase5 bits | 43,264 / 65,536 | 5,408 / 8,192 |
| Top 4 **decoded** Phase5 bits | 57,600 / 65,536 | 7,200 / 8,192 |

Even this conservative option is **not scheduled** on the existing Golden
core: its final endpoint DAC lookup already addresses 10 phase bits in LUT16
mode, while LUT8 offers at most one extra address bit (11 bits total). One
direct raw-middle bit has zero safe corrections; two need at least 12 address
bits on that direct lookup. Decoded Phase5 hint bits also require the missing
middle-phase decode. These offline results therefore identify a quality and
capacity trade-off for a redesigned datapath, not a flashable partial mode.

Before any live `PHASE5-360` mode or flashable claim, a candidate must:

1. Decode both raw Q4/I4 samples and retain the previous phase with one C5 TX
   BitScrambler, no CPU work in the 40 MS/s path.
2. Emit the exact oracle result in at most two bundles per 50 ns pair, within
   the resident LUT budget, with semantic sync matching the final DAC.
3. Pass exhaustive raw/phase source-model equivalence, boundary-continuity,
   assembler and sustained C5 video/transport tests.

Until those conditions pass, keep the live demodulator on the known Golden
path. A successful ESP-IDF build of this oracle proves only the observation
path and must not be described as a live Phase5-360 receiver.
