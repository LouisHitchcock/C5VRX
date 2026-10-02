# C5VRX-4: long-range receiver research

Status: first experimental firmware implementation. This is not a demonstrated
replacement for C5VRX-3 or a measured range improvement.
The ordinary PR release still builds the root C5VRX-3 application. The separate
**C5VRX-4 Experimental Build** workflow uploads the experimental firmware as an
Actions artifact; it does not publish it as a production release.

Started from main `59a8713b467916f00436642702a4b14e9598c662` on 2026-09-30.
Research imported from PR #122, commit `33a8c0b`, without importing its firmware
changes. The experiment lives at `experiments/c5vrx-4` in the repository root.

## Objective

Maximize usable analog-FPV range while retaining at least Golden Phase5 colour,
detail, sync stability and fade recovery. Phase8 is a comparison baseline,
not a required architecture. The entire sample/DSP/output pipeline may be
redesigned; measured hardware limits still apply.

## Starting documents

- [RESEARCH.md](RESEARCH.md): source review, evidence, rejected assumptions,
  hardware constraints and candidate architectures carried over from PR #122.
- [DESIGN.md](DESIGN.md): project decisions, open questions and implementation
  sequence for this experiment.
- [../../docs/arc-receive-chain.md](../../docs/arc-receive-chain.md): recovered
  PHY ABI, RF/BB/fine gain stages and calibration constraints.
- [../../docs/continuous-iq-findings.md](../../docs/continuous-iq-findings.md):
  live source, SRAM ownership and physically demonstrated capture behaviour.

## Working rule

The operator subsequently requested implementation. The first target uses
the existing board, eight coarse IQ lanes and a three-bundle span-75 detector.
It reuses the root RF/UI/DMA infrastructure through compile-time experiment
hooks; the new program, generator, gate controller and build entry live here.
A separate architecture choice is still open: whether additional DSP hardware
is allowed for the wider I6/Q6/filter/tracking pipeline.

Usable-range improvement means additional controlled RF attenuation at equal
picture quality. Larger IQ amplitude, a register labelled dB, or smoother
close-range video alone does not establish sensitivity improvement.

## Build

From an ESP-IDF v6.0.2 environment, in this directory:

```sh
python generate_pipeline.py
idf.py -DIDF_TARGET=esp32c5 build
```

App output: `build/c5vrx4.bin`. Flashing requires the matching bootloader and
partition table plus app, using this project's `flasher_args.json`.
No automatic flash is performed by the build.

Build record (2026-09-30): ESP-IDF v6.0.2 Docker build completed successfully;
application size `0x115760` bytes, version `4.0.0-exp-span75`. The ordinary
root application was not rebuilt in this implementation session.

## First hardware observation (2026-09-30)

Commit `162c2ab` was flashed on the connected ESP32-C5, including the matching
bootloader and partition table; esptool verified the written hashes. The
operator reported usable video, some noise, and a less clean picture than
C5VRX-3. This is the starting prototype for further development, not evidence
of additional range. No controlled attenuation comparison or instrumentation
of the output waveform has been performed.

## Firmware contract (PR #154 PHY lab update)

- IQ: signed I4/Q4, coarse I[9:6]/Q[9:6] at startup, established finer lanes
  selected by Direct Gain; 40 MS/s, continuous existing 32 KiB DMA ring.
- Detector: Phase6 endpoint difference across 75 ns, separate nominal
  resistor-DAC inversion LUT; three bundles consume and emit three bytes.
- Output: 13.333 MS/s unique codes held `[D,D,D]` at 40 MHz, existing pin order.
- Direct Gain V5 is the default, using the shared calibrated gain table and
  200 us observer. Native AGC is opt-in via `N` / RF menu and reboot.
- Gain-owner selection and receiver settings both use `c5vrx4`; changing them
  does not change the ordinary application's `c5vrx` settings.
- Native-only tracking gate: 1000 us period, 20 us open, acquisition profile127;
  suspended across channel, bandwidth and frequency-offset changes. Under
  Direct Gain the gate creates no timer and performs no AGC register writes.
- `~` compares paced/continuous native tracking only when native owns gain.
  `T` prints `C5VRX4` owner/timing state before the existing diagnostics.
- Menu rendering remains the original raster transport; video output mode is
  fixed to 6BIT@40 for this experiment.

This version does **not** implement wider IQ capture, a complex channel filter,
an FM tracking loop or matched transmitter de-emphasis. It does not repair
individual samples. The DAC table uses nominal resistor values, not measured
board calibration. Wrap at +/-6.667 MHz, video aliasing, hardware FIFO pacing
and colour/detail response remain unverified. Software Phase5-derived sync
diagnostics are not a measurement of this detector's actual DAC waveform.

## Shared analog PHY lab (#150–153, #155)

This isolated target compiles the same `rf.c`, `video.c` and `phy_rx_lab.c` as
C5VRX-3, including policy restoration, serialized RF transactions and generation
invalidation. Direct Gain makes the shared experiments accessible:

| Key | Experiment / observation |
| --- | --- |
| `H` | Gain table and pinned PHY invariant snapshot |
| `}` | Toggle 50 ms register monitoring; `L` records/dumps event markers |
| `{` | Quiet baseline and bounded analog I2C/PHY parameter snapshot |
| `[` / `]` | Next isolated 10-second packet/NF/CCA/BW/channel/spur profile / restore |
| `W` / `B` | Front-end BW comparison / public vendor BW plus retune comparison |
| `A` | Frequency-offset comparison with Direct Gain ownership restored |
| `:` | One-second BASELINE / 11p ENABLED / exact RESTORED Q4 comparisons |

The 11p observation is credited to SushiDude (@Ready4Sushi on X).
See [the shared lab contract](../../docs/analog-lock-phy-lab.md) for exact masks,
restore semantics, binary pinning and native-mode restrictions. The span75
three-bundle detector and `[D,D,D]` transport are retained; shared Phase5 semantic
sync diagnostics do not measure the C5VRX-4 DAC waveform. Q4 input statistics
remain valid as input observations. No range improvement is claimed.

CI runs native-gate isolation and pinned/unpinned lab lifecycle tests before
building this target. Host tests establish software ownership/rollback only;
repeat controlled hardware attenuation and PAL/NTSC comparisons on C5VRX-4.

Port verification for PR #154: both root C5VRX-3 and isolated C5VRX-4 firmware
built successfully with ESP-IDF v6.0.2. The generated C5VRX-4 configuration has
`CONFIG_C5VRX_DIRECT_GAIN_V3_EXPERIMENT=y` and Phase8 HR disabled. All 206 shared
architecture checks, the existing host range/demod suite, pinned/unpinned lab
lifecycle tests and C5VRX-4 gate isolation test passed. Hardware acceptance is
still pending; this record is not an RF sensitivity or video-quality result.
