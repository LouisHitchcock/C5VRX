# C5 libphy: native AGC and baseband reset audit

Investigation on 2026-09-30 against PR #122 head
`25afb9db85570944b46dc84334f046c9a963b646`. This is a binary audit and a
proposed bench experiment, not a noise fix or a hardware-tested patch.
No production PHY setting is changed by this document or its audit tool.

## Exact input and reproduction

ESP-IDF v6.0.2 resolves `components/esp_phy/lib` to
[`59c1234e929212aec0fdda75769b759951235536`](https://github.com/espressif/esp-phy-lib/tree/59c1234e929212aec0fdda75769b759951235536).
The examined file is `esp32c5/libphy.a`, SHA-256:

```text
dbf33c418c8d408d4005c849d12a1432deea82e2e5e57de3c8ddf914d104fffb
```

All 21 archive members were enumerated. Their symbols, instructions and
relocations were inspected; focused functions were also independently decoded
with Capstone RV32+C. GNU binutils 2.42 confirms the watchdog instruction
sequences. Archive inspection covers library code, not undisassembled ROM or
all Wi-Fi/MAC libraries.

```sh
python tools/analyze_native_agc_phy.py /path/to/esp32c5/libphy.a --disassembly
```

Use `--objdump /path/to/riscv32-esp-elf-objdump` if necessary. The tool rejects
a different archive hash by default and emits JSON to stdout. It never patches
the archive. Dependencies are Python's standard library and GNU RISC-V objdump.

## New candidate: baseband watchdog reset

The library has a separate baseband watchdog configuration and reset path.
It is distinct from the CPU/task watchdog and from disabling/forcing gain.

| Function | Object / instruction offset | Confirmed operation |
| --- | --- | --- |
| `phy_bb_wdg_cfg()` | `phy_reg.o`, `.iram1` + `0x43c`, 44 bytes | Writes watchdog config at `0x600A7C3C`; sets `0x600A7C40[31]` |
| `phy_bb_wdt_rst_enable(en)` | `phy_reg.o`, own text section, 28 bytes | Replaces only `0x600A7C40[31]` with `en` (0 or 1) |
| `phy_bb_wdt_int_enable(en)` | same object, 32 bytes | Replaces only `0x600A7C40[30]` |
| `phy_bb_wdt_timeout_clear()` | same object, 20 bytes | ORs `0x600A7C40[29]`; effect/acknowledgement semantics require bench verification |
| `phy_bb_wdt_get_status()` | same object, 10 bytes | Reads and returns the full word at `0x600A7C08`; status bit definitions are not recovered |
| `phy_bb_fsm_rst()` | `.iram1` + `0x3f2`, 28 bytes | Pulses `0x600A7C28[1]` |

Equivalent operations in the pinned binary:

```c
/* phy_bb_wdg_cfg */
REG[0x600A7C3C] = (REG[0x600A7C3C] & 0xBFFF0000u) | 0x400000AAu;
REG[0x600A7C40] |= 0x80000000u;

/* phy_bb_wdt_rst_enable: en must be 0 or 1 */
REG[0x600A7C40] = (REG[0x600A7C40] & 0x7FFFFFFFu) | (en << 31);

/* phy_bb_fsm_rst */
REG[0x600A7C28] |= 2u;
REG[0x600A7C28] &= ~2u;
```

`phy_set_bb_wdg()` also exposes configuration arguments: a0 -> 7C3C[31],
a1 -> 7C3C[30], a2 -> 7C3C[29:16], a3 -> low config bits, a4 ->
7C40[31], a5 -> 7C40[30], a6 -> 7C40[29]. Do not call this broad setter
with guessed timer units or unchecked a3: the instruction ORs a3 without
first limiting it to 16 bits. The meanings and time units of the config fields
are not established. Do not infer a 25–50 us timer from the constant 0xAA.

**Hypothesis:** repeated receive watchdog resets may contribute to native
reacquisition on continuous non-Wi-Fi FM. This has not been measured. Packet
abort, other hardware events and RF saturation remain alternatives. The
existence of this reset path is not evidence that it causes the observed noise.

## Initialization and safe patch placement

`phy_reg_init_new()` in `phy_init.o` calls, in order (other calls omitted):

1. `phy_agc_reg_init_new()`
2. `phy_bb_reg_init_new()`
3. `phy_bb_wdg_cfg()`
4. `bb_agc_reg_update()`
5. `phy_bb_fsm_rst()`
6. `phy_mac_enable_bb(1)`
7. `phy_rx_pkdet_num_set()`

Wake initialization and `phy_bb_init()` invoke that initializer. The library
calls `phy_enable_agc()` from wake, BB initialization, channel setting and
channel-offset setting. It clears `7030[29]`, then sets and clears `702C[23]`.
These are explicit gain-control writes, not a CPU gain-acquisition loop.
No per-sample acquisition-delay loop was found in the examined AGC routines.
This does not prove no tracking control exists elsewhere in silicon or ROM.

The least invasive experiment is a **RAM-only, explicit lab option applied
after vendor initialization/calibration**, using the existing function
`phy_bb_wdt_rst_enable(0)` or the equivalent one-bit masked write. It must
leave `7030[29]` clear, `702C[23]` clear, and the RF-saturation policy unchanged.
Avoid enabling a new PHY interrupt without a handler. Save and restore only
the changed reset bit, not an entire live/status register. Reboot restores the
vendor path; log/read back this bit after every retune and wake.

If the hardware experiment succeeds, the preferred implementation is a small
source-owned PHY configuration shim after the vendor receive setup, not a
redistributed edited `libphy.a`. The current `rf_start()`/`rf_set_channel()`
already have post-vendor application points for the acquisition profiles.
Those hooks alone do not cover every possible wake/reinitialization path;
verify actual calls and final-ELF bindings before making this production.

## Linker wrapping: useful, but not universal

The archive relocation from `phy_init.o` to `bb_agc_reg_update()` in
`phy_reg.o` is a candidate for `--wrap=bb_agc_reg_update`: call the real
function, then apply the validated policy. The watchdog setup precedes it.
This is an alternative if post-initialization restoration is being overwritten.
Avoid repeatedly calling the entire initializer during live reception.

A minimal RV32 GNU ld experiment confirmed:

- an undefined cross-object reference is redirected to `__wrap_vendor`;
- `__real_vendor` resolves to the original function;
- a reference to a function defined in the same object bypasses the wrapper.

Therefore `--wrap=phy_reg_init_new` cannot intercept the relevant
same-object calls from `phy_init.o`. Merely defining a second strong function
can also produce duplicate definitions when `phy_reg.o` is pulled for its
other symbols. ROM-internal calls bypass archive interposition as well.
The IDF ECO3 ROM map supplies PHY symbols; symbol presence in libphy does
not guarantee which implementation the final C5 firmware calls. Inspect the
ELF/map and relevant call sites. A synthetic linker proof is not a firmware
build or a hardware proof.

## Bench decision, not just a cleaner stationary picture

Use the normal coarse top-four-bit IQ path, one fixed channel/BW/demodulator,
vendor timing, vendor start gain and offset disabled. Fine-lane folding would
confound this first experiment.

1. Record baseline 7C3C, 7C40 and 7C08 plus full-word IQ/gain/FSM dumps.
   Determine status/timeout behavior experimentally; do not invent a status bit
   or treat a sticky status snapshot as an event count.
2. Apply only the watchdog-reset-bit change. Capture many independent windows
   at matched RF levels, alternating vendor -> experiment -> vendor. Measure
   gain-change cluster duration/rate, bad-IQ fraction, phase error and video.
3. Step RF power/attenuation in both directions while the experiment is active.
   Require native gain to respond to overload and weak RF. Check no-carrier,
   reacquisition, channel changes and prolonged recovery as well.
4. If gain becomes fixed, the experiment has disabled necessary acquisition;
   reject it even if stationary video looks clean. If events persist, this reset
   path is not sufficient. If acquisition is reduced and native gain still
   responds, investigate the exact status/timer coupling before promotion.
5. Restore vendor immediately on stalled gain or receive failure. Do not add a
   CPU gain fallback to make the experiment appear successful.

No C5 is attached to this investigation; none of these hardware results has
been obtained. This is a new testable candidate, not a proven native continuous
AGC mode.

## Existing acquisition candidates and target offset

The pinned `phy_agc_reg_init_new()` confirms the PR's owned fields and
initial constants:

| Field | Mask | Initialization |
| --- | --- | --- |
| `7034[30:24]` | `0x7F000000` | 10 |
| `7158[6:0]` | `0x0000007F` | 13 |
| `71B0[27:21]` | `0x0FE00000` | 30 |

Their exact timing/control meanings remain unknown. Faster gain-change
spacing must not be called a win without final settling, event rate, total
bad-IQ occupancy and range tests. Test them separately from the watchdog bit.

`phy_set_rx_comp_new()` writes -30/-32 to `702C[7:0]` and `70A0[31:24]`
based on phy_param[0x2A]. It does not establish that either byte is a calibrated
continuous AGC target in dB. PR #122's later live offset check found unchanged
P50 and hunting: do not promote that self-calibrator or combine it with this
experiment. Its CPU updates can obscure the causal result.

Recommended priority: **watchdog/reset causality experiment**, then native
acquisition-policy timing tests, with target-level work remaining separate.
Do not modify a vendor binary until a specific behavior-changing patch has
been physically demonstrated and source-level hooks are shown insufficient.

## Implemented live switch and first C5 readback

The normal video firmware now implements serial `&` as a RAM-only toggle
of `7C40[31]`. It refuses entry unless native AGC is enabled, packet AGC
is enabled, forced-gain bits are clear, coarse IQ is selected, and the older
timing/start-gain/offset overrides are absent. It saves/restores only the
reset bit, never enables an interrupt, never clears status and never changes
the watchdog timer fields. Reboot, channel/frequency retune and bandwidth
change restore vendor behavior. `T` prints `AGC_ACQ` and `AGC_WDG` readback.
If the reset bit is already zero, the toggle returns `ESP_ERR_NOT_SUPPORTED`
instead of pretending to apply a change.

ESP-IDF v6.0.2 compiled the normal image (0x11c7d0 bytes); all 214 repository
architecture checks passed. The COM10 flash hashes were verified. Before
any toggle, A1/no-carrier telemetry showed native AGC, vendor acquisition,
P8 FULL, offset disabled, and:

```text
cfg=0x801800aa ctrl=0x00000000 status=0x2520a032 reset_en=0
agc_ctrl=0xc3c5a6d5 comp_ctrl=0x324053e2
```

Thus **the live baseline reset bit was already disabled** despite the
archive initializer setting it. This reinforces the audit's warning about
actual vendor/ROM paths and later configuration. Carrier-present readback
is still needed; no watchdog change or noise improvement is claimed. Raw
status remains undecoded and must not be called a timeout/event counter.

Carrier-present follow-up on the same A1 setup also read `ctrl=0x00000000`
and `reset_en=0`, with `cfg=0x801800aa`, vendor acquisition profile 0,
native AGC enabled and P8 FULL. The native polling telemetry still reported
`agc_state_sw_per_ms_x10=233` and `agc_start_reentry_per_ms_x10=58`;
IQ `p50=7`, `clip_pm=24`, `coh_pm=340`, signal strength 91. These are
short register-poll/coarse-IQ snapshots, not full-word event durations.
The reset-block toggle was **not applied** because the bit was already off.
Consequently disabling this particular reset bit offers no change to this
live baseline and cannot explain away the observed reacquisitions. This
does not rule out other reset/packet-abort/retrigger paths. Vendor remains
active; no picture improvement or calibrated timing reduction is claimed.
