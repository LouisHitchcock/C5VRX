# Phase5-360 compact delta model

`tools/model_phase5_360_delta.py` is an executable reference for the **ideal**
three-sample Phase5 trajectory. It does not run in the live BitScrambler.

For previous, middle and current Phase5 bins `P`, `M`, `C`, define
`a = wrap32(M-P)` and `g = wrap32(C-P)`. Since modular subtraction gives
`C-M = g-a (mod 32)`, the exact adjacent travel is

```text
travel = a + wrap32(g-a)
address = ((a & 31) << 5) | (g & 31)
LUT[address] = median Golden code for g, if travel == wrap32(g)
             = clamp(20 + 2*travel, 0, 63), otherwise
```

All 32,768 Phase5 triples agree with the direct reference
`wrap32(M-P)+wrap32(C-M)`. Every one of the 1024 LUT addresses is reachable.
For example, `P=0,C=20` needs distinct results for `M=10` (+20 bins) and
`M=20` (-12 bins), so the endpoint difference alone is insufficient.

Golden's live LUT is calibrated per endpoint pair and includes squelch. The
compact address cannot retain absolute `P`, so the model takes the median of
Golden's 32 endpoint-pair codes for each `g` when no winding occurs. The
script counts the remaining DAC differences and their maximum magnitude.
In the current LUT, 4,912 of 24,576 no-winding triples differ from Golden,
with a maximum difference of two DAC codes.
For winding events it uses a candidate P20/G2 curve, not a measured video
transfer. Video quality and RF behavior still need measurement.

## Remaining live implementation gate

The 1024-word trajectory table replaces the endpoint-pair table only if the
hardware can form both five-bit differences and present them to a LUT lookup
before the 50 ns DAC write. The raw Q4/I4 bytes still need Phase5 decoding.
PR #90 proves one hardware difference with pre-encoded operands; it does not
prove two differences plus raw-IQ decoding and this LUT lookup in the two
available bundles. A 1024-word table also leaves no *address* capacity for a
separate phase decoder, even though unused bits within each word may help if
the lookup schedule can be solved.

The exact 32-bin model still needs a raw-IQ hardware schedule. A separate,
approximate live candidate now uses both raw middle-sample sign bits and a
learned two-stage LUT; see [Q2 adjacent-50](phase5-360-q2-live.md). That
candidate does not claim exact agreement with this model for all triples.
