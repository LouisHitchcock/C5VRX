# 11-bit Phase5-360 external logic reference

This is a **source-level FPGA/CPLD candidate**, not a wired or measured board.
It is independent of the C5's two BitScrambler bundles: an external 40 MHz
logic device samples the eight packed Q4/I4 diagnostic pins, decodes each
raw byte with the 256x5 Phase5 ROM, retains `P,M,C`, computes the exact
endpoint-conditioned interval winding, then addresses a 2048x6 DAC ROM.

```
ROM address[10:0] = {winding_flag, previous_phase[4:0], current_phase[4:0]}
```

For flag zero, the ROM returns the **byte-exact** calibrated Golden DAC code.
For flag one, the Golden P20/G2 transfer clips full adjacent winding to code
0 or 63; its polarity is fully determined by the endpoint pair. The external
logic computes the one-bit flag from the middle phase only **after** the two
endpoints are known. Thus 11 address bits are enough for the *final* output
lookup even though early middle compression cannot be exact.

Run `python tools/gen_phase5_360_11bit_rom.py` to regenerate both checked-in
ROM files; `--check` validates them. The generator verifies all 32,768 Phase5
triplets against the independent wrapped adjacent oracle. The reference RTL
accepts alternating first/second raw samples at 40 MHz and updates DAC at
20 MHz after one priming pair.

## Hardware integration gate

- The C5 currently routes eight MODEM_DIAG Q4/I4 lines onto its eight free
  XIAO pads and owns six separate resistor-DAC pins. An external device can
  **tap** the raw pins, but must not electrically fight the C5 DAC outputs.
  The board/firmware must explicitly hand DAC ownership to the logic device,
  or use a separate switched DAC output. No wiring changes are included here.
- A source-synchronous 40 MHz sampling clock, input setup/hold, ROM/comparator
  timing, analog DAC loading and power integrity need synthesis and measurement.
  The C5's local PARLIO sampling clock is not an automatically proven external
  clock for this device. Pair parity must be set once and held across buffers.
- Bench qualification must compare the reference output with Golden at chroma,
  sync and weak signal, including near-origin raw IQ. Exact Phase5-domain
  winding can still be wrong for noisy analog input or physical 25 ns motion
  beyond the shortest-arc assumption.

An average or median cannot recover the missing winding when two triplets
have identical endpoints but different middle routes; it can only filter
noise *after* the route is identified. This circuit retains the information
until the endpoint arrives and then compresses the correction to one bit.

## Golden overlay candidate (keep C5's live demodulator unchanged)

`phase5_360_sidecar.v` models a second, potentially cleaner wiring option:
tap the C5's raw 8-bit IQ **and** six Golden DAC digital lines. It computes
one two-bit action for each IQ pair (`PASS`, `FORCE_LOW`, `FORCE_HIGH`), delays
the action stream to the corresponding Golden output pair, then digitally
selects the Golden code or rail. The C5 remains on its proven two-bundle
Golden program; the sidecar never tries to replace its FM calculation.
`tools/test_phase5_360_sidecar.py` exhausts the correction identity and
demonstrates why a one-pair offset corrupts synthetic output. It also models
a fail-closed Golden-sequence correlation check: accept only a unique exact
lag over a window with enough transitions; reject flat or slipped output.
That Python gate is an illustrative source-domain model. It is **not** an
implemented clock-domain-safe FPGA lock and does not prove that real analog
video always offers a unique alignment signature.

**Alignment is the hard integration gate.** Current production code starts TX
after `RAW_RING_BYTES/2 = 16,384` raw bytes at 40 MS/s, nominally **8,192
50-ns output pairs / 409.6 us** behind RX. Driver startup and BitScrambler
prefetch add unmeasured offset, and RX/TX clock phase or restart behavior may
change it. The overlay's parameterized two-bit action FIFO defaults to 8,192
entries (2 KiB), **not an established correct delay**. A companion six-bit
expected-Golden FIFO (6 KiB) uses the Golden ROM only as an alignment
cross-check; a differing C5 Golden code causes immediate PASS. Matching one
code does not establish alignment. The overlay otherwise passes Golden
untouched until `alignment_valid` is asserted by a future verified lock
circuit. This reference does not generate that signal. The real design
must compare a predicted Golden sequence against tapped C5 Golden output,
establish the exact pair parity and lag, and promptly drop the lock on
ambiguity, menu, transport fault, clock slip, output-mode change or reset.
The XIAO's DAC GPIOs must feed sidecar **inputs**, with a separate six-bit
digital output/mux driving the resistor network; tying two push-pull outputs
together is unsafe. This requires wiring changes and clock-domain analysis.

The C5's compiled PR firmware still emits unmodified Golden. Neither source
RTL has been synthesized or qualified on a logic device, and an ESP32-C5 CI
build does not validate sidecar timing or a physical video signal.
