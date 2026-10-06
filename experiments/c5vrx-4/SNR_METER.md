# 10-bit SNR meter

Status: **host-proven, hardware-pending.** Nothing here is range-proven yet.

## Why

Every range lever in this tree (fixed analog BW, the edge profile, native AGC
vs Direct Gain V5, the 71C4 patch, `phy_11p_set`, lanes, hardware DCO) is
judged today through the 4-bit MODEM_DIAG view: P50/P95, origin, coherence,
glitch ppm, or the picture. At the range edge that view runs out of
resolution (P50 1-2, Q close to 0 at the collapse point, pre-q4-lab.md), so a
1-3 dB improvement is hard to see and harder to repeat.

The same RF dump word carries the full 10-bit I and Q plus the gain index
that was in force for each sample. Measured as band power, that gives a level
in dB that is steady on live FM video and has headroom at both ends. With a
no-carrier floor taken at the same gain, it gives an SNR in dB.

## How a reading works

`rf_enable_continuous_modem()` leaves the dump writer running forever in
dump-first mode with TX_START never firing; it feeds MODEM_DIAG. The HP SRAM
port is held by the CPU (`0x60095004[11:8] = 0`), so the writer's SRAM writes
go nowhere. A reading:

1. fills the 64 KiB dump bank `0x40830000..0x4083FFFF` with a sentinel. That
   bank is the first 64 KiB of the menu / idle raster (see below), so the
   reading refuses (`raster_in_use`) while the menu or the idle raster owns
   TX or a render is running;
2. with interrupts off, sets the SRAM owner field to 2 (the vendor `adctrig`
   value) for 40 us, then restores the saved register;
3. finds the run of words written in that window (non-sentinel, circular),
   drops 16 words at each end, keeps the newest 2048 (25.6 us of RF).

Interrupts stay off from the idle check to the hand-back (~150 us). If any
render happened before the analysis finished, the reading is discarded
(`raster_rendered`).

`DUMP_CTRL`, the selector, the dump format and the DIAG routing are never
written (`verify.py` checks that `snr_meter.c` contains no `DUMP_CTRL` write).
continuous-iq-findings.md is the basis: the writer fills bank A only, and the
bank is readable once ownership is returned.

### Where the bank comes from

There is no spare 64 KiB: PR #166 reports ~38 KB of heap free while
receiving. Before this change static RAM already ran from `0x40810708` to
`0x408427e0`, straight through the dump bank, so lending it would have
overwritten live state (Direct Gain V5, the CVBS monitor, the Wi-Fi connection
manager). The ~100 KB menu / idle raster now lives in a region reserved at
`0x40830000` instead of static `.dram1`:

- static RAM ends at `0x4082a168`; `c5vrx4_raster_memory.ld` fails the link if
  it ever reaches `0x40830000`;
- heap is unchanged to within a few bytes (the raster moved, it did not grow);
- the image is ~100 KB smaller, since `.dram1` carried the raster's zeros;
- nothing reads the raster while live video owns TX, and every menu or idle
  raster entry rebuilds it from `menu_raster_init()`, which writes every byte.

Every `menu_render_menu()` / `menu_init_buffers()` call bumps a generation
counter and holds a busy count; the meter needs busy = 0 at the start and the
same generation at the end.

Analysis (`snr_meter.h`, fixed point, host-tested):

- word decode: Q bits 0..9, I bits 10..19 (signed), gain bits 20..26;
- DC: capture mean, reported and removed;
- clipping: any axis at +511 or -512;
- Welch PSD: 64-point Hann, 50% overlap (63 segments), 1.25 MHz bins;
- bands: in-band |f| <= 10 MHz (17 bins), edge 12.5..20 MHz (14 bins),
  lower / upper halves of the in-band;
- SNR = 10 log10(P_in / N_in - 1), where N_in is the stored no-carrier floor
  for the same gain index (clamped at -30 dB, `na` without a floor or when the
  gain moved during the window);
- periodic rows are filtered with a median of 3 then an EMA (alpha 0.3).

## Lessons carried over from FPVGateC5MK

FPVGateC5MK (Louis Hitchcock) runs an 8-pilot RSSI scanner on the same C5 dump
engine. Findings, re-measured there on hardware, that shaped this meter:

- **Band power, not one frequency.** A single-bin or 4-bit view of FM video
  swung 7-9 dB with picture content; a ~10 MHz band read 0.26-0.47 dB
  standard deviation on live quads after filtering.
