# Staged Direct Gain recovery on the span50/HC50 test state

Status: **operator-accepted on the board, 2026-10-07.** Not a measured dB
result or a controlled single-variable range test.

## Change

Base: Twotoz's PR #183 test state (`59f214bd0d3841983a1f64107328e025e4e33522`,
every v4 program HC50, verify bypassed, sync flywheel off), by Twotoz and the
C5VRX contributors ([C5VRX](https://github.com/Twotoz/C5VRX),
[website and Discord invite](https://twotoz.github.io/C5VRX/)). One change on
top: the staged overload recovery from `784bbe6` (PR #182), which extends
C5VRX's Direct Gain staged emergency-drop route (`main/direct_gain_v3.c` in
donor `69dfd683f534ec1663ecb2cf7645dea38ca05348`).

`c5vrx4_staged_gain_recovery()` (always true here) makes Direct Gain V5:

- reduce BB first, then RF, through physical tuples on severe coarse or
  fixed-lane clipping, instead of forcing G20;
- refuse near-origin IQ as carrier loss until the settle guard has elapsed;
- after an overload, step upward one physical tuple per remeasurement
  instead of requesting table maximum; a healthy HOLD clears the flag.

Real loss still reaches maximum; persistent overload still reaches G20.
Manual/native ownership, lanes, AFC, DC and the HC50 programs are unchanged.

## Evidence

- Host: `tools/test_integration.c` staged-recovery cases pass with the hook
  on; the original G20-floor cases pass with it off.
- Board: PlatformIO ESP-IDF 6.0.2 build, app SHA256 prefix `9dfaa8ea60e9c27e`,
  flashed with verified hashes, NVS preserved, R3/5732 lock after boot.
- Operator (Louis), same day: plain PR #183 gave a good picture but poor
  range; with this change "really solid, good range, good video (although I
  feel this could still be improved slightly)". Same R3 channel; lane/AFC
  NVS state as stored on the board, not re-recorded for this comparison.
