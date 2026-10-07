# No-carrier decisions during gain settling

This extends Direct Gain V5 in C5VRX by Twotoz and the C5VRX contributors
([canonical source](https://github.com/Twotoz/C5VRX),
[official website and Discord invite](https://twotoz.github.io/C5VRX/)).
Baseline revision: `d2b452f`, local `range/on-pr181`, PR181 plus the SNR meter.

## Evidence, 2026-10-07

On Louis's XIAO C5, USB-connected in goggles, R8/5917 MHz, fixed fine lanes,
Direct Gain V5, flywheel and idle raster off, a marked 70.4-second flight
contained 335 bounded SNR captures. 65 captures included ADC rail samples;
52 reported G20. Near the third bad-picture callout, a G66 capture contained
1632/2048 clipped samples, followed by G20, G54, then G20. These are sparse
observations, not a continuous acquisition trace or a measured range change.
USB/meter effects and moving-channel fades were not isolated in that flight.

Source inspection found that `direct_gain_v3_tick` processes its no-carrier
maximum-gain request before the ordinary `DG3_SETTLE` freshness guard. A
host integration reproducer starts with fixed fine G66, applies severe
overload to reach G20, then supplies near-origin IQ 200 us after the applied
write. Baseline requests G83; the regression asserts G20 and fails there.
This proves the control ordering defect for the supplied sequence. The
flight's 5 Hz captures do not prove which decisions caused each gain write.

## Change and scope

Before accepting no-carrier IQ during `DG3_SETTLE`, apply the same physical
settling guard used by ordinary tracking: 300 us unless measured transition
settling exceeds 400 us, then three quarters of that measured duration.
The two paths share a helper to keep the guard identical. No new gain curve,
blacklist, RF control, lane switch or persistent setting is introduced.

After the guard, genuine no-carrier observations still request table maximum.
Saturation after an upward write retains its immediate emergency response.
The regression covers transient quiet IQ at 200/299 us, healthy recovery
without a G83 excursion, genuine loss after settling, a longer measured RF
guard and immediate overload protection during an upward write.

This is only a settling fix. Stable low-level IQ at G20 may still request
maximum after the guard; the change does not establish that the full flight
oscillation or every visual artifact is solved. Physical comparison pending.

Local verification passed on 2026-10-07: the reproducer failed before the
fix and passed afterward; `python tools/verify.py` passed 19 C regressions,
the exhaustive unwrap oracle and source-driven DSP tests. The POSIX memory
mapping tests were skipped by the suite on Windows. The XIAO C5 build passed
with ESP-IDF 6.0.2 (RAM 105944 bytes, flash 1137152 bytes). No CI or physical
acceptance result is claimed for this local change.

Operator-coordinated flash subsequently succeeded on COM33 with verified
flash hashes and watchdog reboot. Application binary SHA256:
`F6226421E3E1EAD5676DF5FE980C03155D26FB862043DF9865685719AEF73A8B`.
Flywheel/idle raster remained off; fresh quad-off calibration completed
G83..G75 and stopped at the measurable floor. This baseline differs from
the earlier flight's calibration state, so an improved baseline or picture
alone cannot isolate the settling guard. Repeat-flight evidence is pending.

## Realtime contract

The CPU change affects only V5 control decisions on completed observations.
Source remains MODEM_DIAG Q4/I4 acquired at 40 MS/s; the raw byte ring and
TX BitScrambler demodulator remain hardware-owned. Three bundles emit one
unique DAC code per 75 ns (13.333 MS/s), repeated at physical TX40 MHz.
No RF sample processing, lookups, boundary state or output timing changes.

## Physical acceptance

Repeat flight completed 2026-10-07, same operator-reported track and logger
cadence, six bad-picture callouts including scrolling and worsening. The
89.719-second marked flight contained 428 captures: 128 at G20 (29.9% of
captures) and 69 with ADC rail samples (16.1%). Baseline was 335 captures,
52 at G20 (15.5%) and 65 with rails (19.4%). These are sampled capture
fractions, not continuous-time occupancy. Duration, RF trajectory and fresh
calibration differ; the paired flights do not establish a causal regression
or improvement due to the guard. They do establish that the live gain swings
and picture faults persisted after this fix.

Near the first new callout, G83 had 1988/2048 rail samples at -0.250 s,
then G20 at -0.063/+0.125 s, then G83 with 1941/2048 rails at +0.312 s.
The controller's `writes` counter increments averaged about 1128/s in the
baseline and 1112/s in the new run, over the available surrounding DG3
snapshots. This counts requested controller transitions, not independently
verified physical RF writes. Transport snapshots near all six new callouts
were zero for TX empty, RX overflow and GDMA errors; no reset/watchdog text
was recorded. No completed DCO_AUTO searches appeared during the marked
flight. Logger ended with periodic meter OFF and serial closed.

Physical picture-quality acceptance therefore failed: the narrow settling
guard is not an established solution for the observed live oscillation.
Do not ship or describe it as a demonstrated range/picture improvement.
Next isolate automatic gain control using a short operator-coordinated
fixed-gain bench A/B, or collect bounded reason-coded controller transitions
to distinguish real no-carrier events from quantizer starvation after G20.
Do not infer the decision reason solely from the 5 Hz meter.

After a verified build and operator-coordinated flash, confirm existing
settings, collect a quad-off baseline, then repeat the same flight track
with the same meter cadence. Compare clipped captures, G20/high-gain
excursions and Louis's picture ratings. Counter readings and a successful
build do not prove sample continuity or an RF sensitivity improvement.
