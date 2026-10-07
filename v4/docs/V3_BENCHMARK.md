# Operator V3 benchmark and V4 acceptance

## Current result

Louis reports V3-level picture and flight range on the HR50 plus staged-gain
candidate `7D29A002EFC6D0D413E64530C61FB0B3D3FBE301B6AFB28E4E4B049BD60C12BB`.
This is operator-rated acceptance, not measured dB or sample-continuity proof.
Golden and Unwrap75 do not inherit that acceptance. The chronological notes
below preserve earlier unsuccessful and pending states as historical evidence.

## Reference-detector preparation (historical, before hardware acceptance)

The operator asked to keep working on V4 after adaptive Unwrap75 failed.
V4 now contains isolated, instruction/LUT-identical reference copies of
the benchmark donor's Phase8 HR and OG Golden Phase5 assembly, with only
provenance comments added. `g` cycles `ref_demod` 0/1/2 with an acknowledged
reboot: Unwrap75, Phase8 HR50, Golden Phase5/50. Absent/invalid key preserves
Unwrap75. Both references retain the original DAC transfer, so this compares
detector-plus-transfer configurations, not phase precision alone.

The raw Q4/I4 RX40 ring, TX40 DAC pins, Zero-EOF descriptors, Direct Gain,
lane policy, analog bandwidth and per-gain hardware DC remain V4. Reference
selection is reused on menu exit and live restart; it never resets DSP at a
physical ring boundary. Span75 semantic sync/standard and voltage estimates
return unavailable rather than misinterpreting stride-2 output as stride-3.
Auto AFC actuation, channel auto-search, native mask, sync flywheel, idle
raster and span75 LUT probes/writers are refused/disabled. Manual tuning
and direct menu raster remain supported; reference picture comparison does
not qualify menu/restart or range behaviour. Existing options are not erased.

Source-driven host regression checks all 65,536 raw endpoint pairs for each
program, duplicated six-bit DAC output, two-bundle cadence, pinned normalized
assembly hashes and span75 exclusion hooks. The model's optional eight-slot
ROM wrap is enabled only for the Phase8 HR reference. These checks passed,
and the local IDF6.0.2 build with CMake integration verification passed.
No physical benefit or FIFO continuity is established yet. Test HR50 first
on R3 with adaptive lanes, before trying Golden under the same RF settings.
Final local reference-mode application SHA256:
`D6F87DE798E22983724682E0AC1492BBF076C2CB7835897F50B8C9F6CDA5EA34`.
Build and integrated verification passed; not yet flashed. POSIX-only gate
and PHY-lab regressions remain skipped on the Windows host.

Subsequently flashed at Louis's explicit request with verified boot/partition/
application write hashes, preserving NVS. `g` acknowledged mode 1 and rebooted;
actual console verified `phase8_hr50`, two bundles, unique20M at TX40,
donor transfer, R3/5732 zero offset, Direct V5 ACTIVE and protected lanes.
Span75 consumers were disabled and setup transport fault counters were zero.
Low-rate no-meter picture logging started; no operator result yet. Golden
has not been selected or hardware-qualified in this V4 integration.

HR50 operator result, 2026-10-07: stationary image **on par with V3**, but
Louis performed a flight and reports **awful range**. Picture comparison is
positive operator evidence for this detector/transfer configuration; range
acceptance failed. No takeoff/landing or exact VTX-on/off markers were supplied,
so the entire log must not be described as a marked flight interval.
`.dev/benchmark_v4_20261007_154223.log` closed normally at about 110 seconds,
with the report marker at +107 seconds. It contains 51 ordinary snapshots,
14 at G20, several clipped/near-origin captures and zero transport faults in
their reported fields. From DG3 +4.109 to +105.375 seconds, requested writes
increase 9213 -> 103857 (~935/s), verified transitions 3830 -> 6794, lane
requests 177 -> 474 and fold drops 43 -> 136. These counters are distinct:
requested writes are not individually verified applied hardware writes, and
controller lane requests are not the physical routing counter. Strong DC
offsets also remain in later coarse/G83 snapshots. A freshly requested gain
and an older completed IQ window can differ; do not treat one G20 rail window
as proof that physically settled G20 saturates. No dB loss is inferred.

