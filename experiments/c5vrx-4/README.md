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

## Current firmware contract

- IQ: signed I4/Q4, 40 MS/s, continuous existing 32 KiB DMA ring. Direct Gain
  V5 can select the existing finer lane sets, just as in C5VRX-3.
- Detector: Phase6 endpoint difference across 75 ns, separate nominal
  resistor-DAC inversion LUT; three bundles consume and emit three bytes.
- Output: 13.333 MS/s unique codes held `[D,D,D]` at 40 MHz, existing pin order.
- Direct Gain V5 is enabled and is the default automatic gain owner, using the
  same controller, observer, emergency sentinel and lane policy as C5VRX-3.
- Native AGC remains an opt-in per-boot choice via `N` or the RF profile menu.
  Its `native_agc` key is isolated in `c5vrx4`; existing `c5vrx` settings do not
  select native in the experiment. Missing or zero key selects firmware gain.
- Only native mode starts the tracking gate: 1000 us period, 20 us open,
  acquisition profile 127; suspended across channel, bandwidth and offset
  changes. V5 mode allocates no gate timer and performs no gate/profile writes.
- `~` compares paced/continuous native tracking only in native mode; it reports
  ignored in V5 mode. `T` prints `C5VRX4` gain-owner/timing state before the
  existing diagnostics. Video settings use NVS namespace `c5vrx4`.
- Menu rendering remains the original raster transport; video output mode is
  fixed to 6BIT@40 for this experiment.

This version does **not** implement wider IQ capture, a complex channel filter,
an FM tracking loop or matched transmitter de-emphasis. It does not repair
individual samples. The DAC table uses nominal resistor values, not measured
board calibration. Wrap at +/-6.667 MHz, video aliasing, hardware FIFO pacing
and colour/detail response remain unverified. Software Phase5-derived sync
diagnostics are not a measurement of this detector's actual DAC waveform.

## Direct Gain V5 comparison

The first flashed build used paced native AGC with the V5 controller disabled.
Therefore the initial less-clean picture cannot isolate the demodulator from
the gain policy. Version `4.0.0-exp-span75-v5` enables the current main gain
controller and defaults to firmware ownership. The span75 program and DAC LUT
are unchanged. Compare with C5VRX-3 using the same RF profile, bandwidth and
lane policy. This update has not yet been evaluated on hardware; it does not
establish that native AGC caused all the observed grain.

When rebuilding an existing experiment build directory, set
`CONFIG_C5VRX_DIRECT_GAIN_V3_EXPERIMENT=y` in `idf.py menuconfig`; existing
sdkconfig values override sdkconfig.defaults. The historical V3 config name
enables the controller currently presented as Direct Gain V5.
