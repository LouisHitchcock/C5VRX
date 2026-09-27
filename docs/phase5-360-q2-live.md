# Q2 adjacent-50 live experiment

`main/fm_phase5_360.bsasm` is an approximate adjacent-50 discriminator for
the 6-bit DAC at 40 MHz. It uses **two middle IQ sign bits** and the full raw
current IQ byte. Its LUT learns a trajectory token from a 50 ns sample pair;
the second lookup combines that token with the previous endpoint Phase5.
The output is one six-bit CVBS code per 50 ns, duplicated as `[D,D]`.

This design uses the same proven two-bundle transport shape as Trajectory v2:
one raw-to-token lookup, one previous-phase/token-to-DAC lookup, one `read 16`,
and one `write 16` per pair. There is no conditional branch on `BH=O0`.
That earlier experiment compared against the *previous* output register,
which the host model had incorrectly treated as the current bundle. Its
observed static and video stoppage are why this candidate uses a different
pipeline.

The training target is the continuous sum of two adjacent phase steps, with
no second shortest-arc wrap. Ten percent of training samples have large,
same-direction steps to expose the 180-degree endpoint ambiguity. The model
is approximate because the middle sample contributes two sign bits rather
than a full independent Phase5 code. It is **360-aware**, not a proof of
correct 360-degree output for every raw-IQ triple.

## Verification before flashing

- The ESP32-C5 assembler accepts four instructions, including two prime
  bundles and the two-bundle steady loop, with a 1024x16 LUT.
- A source-driven BitScrambler model matches the reference LUT code for 8,191
  consecutive raw-IQ pairs, byte for byte.
- Independent deterministic clean, weak, and edge scenes beat Phase5 in DAC
  mean absolute error and errors of 16 codes or more. In the edge scene, all
  60 known winding pairs average 0.05 DAC-code error versus 55.98 for Phase5.
- These are simulated scenes, not RF-capture or screen-quality measurements.

The generator is `tools/train_phase5_360_q2.py --write`; the bitstream check
is `tools/test_phase5_360_q2.py`; the independent scene comparison is
`tools/bench_phase5_360_q2.py`. The physical acceptance gate is sustained
live video with `tx_empty=0`, followed by side-by-side image-quality and
edge/static comparison against Phase5 on the same VTX scene.