Keep HR50 as the current picture baseline while isolating V4 gain/RF/DC
behaviour. Do not expect a switch to Golden alone to repair demonstrated
control instability. Further settings changes need landed/quad-off confirmation,
and a stationary controlled-gain comparison precedes another flight.

This comparison extends C5VRX by Twotoz and the C5VRX contributors:
https://github.com/Twotoz/C5VRX and https://twotoz.github.io/C5VRX/.
The official website provides the Discord invite. Existing GPL-3.0-only
and source notices apply. These are receiver experiments, not an independent
demodulator invention or an endorsed fork release.

## Benchmark recorded 2026-10-07

Louis reports substantially better goggle video from zerowidth's published
standalone `pr-3` C5VRX-3 application at
`69dfd683f534ec1663ecb2cf7645dea38ca05348`. Application SHA256:
`49d5a6ab648fb7d00c7e457f51d76f78029ec2831d5a3690f615675f6fd6ad24`.
Local binaries, matching bootloader/partitions and checksums are preserved
under `.dev/zerowidth-pr3/`. This is not the separate C5-to-P4 firmware.
It does not supply wider IQ or P4 denoising to the standalone C5 receiver.

The compiled Phase8 HR path comes from the donor's
`main/fm_phase8_hr_live.bsasm`: raw Q4/I4 RX40, CPU control only, cyclic raw
ring, two TX bundles per pair, 50-ns endpoints, unique DAC20 emitted as
`[D,D]` at physical TX40. Optional NVS-selected HC must be checked before
asserting the exact runtime decoder. Do not label this image Golden.

The preceding local V4 image uses Q4/I4 RX40, a raw cyclic ring, TX-only
Unwrap75/static STD150, three bundles, unique DAC13.333 emitted `[D,D,D]`
at physical TX40. It had fixed fine lanes, Direct Gain V5 active, AFC off,
sync flywheel off and idle raster off. Its flight still showed clipping,
G20 excursions and scrolling after the scoped settling guard fix.
See POST_DROP_SETTLING.md for that failed physical acceptance.

Receiver tuning changed from R8/5917 to R3/5732 for the V3 image; the old
standalone application rejects frequencies above 5885 MHz. Thus the reported
improvement is operator evidence for the whole configuration, not a controlled
same-channel proof against Unwrap75 or fixed fine alone. Gain, analog bandwidth,
DC correction and diagnostics also differ. A later no-carrier USB snapshot
must not be treated as telemetry for the earlier good-picture instant.

## First V4 comparison: existing protected adaptive lanes

Use the existing `Z` comparison; do not change the production fixed-fine
default merely to construct this test. Confirm `lanes=protected_v5` after
the required reboot. Starting from fixed fine, two separately acknowledged
cycles select fixed ultrafine, then protected V5. Never blindly send `ZZ`:
each command reboots and the second can be lost. Under native AGC the tap
remains coarse, so first confirm active Direct Gain ownership.

Hold these settings constant against the preceding V4 flight:

- Unwrap75 STATIC and STD150; no detector or DAC transfer change.
- Existing fixed analog bandwidth/DC policy, sampling policy and settling fix.
- Active Direct Gain V5, AFC off, radius boost off, edge gear off.
- Sync flywheel and idle raster off.

Tune and verify R3/5732 before powering the quad for comparison. Start with
the SNR meter disabled: it is an additional diagnostic consumer absent in V3.
Use low-rate ordinary status snapshots, not sample-paced USB or a new PHY lab.
Keep camera, VTX channel/power, antennas, goggle input, location and quad
position unchanged. Compare stationary close and weaker-signal positions
before asking for a flight. Quad must be off before a flash; preserve NVS,
capture the applied settings, and keep the verified V3 full-image rollback.

