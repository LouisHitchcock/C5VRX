# C5VRX-4 integrated alpha

One isolated ESP32-C5 receiver project, assembled from the C5VRX-3 range/control
work and the three-bundle Phase8 Unwrap75 experiments. It extends **C5VRX by
Twotoz and the C5VRX contributors** and their receiver research.
Source: https://github.com/Twotoz/C5VRX · official website and Discord invite:
https://twotoz.github.io/C5VRX/. Existing author notices and GPL-3.0-only apply.

This directory contains its own runtime `main/`, configuration, partition table,
tests and evidence. It does not compile the repository's C5VRX-3 `main/`.
The active V4 project lives under `v4/`; merge requires the
operator's explicit approval. Building and publishing a PR alpha is independent
of merging. The V4 alpha workflow and flasher build this project independently.

## Receiver contract

### ELRS VRx backpack

An ESP32-S3 running the [ExpressLRS Backpack](https://github.com/ExpressLRS/Backpack)
HDZero VRx target sends the radio's VTX channel over one wire to D10
(GPIO10; IQ bit I[9] moved to the unconnected GPIO2 pad); the C5 retunes
like a button channel change. ELRS L band is refused
(different frequency plan). Menu SETUP `ELRS BACKPACK`, serial `U`. See
[docs/ELRS_BACKPACK.md](docs/ELRS_BACKPACK.md).

### Current code layout

The standalone project is organized as follows:

| Path | Contents |
| --- | --- |
| `main/` | Receiver tasks, RF control and IDF component |
| `firmware/` | V4 pipeline, CVBS and lane implementation |
| `firmware/include/` | V4 headers and generated tables |
| `firmware/programs/` | Current generated BitScrambler programs |
| `tools/` | Generators, host regressions and flash utilities |
| `docs/` | Design, provenance, measurements and acceptance notes |

The runtime is specialized for C5VRX-4. `main/video.c` only starts the receiver;
the previous 8,765-line C5VRX-3/V4 implementation is split by ownership:

| Modules | Responsibility |
| --- | --- |
| `video_transport.c` | Raw DMA ring, PARLIO and current three-bundle programs |
| `video_gain.c` | Direct Gain V5 observer, sentinel and gain/BW writes |
| `video_control.c`, `video_settings.c` | Buttons, scanner, AFC, NVS and native ownership |
| `video_menu.c`, `video_idle.c` | Standalone menu/idle TX and optional flywheel |
| `video_measure.c`, `video_calibration.c` | Completed snapshots, CVBS, bandwidth/witness calibration |
| `video_dco.c`, `video_drift.c`, `video_sampling.c` | Per-gain DC correction, drift tracking and verified sampling checks |
| `video_recorder.c` | Bounded flight history in NVS |
| `video_console.c`, `video_diagnostics.c`, `video_lab.c` | USB commands, fault evidence and retained PHY A/B labs |

`video_internal.h` is the private task/transport contract; application code uses
`video.h`. DMA/BitScrambler still own sample pacing. The scanner retains its IQ
classification, semantic-video confidence and centred-RF tie-break.

The isolated target no longer builds C5VRX-3 boot probes, Golden/Phase5/FM4/HC
programs, Direct Gain V1/V2, ARC/Fusion/range gain controllers or their unused
generators. Their original implementations remain in the root C5VRX-3 tree and
Git history. The unused 6-ms Fusion task and 4,096-byte stack are gone.

Only Direct Gain V5 and opt-in native AGC remain as gain owners. Old profile
bytes migrate to V5 without changing the 14-byte v3/v4 settings layout. `N` and
`X` toggle native AGC with a reboot; `D` resets V5 controls. Removed research keys:
`g`, `F`, `G`, `U`, `S`, `R`, `I`, `Y`, `i`, `z`, and `1` through `6`.
The lab-row output omits stale Fusion/FFT fields that are no longer measured.
Existing boot-option opt-outs, automatic calibrations and PHY write guards stay.

Host verification covers the current algorithms and generated programs, including
the exhaustive 524,386,048-trajectory unwrap oracle. Removed-controller tests no
longer run in this target; scanner classification tests remain. Firmware builds
and host tests do not replace a board test of menu/idle handoffs, retuning, native
AGC, calibration and live video after the refactor.

### Live pipeline

- MODEM_DIAG packed Q4/I4, positive-edge PARLIO RX at 40 MS/s, raw cyclic ring.
- TX-only Phase8 endpoint decode plus middle-sample quadrant winding; exactly
  three bundles per three input/output bytes. Unique CVBS 13.333 MS/s,
  continuous `[D,D,D]` at a 40 MHz / 40 MB/s physical DAC transport.
- Six original DAC GPIOs and resistor network; no CPU sample-paced output.
- Direct Gain V5 by default: first-window physical correction, 200-us observer,
  descriptor dedupe, table-maximum listening, measured noise lane cap and
  anti-hunt damping: moderate excursions must persist for 20 ms before a
  write; severe clipping still cuts gain immediately. Channel fades, poor phase coherence and prior clipping
  never permanently blacklist gain tuples; legacy NVS bans are ignored while
  measured gain ratios remain available. A healthy envelope stays write-free
  even with poor phase, while real overload still drops gain immediately.
  This prevents firmware-induced loss of available gain; indoor multipath and
  any RF/dB improvement still require a board A/B test with the moving quad.
  Native AGC remains a separate opt-in gain owner.
- Fixed fine IQ lanes by default: ADC bits {9,7,6,5} on I and Q (step 32
  codes, signed window +-256), selected before PARLIO RX starts and never
  switched at runtime, for Direct V5 (native AGC uses the coarse set
  {9,8,7,6} for its whole session). Analog gain does all amplitude
  tracking; its 13..32 P50 band is ~3.6-5.7 fine cells (~115-180 codes). With
  the measured ~35-code receiver noise at maximum gain, one fine step is close
  to one noise sigma: finer lanes add no phase information there but fold
  sooner, and coarse loses ~0.7 dB at the edge (host simulation, not a range
  measurement). Fixed ultrafine and protected adaptive V5 lanes remain Z
  comparisons.
- Severe clipping (>=50%, P95>=95) on the coarse or a fixed lane sends active
  Direct Gain to G20, its existing controller floor: a fixed lane has no
  escape lane. Moderate overload uses staged BB-first cuts. In the adaptive
  comparison finer-lane saturation first escapes to coarse. No native/manual
  gain writes.
- Fixed nominal loaded CVBS transfer STD150 (default): 0.310-V blanking +
  0.150 V/MHz, nearest DAC code with saturation. Assumed nominal sync/white
  targets are 0.010/1.010 V: 0.300 V sync depth and 1.000 V sync-to-white
  under one 75-ohm load. Replaces HR100's undersized output; full amplitude
  leaves only small rail margin, not broad CFO tolerance. CVBS150 and legacy
  remain M comparisons. See docs/CVBS_OUTPUT.md for sources and physical limits.
- Slow sync supervision uses the same stride-3 Phase8/winding transfer estimate,
  instead of the old Phase5 shadow. Snapshot alignment remains approximate.
- AFC V2 measures burst-confirmed sync and burst-free porch, with both endpoints
  valid, bounded/stable 16-window evidence, context/settle/copy refusal and a
  maximum of four 250-kHz acquisition steps. AFC video TRACK is sticky and
  writes nothing while valid; it is independent of gain HOLD. **AUTO defaults
  off**: transmitter reference/sign still need physical calibration.
- PR154 PHY ownership restoration, serialized generation-checked gain writes,
  stale-overload mailbox rejection, reversible pinned PHY/BW/11p/native-hold labs.
- Tuning reaches 5945 MHz (R8, E6..E8) through `phy_set_freq` from the 5885 MHz
  centre, as zerowidth decoded R8; the old 5885 MHz ceiling is gone.
- Digital DC recentring and the CVBS level servo retain their requested boot
  options, but live LUT updates are blocked by the current implementation;
  the level task is not started. The historical level/recentring notes describe
  the intended paths and pending hardware gates, not active corrections.
- Per-gain hardware DC correction uses quiet-gated, context-tagged calibration
  codes, stops below the boot's measurable noise floor, and re-holds each gain's
  pair immediately after a gain write/PHY restore. Native ownership, labs,
  settling, broken-IQ refusal and per-gain bans remain enforced. Drift tracking
  makes bounded fine-code nudges while receiving; Ctrl-T provides a RAM A/B.
  Exact-frequency vendor RX recalibration is a manual `~` lab only.
- The default-on sampling-phase autocheck latches only after a measured-good
  result, retries refused/unsettled scans and rechecks after retunes. Native
  AGC is supported; `&` opts out.
- Default-on fixed analog bandwidth replaces the BW20/BW40 gear, which only
  moved the digital filter. It builds on [ESPARGOS esp-sdr](https://github.com/ESPARGOS/esp-sdr)'s C5 `BANDWIDTH`
  control (absolute RX0 capacitor code in BBTOP 0x67 regs 6/7, noise-FFT
  width curves, commit `ac627b0b`) and on [zerowidth/C5VRX PR #3](https://github.com/zerowidth/C5VRX/pull/3)'s measured noise reduction from a
  narrower filter. Without a carrier at maximum gain the receiver-noise width
  is measured on this chip for the calibrated bytes and codes 0..60, matched
  against both esp-sdr curves, and the narrowest code still >=24 MHz (the
  widest if none reaches it) is stored (NVS `bw_code`, `bw_width`) and
  re-applied at boot and on every retune. C5VRX's own `WIFI_BW20` test lost
  detail and chroma, hence the 24 MHz floor. Runs automatically once (VTX
  off); `=` repeats it, NVS `c5vrx4/fixed_bw=0` (`^`) restores the gear.
- Native AGC acquisition mask (native mode, default on once calibrated): the
  C5 packet AGC re-acquires every ~25-50 us with a ~2-3 us saturated/starved
  gain walk. A MODEM_DIAG AGC state bit, found by an on-board witness
  calibration (`*`, automatic at the first native carrier), rides on PARLIO
  data bit 0 (Q LSB) and the STATIC program holds the last DAC value through
  every walk, reseeding phase so the next clean span is exact. Native keeps
  its sub-line reaction; `|` opts out. See docs/NATIVE_AGC_MASK.md.
- No-carrier idle raster (default on, for HDZero/TP2825 goggles): after 2 s
  without any carrier or sync, the standalone BT.470 raster sends clean black
  video in the live/last stable standard instead of demodulated noise, so the
  goggles neither show green nor switch PAL/NTSC. The first carrier or sync
  returns to live video. The last stable standard is kept in NVS. `_` opts
  out. See docs/HDZERO.md.
- Fade-gated sync flywheel and line repair follow PR #180's defaults and
  preserve its bounded CPU work, source/read-frontier checks, vertical-interval
  refusal and limited consecutive line repair. SETUP/`w` provide opt-outs.
  Board acceptance of the combined runtime remains pending.
- V5 strong-signal radius boost (opt-in, `y`): on a strong, tight, rail-free
  carrier the healthy P50 band moves from 13..32 to 30..46 (IQ ring ~200
  instead of ~150 codes), so the 4-bit phase is finer. The first rail code,
  P95 or level jump returns to the normal band at once. `y` enables it. See
  docs/RADIUS_BOOST.md.
- Pre-demodulation labs (`!`, `@`, `#`, `$`) measure sampling phase, DC centring
  and filter width. See docs/PREDEMOD_LAB.md.
- Main's analog-video scanner confidence and centred-RF tie-break are retained.
  They identify candidate channels; a confident scan is not range proof.

## Operator controls

| Key | Action |
|---|---|
| `T` | Detector, mapping, lane geometry and gain-owner status |
| `J` | AFC state plus eight bounded sync/IQ snapshots; no actuator |
| `u` | Toggle the requested level-regulation option, reboot; live LUT writes remain blocked |
| `M` | Cycle STD150 (default) / CVBS150 / previous full-span transfer, reboot |
| `Z` | Cycle fixed fine (default) / fixed ultrafine / protected adaptive V5 lanes, reboot |
| `h` | STATIC / bounded HISTORY phase decode, reboot |
| `N` | Direct Gain / native AGC, reboot |
| `H`, `{}`, `[]`, `W`, `B`, `A`, `:`, `L` | PR154 shared PHY diagnostics/labs |
| `(`, `)` | Explicit native-only BB hold / 100-cycle reversible lab |
| `!` | Pre-demod status: lane policy, glitch ppm, DC centre, DC-cal point, DCO words, filter caps |
| `@` | Sampling-phase scan: RX clock slips, mid-transition glitch ppm, settles on a clean position |
| `#` | Reversible RX DCO (PBUS DC DAC) closed-loop correction A/B, pinned PHY only |
| `$` | Reversible RX filter-capacitor sweep (0x67 regs 6..13), pinned PHY only |
| `%` | Toggle the requested digital DC-recentring option, reboot; live LUT writes remain blocked |
| `&` | Toggle default-on first-lock sampling-phase check, reboot |
| `=` | Measure the receiver-noise width per RX filter code and store the fixed BW (VTX off) |
| `^` | Toggle default-on fixed analog BW (off restores the V5 BW gear), reboot |
| `*` | Native AGC witness calibration (VTX on, native mode): find the acquisition state bit, store, reboot |
| `\|` | Toggle the native AGC acquisition mask, reboot |
| `_` | Toggle the no-carrier idle raster (HDZero), reboot |
| `y` | Toggle the V5 strong-signal radius boost, reboot |
| `w` | Toggle the sync flywheel (default off: on hardware the 100 µs wake starved IDLE, the USB console and the menu; when on, missing/noisy H and V sync are rebuilt), reboot |
| `'` | sigRSSI mode A/B (live signal RSSI, exact AGC-word restore) |
| `"` | `phy_param_track_tot` temperature-tracking A/B |
| `/` | Digital RX filter mode 0..15 and other ADC rate A/B (is a digital filter ahead of the tap?), exact restore |
| `;` | BW20 channel setup with the analog filter wide open (codes 0/8/16) vs BW40: width, noise bandwidth, clicks; 1 s per stage |

The lane policy uses the NVS key `c5vrx4/lane_mode` (0 fixed fine, 1 fixed
ultrafine, 2 protected V5); the older `force_ultra_v2` and PR146 `force_ultra`
comparison keys are ignored, so an earlier test setting cannot override fixed fine.
Other C5VRX-4 settings remain in `c5vrx4`, separate from C5VRX-3 `c5vrx`.
STATIC remains the default; HISTORY, fixed ultrafine and adaptive lanes are comparisons.
Normal probes are disabled at boot. Lab writes require the pinned PHY archive;
unknown libraries keep observation and refuse undocumented writes.

## Build and verification

```sh
cd v4
python3 tools/verify.py
. "$IDF_PATH/export.sh"  # ESP-IDF v6.0.2
idf.py -DIDF_TARGET=esp32c5 build
```

IDF configuration invokes `verify.py`, so the existing alpha workflow executes
this directory's C regressions, AFC cases, Phase8/routing tests and exhaustive
524,386,048-trajectory oracle before cross-compiling. Host Python 3 and GCC are
required; this verification does not need NumPy, network access or hardware.
Generated tables must match checked-in artifacts. No binaries are committed.
The supervisory task stack is 16 KiB to accommodate the bounded snapshot
analyzer; startup refuses allocation failure. The separate adaptive 5/20-ms level worker uses another 16-KiB stack and an 8190-byte heap
snapshot; J capture also has a 16-KiB stack. Heap/stack margin under menu and
concurrent capture still needs hardware observation; T reports level-worker margins.

### Flashing a running board (no BOOT button)

esptool's USB reset cannot reach the ROM loader while C5VRX-4 runs: the
RTS/DTR reset leaves the forced modem clocks and the always-on dump writer
running into the loader, whose USB download loop then stops answering
("Connecting...." then "Write timeout"). Console key `` ` `` makes the
firmware stop the writer and Wi-Fi, request a download boot and restart
cleanly, after which esptool connects without a reset:

```sh
python tools/enter_download.py COM33
esptool --port COM33 --before no-reset --after watchdog-reset write-flash ...
```

`--after watchdog-reset` starts the new image (an RTS hard reset returns a
download-booted C5 to the loader). PlatformIO users can add
`extra_scripts = pre:tools/pio_enter_download.py` and
`upload_flags = --before=no-reset` to do both around every upload. Images
without the key, or a wedged board, still need manual download mode (hold
BOOT, tap RESET). `tools/console_soak.py` measures USB console health (write
timeouts, reply latency, heartbeat gaps).

[docs/INTEGRATION.md](docs/INTEGRATION.md) records PR/issue disposition and acceptance.
[docs/INTEGRATION_SOURCES.json](docs/INTEGRATION_SOURCES.json) pins donor revisions.
[docs/CVBS_OUTPUT.md](docs/CVBS_OUTPUT.md) explains the loaded transfer and scope model.
Earlier research files are donor records; this README defines current defaults.

## Evidence boundary

This is an unmerged test build. Host tests and compiler success do not establish
sample-gapless transport, improved sensitivity/range, PAL/NTSC compliance or
HDZero acceptance. Fixed scaling does not recover phase information lost to
clipping, origin collapse or RF noise. Automatic sync-referenced video level regulation is enabled by default at Leon's
request. It targets 286/300-mV NTSC/PAL sync depth, rejects noisy/stale evidence,
holds through signal loss and latches off on write/transport faults. Three valid
snapshots qualify bounded updates; recovery runs at 5 ms for 100 ms, then
returns to 20 ms. RF settling and lane history are excluded; each DAC entry
slews by at most 32 mV according to the loaded voltage table. Concurrent LUT arbitration and goggle
acceptance remain physical gates. It corrects output gain/offset, not IQ DC.
H/V regeneration/coasting and CPU raw-ring sync repair remain absent.
See [docs/CVBS_LEVEL.md](docs/CVBS_LEVEL.md) for controls, evidence and limits.