- **The spectrum is inverted on the raw word.** For x = b0 + j*b1 (bits 0..9
  as the real part) RF above the LO appears at negative frequency. C5VRX names
  bits 10..19 as I, so x = I + jQ is j*conj of that and RF above the LO is at
  positive frequency here (`SNR_SPECTRUM_SIGN`). Getting it wrong there read
  the empty mirror (a 4 dB rise instead of 35 dB).
- **Remove DC per capture.** Idle DC was -1.5 to -2.5 LSB (C5-Zero) and up to
  +10 LSB (XIAO); removing the capture mean made it irrelevant.
- **Median of 3 before smoothing.** 5.8 GHz Wi-Fi bursts appeared as single
  captures up to +27 dB; a median of 3 removed them.
- **Do not trust the scalar AGC RSSI as a level.** It refreshes only on
  packet-like activity and holds stale values on a steady carrier; the
  continuous signal-RSSI mode tracked (-48 dBm VTX on, -94 dBm off). Relevant
  to the `'` sigRSSI lab as a NO_CARRIER metric.
- **The noise floor is flat across +-38 MHz at moderate gain** (gain 30, BW40
  wide), i.e. ADC/baseband-limited there; the floor at G80+ will be front-end
  noise shaped by the analog filter, which is why the floor is stored per
  gain index rather than modelled.
- **Exact-MHz tuning matters off the Wi-Fi grid.** Gen-1 C5RX lost about
  25 dB on R3 (12 MHz from Wi-Fi channel 144) until it tuned the PHY to the
  exact frequency. C5VRX already calls `phy_set_freq` for off-grid channels;
  this meter makes a per-channel check cheap (same VTX power, `c` through the
  channels, compare `in_db`).

## Operator controls

| Key | Action |
|---|---|
| `7` | One reading: `SNR` row and an `SNR_PSD` row (centi-dB, -40..+38.75 MHz, RF orientation). |
| `8` | Floor at the current gain, 16 readings averaged. **VTX off.** Stored in NVS `c5vrx4/snr_floor`, per gain index. |
| `9` | Toggle `SNR_ROW` every 200 ms, with `filt_snr_db` (or `filt_in_db` when no floor exists for that gain). |

Row fields: `mhz off_khz gain in_db edge_db total_db lo_db hi_db peak_bin
floor_db over_db snr_db [filt_*] dc_i dc_q clips n seg window`. Powers are dB
re 1 LSB^2 of complex sample power. `window` is the number of words written in
the 40 us window (expect ~3200).

With no carrier, Direct Gain V5 goes to the table maximum, so `8` with the VTX
off stores the floor at the gain the range edge uses. Floors at other gains
need manual gain (`m`, `+`/`-`) while pressing `8`.

The idle raster takes TX 2 s after the carrier is lost and then holds the
bank, so readings stop with `raster_in_use`. For `8`, either press it within
2 s of the VTX going off or turn the idle raster off with `_` (reboot) for the
calibration. At the range edge readings continue as long as the receiver
still counts a carrier; once the idle raster takes over, there is no picture
to measure anyway.

## Bench plan

1. **Safety, VTX off.** `_` to turn the idle raster off (reboot) so live
   (noise) video owns TX, then `9` on for a minute. Expect `window` ~3200, no
   reboot, `T` heap unchanged, no change in `d` transport faults. Open and
   close the menu once while `9` runs: rows refuse with `raster_in_use` while
   it is up, the menu draws correctly, rows resume after it closes.
2. **Floor.** `8` at the V5 no-carrier gain; repeat once and check the two
   floors agree within ~0.3 dB.
3. **Short VTX burst (30-60 s).** `9` on, VTX at a fixed distance: `snr_db`
   steady to well under 1 dB, `clips` 0, `gain` matching V5. Watch the
   picture for any 40 us mark at 5 Hz; if visible, slow the rate or restrict
   readings to the vertical interval before any further use.
4. **Sign check.** In the same burst, `,` / `.` to +-1.5 MHz: `peak_bin` and
   the `hi_db - lo_db` balance follow the offset in the expected direction.
5. **Range use.** VTX into SMA attenuators (or a walk with `9` rows logged):
   the step where the picture collapses gives the picture-threshold SNR, then
   every lever is an A/B at a fixed attenuation.

## Next steps (not implemented)

- A/B the open range levers with this yardstick: fixed BW vs edge profile,
  `phy_11p_set`, native vs V5 at the edge, per-channel off-grid tuning.
- Feed `snr_db` into V5's NO_CARRIER / starved-carrier decisions, where the
  4-bit view cannot tell a weak carrier from noise.