`tools/test_integration.c:test_benchmark_lane_escape` checks that the same
G66 fine-lane overload sends fixed fine to G20, whereas adaptive first escapes
to coarse without a gain write, refuses pre-switch evidence, holds a healthy
fresh coarse window, and still cuts genuine persistent coarse overload.
This is a host control regression, not evidence of seamless routing or
picture improvement. GPIO mapping writes remain sequential and untagged.

Local validation on 2026-10-07 passed: IDF 6.0.2 XIAO build, 19 Windows
C regressions, exhaustive Unwrap75 oracle and source-driven DSP tests.
POSIX gate/PHY-lab cases were skipped on Windows. Firmware SHA256 remains
`F6226421E3E1EAD5676DF5FE980C03155D26FB862043DF9865685719AEF73A8B`;
adaptive selection is an existing NVS mode, not a new firmware implementation.
Louis confirmed quad off/USB connected and this candidate was then flashed
with matching bootloader/partitions and verified write hashes. Benchmark R3
NVS/PHY was backed up; NVS was not erased. Separate acknowledged `Z` cycles
selected protected V5; R3/5732, zero offset, active Direct Gain, AFC off,
STATIC/STD150, flywheel off, idle off and radius boost off were verified.
No SNR meter was enabled. Transport fault counters were zero in the setup
snapshot. Operator picture acceptance remains pending; the quad-off false
carrier indications and DC/gain movement are not reception-quality evidence.

## Acceptance and subsequent decision

First adaptive V4 comparison, 2026-10-07: Louis reports **significantly worse**
than V3. Physical picture acceptance failed. Capture
`.dev/benchmark_v4_20261007_152633.log` ran approximately 111 seconds,
with the operator report marked at +108 seconds; no precise VTX power-on
marker was supplied. Logger stopped normally and released COM33, without
enabling SNR readings. Later snapshots retained G83/coarse, appreciable DC
and repeated lane/gain-controller activity; transport fault fields remained
zero in the snapshots. Controller requested-write counters are not proof
of individually verified hardware writes. This result rejects adaptive lanes
alone as a sufficient fix; it does not isolate Unwrap75 from RF/DC/control
differences or establish a range/dB regression.

Require operator-rated picture comparable to the V3 benchmark at matched
positions, no new scrolling/blackouts, healthy-envelope gain stability,
overload recovery, and no reported DMA/BitScrambler faults. Record firmware
hashes, settings, ratings and timestamps. Flight/range remain unproven until
the corresponding controlled hardware test passes.

If adaptive V4 remains worse, extend the original C5VRX Phase8 HR TX-only
50-ns route within V4 as a separate comparison. Do not simply swap assembly:
the stride-3 sync estimator, CVBS mapping, menu/restart ownership, optional
mask/flywheel and sample-rate assumptions need an explicit compatibility audit.
Keep unsupported span75-dependent features disabled in that experiment.
Neither a successful build nor the exhaustive Unwrap75 oracle establishes
that Unwrap75 should be retained if physical picture quality is inferior.

Twotoz also suggested OG Golden Phase5 on 2026-10-07. Include that as a
third detector candidate, not as another name for the current Phase8 HR
benchmark. Its original `main/fm.bsasm` uses full Q4/I4 RX40, uniform 32-state
phase quantization, 50-ns endpoint differences, two TX bundles, unique DAC20
and `[D,D]` at TX40. `docs/pr-derived-findings.md`, section 4, records earlier
hardware-quality evidence in its favour, as well as its endpoint winding
limitation. Five-bit phase is an internal quantization, not five-bit RF IQ
or a five-bit DAC. Coarser phase can change noise/detail tradeoffs; the earlier
hardware result does not prove superiority on this board today. Preserve
all 32 circular phase states and the original calibrated DAC mapping for
the initial reference test. Only compare detectors under matched lane/gain
and RF conditions, and audit the same span75-dependent V4 consumers first.

## Fixed-gain follow-up, 2026-10-07

