# Fixed coarse default

Operator request: use coarse by default and keep the lane fixed while
assessing range. Version `4.0.0-exp-coarse`, follow-up to PR #146.

Coarse selects I/Q ADC bits {9,8,7,6}, step64 codes, full signed tap window
[-512,511]. A lower bit count could give coarser quantization but would not
extend this window. This is maximum headroom while retaining I4/Q4.

## Behavior

- Missing/zero `c5vrx4/iq_ultra`: fixed coarse, lane0.
- Serial Z toggles fixed coarse / fixed ultrafine and reboots.
- Both modes keep a constant physical lane in V5, manual and native gain.
- The older `force_ultra` preference is not read. Flashing over PR #146 thus
  selects coarse unless this new key has explicitly been changed.
- RF lane requests resolve to the selected fixed geometry. V5's internal
  lane/cap/maximum agree: coarse has lane_max0; ultrafine retains lane2.
- Analog gain remains owned by the existing selected mode; no maximum-gain
  default or native gain write is introduced. T prints physical lane/policy.
- Phase8 endpoint/trajectory decoding, LUT, DAC mapping, capture pins and
  three-bundle cadence are unchanged.

## Range interpretation

Coarse increases amplitude headroom relative to ultrafine and avoids the
omitted-bit folding of the latter inside the full ADC window. It does not
increase RF sensitivity by itself. With a small IQ envelope, fewer cells can
increase phase quantization error; with substantial receiver noise, finer
capture may provide little benefit. The previous stock-antenna noise findings
suggest coarse can retain much of the available edge information, but do not
prove it is the best picture/range setting in this setup.

The operator reported fixed ultrafine looked identical nearby; range had not
been tested. Compare coarse and ultrafine at the same weak RF input, manual
gain, channel, bandwidth, history setting and original Wi-Fi antenna. Then
compare automatic gain separately. A V5 comparison alone may change analog
gain to maintain a similar digital IQ radius and conceal geometry differences.

No new tests were added or run for this policy change. Compiler/build fit is
not a hardware range result. The connected board retains its previous firmware
until this new build is flashed.

Build: ESP-IDF v6.0.2 success, app size `0x1188d0`. No hardware comparison yet.
