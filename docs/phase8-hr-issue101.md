# Issue 101: Phase8-HR arithmetic oracle

This branch tests the endpoint-only, two-bundle arithmetic proposed in issue
#101. The generated `bs_phase8_hr_probe.bsasm` is a finite boot-time loopback
oracle. It is **not** loaded into the live 6BIT@40 video path. After the probe,
the firmware starts the normal mainline video program.

## Working two-bundle dataflow

Each of the 256 raw Q4/I4 bytes maps to a Phase8 code using the center of
its truncated signed nibble cell. A 1024-word LUT repeats the raw mapping in
four banks, because the previous endpoint term occupies bits 24..25 of the
worker's LUT address. Each word packs two eight-bit terms:

`minus = (80 - 3*phase8) mod 256`, `plus = (3*phase8) mod 256`.

The worker loads the full 16-bit Counter A with the next raw address in its
low byte and `minus(current)` in its high byte. The controller adds
`plus(next)` into the high byte while adding zero into the low byte. The next
worker emits `A[10:15]` twice, reads the next IQ pair, and loads Counter A
again. Eight unrolled slots wrap without a jump; there are exactly two bundles
per 50 ns pair. The first two output pairs are startup values.

## Host results

`python tools/gen_phase8_hr.py` and `python tools/test_phase8_hr.py` assemble
and simulate the source program across all 65,536 ordered raw endpoint pairs.
The mapping and duplicated output agree with the modular arithmetic for every
pair. There are 166 distinct Phase8 codes across the 256 Q4/I4 cells. Relative
to each cell center's exact angle, Phase8 quantization has 0.318 degrees RMS
error; the existing Phase5 centroid has 3.224 degrees RMS error. These are
quantization figures, not measured image-quality gains.

The proposed direct six-bit extraction is unsafe for live video. With bias 80,
the eight-bit sum has the desired linear interpretation only for signed Phase8
deltas from -26 through +58. Across all ordered raw pairs, 43,762 overflow
that range. A -32-bin sync-like step would emit DAC code 60 instead of code 0.
The live acceptance gate therefore requires a guard or different arithmetic
which cannot turn a valid sync tip into white, while retaining the two-bundle
cadence. This oracle does not solve that guard.

The optional boot probe prints `BS_PHASE8_HR status=...` and `p` repeats it
after startup. A hardware PASS validates the full-width Counter A schedule
for its representative transitions; it does not validate live picture quality
or the unsolved range guard.

## Silicon result (ESP32-C5 v1.0, 2026-09-28)

The flashed oracle reported:

```text
BS_PHASE8_HR status=PASS written=256 mismatches=0 err=ESP_OK
```

The same USB snapshot showed `tx_empty=0`, `rx_ovf=0`, `bs_empty=0`, and
`bs_eof=0` after normal mainline video startup. At that moment `p=0` and
`sync_q=0`, so the VTX was not supplying a usable signal for picture quality
assessment. This is a successful arithmetic/transport oracle, not a live
Phase8-HR video result.