Fresh quad-on confirmation preceded capture
`.dev/benchmark_v4_20261007_155514.log`. HR50, protected adaptive lane policy,
R3 and AFC-off were retained. AUTO baseline again alternated near-origin
and clipped snapshots, with rapid requested gain writes and no increase in
the sampled verified-write counter. These asynchronous fields must not be
interpreted as one settled IQ window at the displayed gain.

Manual mode ACK held G20 at +21.985 seconds; seven two-step increases ACK
G34 at +27.985 seconds. Louis reported **very noisy picture, but better
range**, then unplugged because of overheating. The affected device and
exact unplug time were not specified. No controlled attenuation, matched
distance measurement or flight timing was recorded, so no dB/range number
or physical acceptance is established. Manual mode also freezes V5 lane
updates and per-gain DC service: this implicates the automatic receive-control
bundle, but does not isolate gain, lane switching or DC correction individually.

Automatic mode was restored and the logger closed normally at +127.610
seconds. Stop RF testing until the affected hardware has cooled. Inspect
the automatic control path before another detector change or flight.

### Reference-mode overload recovery candidate

Operator authorized implementation after code inspection. This extends
C5VRX by Twotoz and contributors' Direct Gain staged emergency-drop route
(`main/direct_gain_v3.c` in donor `69dfd683f534ec1663ecb2cf7645dea38ca05348`;
[canonical source](https://github.com/Twotoz/C5VRX),
[official website and Discord invite](https://twotoz.github.io/C5VRX/)).
Only V4 HR50/Golden reference modes opt into the new control comparison.
The frozen root and Unwrap75 severe-overload policy remain unchanged.

Reference overload now uses the existing physical BB-first/RF-second
emergency reduction rather than forcing G20 on severe coarse/fixed-lane
clipping. An overload-recovery flag then makes near-origin IQ request one
physical upward move after the settling guard, not immediate table maximum.
A healthy held window clears the flag. Persistent clipping can still reach
G20; continued real signal loss can still climb to maximum. Finer-lane
fold escape and manual/native ownership are unchanged. No new gain table,
NVS setting, DC calibration, waveform or DMA processing is introduced.

`test_reference_overload_recovery` failed on the old hard-G20 policy and
passed after the change. It checks staged reduction, stale-quiet refusal,
bounded upward recovery, immediate overload protection after an upward
write, convergence in an explicitly synthetic gain-dependent plant,
persistent-overload floor and eventual maximum listening on real loss.
Existing Unwrap75/fixed-lane regressions pass unchanged. The synthetic
plant is not measured RF and does not prove live convergence, less noise,
FIFO continuity or greater range. Reason-coded live decisions and a short,
cooled, airflow-assisted operator comparison remain necessary.

Local validation: focused integration regression passed and the XIAO C5
ESP-IDF 6.0.2 build (including configuration-time full verification) passed
in 139.30 seconds. Windows POSIX-only cases remain skipped. Candidate app
SHA256 is `7D29A002EFC6D0D413E64530C61FB0B3D3FBE301B6AFB28E4E4B049BD60C12BB`;
RAM 105944 bytes, flash 1143296 bytes. Not flashed or physically accepted.

Subsequently flashed on operator quad-off/USB confirmation, with matching
bootloader/partitions and all write hashes verified; NVS was not erased.
Actual reboot confirmed HR50, R3/5732, active Direct Gain, protected lanes,
AFC off and reference-feature exclusions. Setup transport counters were
zero. Quad-off gain motion, DC searches and a G83 hold abort remain; the
flash/boot is not evidence of solved hunting or improved reception.

### Operator flight acceptance

After this flash, Louis reported: "Image looks as good as V3, range is also
as good as V3" and "I did a flight test and this is great." This is positive
operator-rated picture and flight/range acceptance of candidate `7D29A002...`
with HR50 and reference-mode staged gain recovery. It is not a measured dB
result, a sample-continuity proof or a controlled single-variable causal test.
The staged logger ended at +40.890 seconds without takeoff/landing markers;
do not assign the whole capture to that flight. No new Unwrap75 acceptance
is implied. Preserve this exact candidate and settings as the working V4
reference benchmark before any further experiment.
