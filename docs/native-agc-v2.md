# Native AGC V2: Phase8 image quality, AFC reference, range (#121, #115, #118)

Status: research and measurement tooling. No production control change yet.
Native hardware AGC stays the only gain owner (#121). Production RF behaviour
is identical to `main` apart from releasing FFT scale again after a channel
change in native mode.

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
  ms, i.e. several times per video line. With no signal it can sit still.
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
AGC target level.**

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
  which also reports PAL/NTSC. The result is in P8ENV (`afc2_*`). AFC does not
  act on it until the VTX's centre level is characterised (#115 section 9).
  Test: `tools/test_afc_v2.c`, 48 synthetic PAL/NTSC cases through the real
  quantiser.

## Next bench session (script, not exploration)

1. Verify `ch=A1`, carrier present (coherence > 0) before anything else.
2. Range check against `main` at a fixed distance.
3. Target-level candidates, 3 readings per step, VTX close: `7094` initial
   gain, `713C` threshold, `705C` RF saturation off, `8028` bytes, `8020`,
   `801C`. Success = larger P50 without clipping and fewer restarts.
4. PLL tracking A/B for the white-screen / stuck-low state (separate lab build
   with `CONFIG_ESP_PHY_DISABLE_PLL_TRACK=n`, toggled with `i`).
