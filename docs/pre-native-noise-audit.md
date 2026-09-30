# Noise history before native AGC (2026-09-30)

Question: did a change merged into main introduce noise before native AGC?
This is a source/history review, not a new hardware comparison.

## Confirmed changes

- Main merge `83780a0` (PR #110, 2026-09-28) enabled both
  `C5VRX_PHASE8_HR_LIVE_TEST` and `C5VRX_DIRECT_GAIN_V3_EXPERIMENT` by default.
  Before the merge, the 6BIT path selected Phase5-360 when enabled. After it,
  it selected the new full-range adjacent Phase8 program. Thus this merge
  changed both the demodulator and firmware gain controller before native AGC.
- Phase8 uses a 256-bin endpoint phase estimate and maps
  `(128 + current_phase - previous_phase) mod 256` to six DAC bits.
  It does not use the old pair LUT's DAC mapping. This is a changed transfer
  function, not evidence that more phase bins inherently cause noise.
  Commit `fe40f45` changed the earlier Phase8 experimental slope from
  `76 + 2*delta` to `128 + delta`, removing early arithmetic wrap.
- The pre-native V3 implementation has a specific weak-signal recovery
  candidate: `direct_gain_v3_observe()` classifies P50 <= 4, origin >= 650 pm
  and coherence < 20 as no carrier, then requests `survival_gain` immediately.
  `start_write()` returns without writing when that gain is already selected.
  Repeated observations satisfying this condition cannot progress to the
  later gain-up logic. This matches the recorded G62, zero-write, 30-second
  collapse in `phase8-range-envelope.md`. It does **not** establish a hard
  G62 ceiling: V3 accepts the gain table's maximum index.
- `phase8-range-envelope.md` already labels Phase8 + V3 as the regression
  relative to its Golden + V3 comparison protocol. That protocol remains
  open; the label alone is not a completed controlled A/B result.

## What native introduction changed

`f46c4ba` introduced native AGC as opt-in; `04a405e` made it the default.
The native path stops forcing firmware gain and leaves the PHY AGC enabled.
The latter default-switch commit does not change the live demodulator LUT.
The main comparison `6d36a5d..fdc32a1` (PR #120) has no changes in
`fm.bsasm`, `fm_phase8_hr_live.bsasm`, `gen_phase8_hr_live.py` or
`sdkconfig.defaults`.

## Scope of the conclusion

PR #110 is a concrete pre-native regression boundary to investigate. The V3
no-carrier branch can explain failure to recover a weak signal while V3 owns
gain. It cannot explain ongoing gain cycling when native AGC owns gain.
Low-amplitude Q4 phase quantization remains a separate mechanism capable of
turning small input changes into large phase steps. Central occupancy alone
is not proof of failure: the earlier native walk test also had a perfect
picture with high central occupancy.

The operator has already reported that Phase5 did not suppress the current
noise. Do not present a Phase5 rollback or the V3 recovery fix as a proven
cure for native grain. Isolating a historical regression requires a matched
comparison with the same gain owner, RF conditions and output mode, changing
only the demodulator/transfer under investigation. No firmware or board
settings were changed for this audit.
