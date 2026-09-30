# Native AGC V2: Phase8 image quality, AFC reference, range (#121, #115, #118)

Status: research, measurement tooling and opt-in controls. Default
behaviour is unchanged: native hardware AGC stays the only gain owner (#121),
AFC stays OFF, the start-gain override stays at the vendor value and the
demodulator stays Phase8. Production RF behaviour differs from `main` only by
releasing FFT scale again after a channel change in native mode.

## Native AGC register semantics

Sources: the pinned C5 `libphy.a` (disassembled) and
[ESPARGOS esp-sdr](https://github.com/ESPARGOS/esp-sdr), which implements
hardware-AGC and manual gain on C5/C6/C61. Items marked *bench* were measured
on this board.

| Register / routine | Meaning | Evidence |
|---|---|---|
| `0x600A702C` [31:24] / [23] | forced gain index / force enable (`phy_force_rx_gain`) | disassembly |
| `0x600A702C` [14:8] | highest calibrated gain index (83 here) | esp-sdr `gain_max()`, `phy_agc_max_gain_set` |
| `0x600A7094` [8:2] | AGC **initial gain**: each acquisition starts here (vendor: max-1 = 82) | esp-sdr C61, `phy_agc_max_gain_set`; *bench*: 7078 re-enters 82 at every restart |
| `0x600A713C` [24:18] | AGC gain threshold (vendor: max-1 = 82) | esp-sdr C61, `phy_agc_max_gain_set` |
| `0x600A7030` bit 29 | baseband AGC disable (`phy_disable_agc` / `phy_enable_agc`, reversible) | disassembly |
| `0x600A705C` | RF-side AGC / saturation intervention; `phy_rfagc_disable` writes 0 (plus clears bits in `0x600A284C`, already 0 in native mode) | disassembly, esp-sdr disables RF saturation intervention for stable gain |
| `0x600A706C` [15:8] | signed signal RSSI (`phy_get_sigrssi`) | disassembly |
| `0x600A706C` [7:0], `0x600A7078` [7:0] | undecoded AGC state; 7078 walks 82 -> 60 -> 58 at each re-acquisition | *bench* |
| `0x600A7064`, `0x600A7114` | four ascending saturation bytes each (`phy_wifi_agc_sat_gain`) | *bench*: +4 A/B had no measurable effect |
| `0x600A7068` | packet-detect config (`phy_rx_pkdet_num_set` = 0x808) | disassembly |
| `0x600A7010/7014` [31:23], `0x600A7044` [7:0] | rx-sense detection thresholds (`phy_rx_sense_set`) | disassembly |
| `0x600A8004..807C` | written only by `bb_agc_reg_update()`; `8028 = 0xC0403020` looks like stacked level thresholds | disassembly; untested |
| sample-dump word [27:20] / [31:28] | per-sample gain index / AGC FSM state (esp-sdr dump format; Q in [9:0], I in [19:10] as routed to MODEM_DIAG) | esp-sdr; not yet read on C5VRX |

## Bench findings (A1 5865 MHz, VTX close)

- Native AGC re-acquires constantly with a carrier present: the state byte
  changes 137-319 times per ~20 ms. `7078` re-enters 82 roughly 1-4 times per
  ms, i.e. 0.064-0.256 re-entries per 64 us line on average. With no signal
  it can sit still. These bytes are state proxies, not validated per-sample
  gain metadata; re-entry is not proof of a hardware restart.
- Pinning the gain (force bit) gave a perfectly steady Q4 circle and zero phase
  jumps, but the picture got noisier at the AGC's own low median gain. Firmware
  holding is also ruled out by #121 (zero CPU gain decisions).
- **Field rounds 1-3 (`7128`, `7034`, `7158`, `71B0`, `7068`, `7010`,
  `7014`, `7044`) are invalid**: a BOOT short-click had moved the receiver to
  A2, so every reading shows no carrier (hard-jump rate >= 928 per mille,
  coherence ~0). P8ENV now carries `ch=`/`mhz=`, and `p8env_sweep.py` warns on
  carrier-less rows.

## Blue blacks / constant static: Q4 quantisation in the colour band

`tools/sim_phase8_false_colour.py` models the live path exactly (Q4 cells ->
256-code LUT -> 50 ns endpoint delta, as in `fm_phase8_hr_live.bsasm`) and
measures the energy the DAC output puts in the PAL/NTSC chroma band on flat
picture areas ("false colour"):

| Q4 radius (cells) | false colour, IRE rms (PAL, sigma 0.25) |
|---|---|
| 1.5 | ~6.5 |
| 2.5 (native AGC today) | ~3.9 |
| 4.0 | ~2.4 |
| 5.5 | ~1.8 |

With little noise, specific picture levels produce deterministic tones up to
~4.8 IRE at radius 2.5: a dark tone at such a level decodes as a colour cast.
An arc-midpoint LUT does not help (4.38 -> 4.28), and the live path already
uses Golden's 50 ns delta. **The fix is a larger Q4 radius, i.e. the native
AGC target level, if that control is identified and measured.** Larger
radius with fixed post-gain noise is not a constant-SNR RF range comparison.

## AFC

- **Hygiene (#115 items 2, 4, 5)**: `main/afc_state.h`. CFO pairs need both
  endpoints valid; no sample is accepted across a settle period or gain/PHY
  transition; state resets on mode, channel, profile, BW, offset or RF
  generation change; a correction needs 8 fresh samples. Test:
  `tools/test_afc_state.c`.
- **V2 reference (#115 items 1, 3, 7-10)**: `main/afc_v2.h` measures
  instantaneous frequency only on the sync tip and on the back porch after the
  colour burst. Sync is detected on the demodulated frequency, independent of
  FM polarity, bracketed by matching porches and confirmed by the colour burst,
  which also reports PAL/NTSC. Smoothing is a 20+6 sample cascade that nulls
  the VTX audio subcarriers (6.0 / 6.5 MHz). Reported in P8ENV (`afc2_*`).
  Test: `tools/test_afc_v2.c`, 48 synthetic PAL/NTSC cases (both polarities,
  colour burst, random picture blocks, audio subcarriers) through the real
  quantiser.
- **V2 correction**: `main/afc_v2_ctrl.h`. AUTO AFC (default OFF, acquisition
  only) now corrects only from that reference: 16 burst-confirmed estimates,
  same polarity/standard, MAD <= 80 kHz, 50 kHz deadband, steps <= 250 kHz,
  at most 4 corrections per acquisition. The porch is the default centre
  reference; #115 section 9 still requires confirming the centre level and
  the sign on hardware before AUTO AFC is recommended. Test:
  `tools/test_afc_v2_ctrl.c`.

## VTX facts that matter here (RTC6705 datasheet)

Almost every analog FPV VTX uses the Richwave RTC6705: 1 Vpp video input and
FM audio subcarriers at **6.0 and 6.5 MHz** at -25..-30 dBc (modulation index
~0.06-0.11, i.e. roughly 0.4-0.7 MHz peak carrier deviation each; 12 kHz audio
pre-emphasis). A normal FPV receiver low-pass filters the demodulated video,
removing them. C5VRX puts the demodulated frequency straight on the 20 MS/s
DAC, which attenuates 6.5 MHz only ~16 %, so a tone of roughly 10 IRE at
6-6.5 MHz is probably present in the CVBS output as fine static patterns. This
is unconfirmed on this VTX: the short `Q` captures were too noisy to resolve it.
A full-window capture (`z` + `tools/analyze_q4_window.py`) can expose spectral
peaks but cannot identify their cause. The review reproduced 22 dB / 20 dB
peaks in these bands from constant-envelope FM with NO audio, due to Q4
harmonics. Change picture level / CFO and compare the tone's motion before
attributing it to audio. No sound trap or AGC register change is enabled by
these observations. The loaded DAC resistor network and 470 pF capacitor
must also be included in any measured filter response.

## Lab tools in this PR (all read-only unless stated)

| Key | Function |
|---|---|
| `E` | P8ENV row: channel, Q4 envelope, state-byte changes and start-byte re-entry proxy, start gain, AFC V2 reference |
| `h` | fast poll of the native AGC state bytes (histogram, switch rate) |
| `Q` / `z` | raw Q4: four 64-sample runs / one contiguous 4092-sample window |
| `T` | AGC register dump (`0x600A7000..71FC`, `0x600A8000..807C`) |
| `P` `M` `J` `B` | *writes*: select / step / restore candidate AGC fields (reversible, reboot restores) |
| `w` | *writes, persisted*: native AGC start gain vendor -> 74 -> 66 -> vendor |
| `u` | *persisted, reboots*: Golden <-> Phase8 VIDEO32 (bounded video-transfer candidate) |
| `d` | *persisted, reboots*: original Phase8 FULL (8-bit endpoint arithmetic) |
| `i` | *persisted, reboots*: PLL-tracking A/B (only in a lab build with `CONFIG_ESP_PHY_DISABLE_PLL_TRACK=n`) |
| `N` | *persisted, reboots*: native AGC <-> firmware gain |

## Next bench session (script, not exploration)

1. Verify `ch=A1`, carrier present (coherence > 0) before anything else.
2. Start gain A/B: hold each of vendor / 74 / 66 for about 30 s, operator
   answers "blue / not blue, noisy / clean" per setting (no timing race).
3. Range: same spot at the edge, Phase8 vs Golden (`u`) and start gain vendor
   vs best candidate; compare with `main` if doubts remain.
4. Several `z` windows with different picture levels / CFO: investigate the
   6.0/6.5 MHz peaks and candidate envelope edges. Neither proves audio or AGC.
5. AFC V2 on hardware: step the offset (`,` / `.`) and check `afc2_porch_khz`
   follows 1:1 with the expected sign; only then try AUTO AFC.
6. PLL tracking A/B for the white-screen / stuck-low state (lab build).

## Review fixes and bounded video candidate

The VIDEO OUTPUT menu now cycles PHASE5 -> PHASE8 FULL -> P8 VIDEO32 with a long
press; the choice is persisted and applied when leaving the menu. Serial
`u` / `d` selects and reboots. Fresh installations still default to P8 FULL;
existing `demod_golden` choices migrate without changing their meaning.

P8 FULL maps the entire signed endpoint range onto 64 DAC codes, pedestal
32. It has much less video swing than Golden at the same RF. P8 VIDEO32
uses Phase8 cell-centre angles to design a 32-state codebook and a bounded
pair LUT: pedestal 20, gain 0.75 code per Phase8 bin, saturation before
conversion, tapered far tail at 96..112 bins (135..157.5 degrees). The far
tail is a video prior, not proof that a transition is impossible. Both use
two bundles, 20 MS/s unique DAC output duplicated at 40 MHz, and continuous
state across DMA boundaries. VIDEO32 does NOT preserve eight bits of
endpoint state; it is an explicit precision/transfer tradeoff. A full-precision
nonlinear transfer has not been proven at two-bundle throughput.

`tools/test_phase8_video.py` exhausts all 65,536 raw endpoint pairs through
the emitted assembly. `sim_phase8_false_colour.py` also models VIDEO32 and
reports brightness error instead of returning a placeholder zero. At sigma
0.25, PAL dark maximum at radius 2.5 is approximately 4.38 IRE for FULL versus
4.79 for VIDEO32, and at radius 5.5 2.31 versus 2.78. Thus this candidate is
NOT advertised as a fine-grain cure and is opt-in. It fixes output range and
unsafe amplified tails; loaded voltage, colour, sharpness and range need A/B.

Grain is already present in noiseless Q4 simulations. Eliminating it needs
better input information / suitable filtering, not simply more phase bits or
digital gain. Raising the native target, a finer pre-Q4 tap with explicit
overflow handling, or a proven realtime filter are still hardware research;
no guessed native AGC target register is written by this change.

AFC now uses individually burst-confirmed runs with sufficient valid tip and
porch pairs. Invalid estimates discard ALL consecutive history. Context is
checked across the snapshot; a bounded DMA-copy deadline prevents accepting
a copy after the ring could have lapped. Settling ticks advance before any
native/manual/profile branch. Native mode skips firmware gain state machines;
its AFC gate no longer requires firmware-controller power >=18. A conservative
IQ-envelope gate rejects suspected transitions, but cannot prove autonomous
gain stayed fixed without per-sample metadata. The 20+6 smoother group delay
is 12 samples (0.3 us), not 26 samples. AFC remains OFF by default pending
centre/sign validation.
Native acquisition lock is sticky: CFO drift alone does not authorize an
in-flight retune. Four invalid windows re-arm acquisition and its correction
budget; changing the receive context also resets the lock.

The analyzer requires complete ordered 4092-byte windows, validates both
frequency endpoints, and uses Cartesian-cell radius bounds before reporting
envelope candidates. Rotating a noiseless radius-1.2 vector no longer creates
hundreds of fake AGC events. State fields are labelled as proxies; start-byte
re-entry uses the configured start (including 66/74), not a hard-coded >=80.


## Phase5/Phase8 switching and remaining range work

Golden is the original **Phase5** endpoint discriminator. On VIDEO OUTPUT,
long press cycles **PHASE5 -> PHASE8 FULL -> P8 VIDEO32**. The first
two are the reference A/B; VIDEO32 remains an explicitly experimental,
32-state bounded-transfer candidate. Exit the menu to apply the saved choice.
Native hardware AGC owns RF gain in all three selections. The recovery
hold now also restores and persists the actual Golden/Phase5 program, rather
than only resetting a legacy enum while Phase8 remained selected.

The native sync/standard observer now uses the Phase8 raw-IQ detector in
`afc_v2.h` rather than Golden's DAC low-code threshold. Accepted burst-bearing
runs report their measured width and, when two agreeing runs fit inside the
window, their measured spacing. Standard votes require matching PAL/NTSC burst
and horizontal spacing; a lone burst earns only partial sync confidence.
This observer is independent of the selected DAC pedestal/gain.

Acquisition lock and AFC writes share the 16-estimate median/MAD stability
test. AUTO must additionally be centred; OFF/HOLD can lock a stable receive
state with a nonzero reference offset, so experimental BW AUTO cannot keep
switching solely because centering is disabled. Valid TRACK remains sticky.
Invalid DMA copies also age the native lock, clear stale CFO and discard
consecutive evidence. All slow IQ consumers use the bounded descriptor copy;
the returned ring offset belongs to that same copy, preserving endpoint parity.
The copy deadline sums the actual intervening descriptor lengths, including
the short tail node, rather than assuming a uniform ring.

For #118, compare exactly the same frozen samples through the three actual
assembly LUTs, without RF/AGC variations between modes:

```sh
python3 tools/compare_live_demods.py capture.log
python3 tools/compare_live_demods.py capture.bin --binary --parity 1
```

The JSON contains a per-window input hash, endpoint parity, sample count and
per-mode DAC swing/rail/jump statistics. Separate `z` windows are never joined
across a capture gap. Raw DAC statistics have different gains/pedestals and
are not a quality ranking or proof of RF sensitivity. Use a controlled
attenuation sweep and external CVBS picture/sync rating to establish range.
The exhaustive regression checks all 65,536 raw pairs and both byte parities.

Register semantics were checked against Espressif's pinned v6.0.2
[`ahb_dma_struct.h`](https://github.com/espressif/esp-idf/blob/v6.0.2/components/soc/esp32c5/register/soc/ahb_dma_struct.h):
`in_dscr_bf0` identifies fetched descriptor x; `in_dscr` already points at
x+1. We therefore retain the previous-buffer selection and validate its
copy lifetime. This is software snapshot hygiene, not a new proof of gapless RF.

Remaining #121 hardware work is still finding and validating native target,
hysteresis and retrigger controls, then measuring annulus/near-far recovery.
The failed target-register sweeps do not justify promoting guessed writes.
No CPU gain controller or guessed native policy patch is enabled here.
