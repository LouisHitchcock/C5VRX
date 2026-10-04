# HDZero goggles and the no-carrier idle raster

C5VRX by Twotoz and the C5VRX contributors. Background: Leon reported that
modern HDZero goggles sometimes showed no video from C5VRX while 14-year-old
analog goggles did. On 2026-10-04 Leon shared an HDZero user's experience
(community report, not a measurement):

- HDZero shows a green screen on signal loss, or when a module sends a signal
  it does not accept. A TBS Fusion module shows green before it has locked a
  signal; it goes away after lock or after opening the module menu.
- Dominator-class goggles digitise and show almost anything. HDZero passes
  the analog signal through a TP2825 decoder, a deinterlacer and a scaler, so
  it is much stricter about the PAL/NTSC specification.
- Module menus are NTSC. NTSC seems slightly more tolerant than PAL.

## What the HDZero firmware does (source reading)

[hd-zero/hdzero-goggle](https://github.com/hd-zero/hdzero-goggle) (GPL-3.0),
revision `adb901d9493a3f3d0ca3b052eaa693e51046dd3a`. The relevant code is
`src/driver/hardware-goggle.c` and `hardware-goggle2.c` (`AV_in_detect`),
`src/core/thread.c` (`thread_peripheral`) and `src/driver/tp2825-goggle*.c`.
This section describes the behaviour; no HDZero code is copied. The status
bits are named only through the masks the firmware uses.

| Behaviour | Detail |
|---|---|
| Poll rate | TP2825 register 0x01 is read once per peripheral pass, about every 100 ms (51 x 2 ms sleeps plus I2C). |
| PAL/NTSC switch | If the decoder reports lock in the other standard on two consecutive polls, the firmware switches the TP2825 mode. Goggles 2 also switches the display timing (720p50/720p60). The lock state is then reset. |
| Lock state | video loss -> search on any H or V/H lock; search -> locked after 10 consecutive polls with V/H lock (~1 s); locked -> search after 5 polls without it; a video-loss bit returns to video loss. Goggles 1 also changes TP2825 register 0x23 (clamp) with this state. |
| Use of "locked" | DVR auto-record start/stop and the video alarm. |
| Default | `analog_format` defaults to NTSC; booting into the analog module uses that setting. |

## What this means for C5VRX

1. **Noise is the worst output for HDZero.** With no carrier, the live
   demodulator outputs receiver noise. A Fatshark shows snow. A TP2825 sees no
   valid sync (green screen) and may briefly report lock in either standard.
   Two such polls switch the goggles' standard, and the display mode with it,
   which must be undone and re-locked when the transmitter appears.
2. **Keep one standard.** The menu raster already follows the live
   standard (AUTO). The last stable live standard is now stored, so the menu
   and the idle raster start in it after a reboot, not in a default.
3. **Amplitude and sync depth** are handled separately: STD150 (1.0 V
   sync-to-white under 75 ohm) and the default-on sync-referenced level servo
   (CVBS_OUTPUT.md, CVBS_LEVEL.md).
4. **Every reboot and TX restart is a video loss** for the decoder, and it
   then takes ~1 s to count as locked again. Settings toggles reboot; avoid
   them in flight.

## No-carrier idle raster

`idle_raster.h` (host-tested in `tools/test_idle_raster.c`) and
`idle_raster_service()` in `main/video.c`.

- **Entry.** All of the following must hold for 2 s (40 control windows of
  50 ms):
  - carrier coherence `q_phase < 25`;
  - no valid sync for at least 2 s;
  - the gain owner is in its no-carrier survival state (V5 at the table
    maximum and not settling, or native AGC);
  - TX is live: no menu, lab, sweep or channel scan;
  - the first-boot fixed-BW calibration has already been tried (it needs 3 s
    of live no-carrier listening).
- **Output.** The standalone BT.470 menu raster: sync, equalising/broad pulses
  and colour burst unchanged, in the live, last stable, or selected standard.
  The picture is blanking-level black with one dim line,
  `C5VRX <channel> <MHz> NO SIGNAL`. The level matches the live STD150
  blanking (code 20, about 0.33 V vs 0.31 V), so the decoder clamp barely
  moves.
- **Exit.** On the first window with `q_phase >= 40`, or on any valid sync,
  live video restarts (the normal menu-exit path, a few ms). A weak
  transmitter showing sync fragments never enters the raster: every fragment
  restarts the 2 s count.
- **Gain.** While the raster owns TX, the V5 observers are paused as in the
  menu, so the gain stays at the table maximum. AGENTS requires NO_CARRIER to
  return to the known high-gain survival state, and that is also the most
  sensitive state for detecting a carrier. Native AGC keeps running in
  hardware.
- **Buttons.**
  - Short press: changes channel as in live video, and the raster shows the
    new channel.
  - Long press: opens the menu. Safe Flight still applies.
  - The 3 s recovery hold works.
  - The menu inactivity timeout does not apply to the raster.
- **Labs.** RX-only labs (`=`, `@`, `#`, `$`) run while the raster owns TX.
  Other labs refuse as they do in the menu.
- **Standard memory.** After 20 valid sync windows in one detected standard,
  that standard is stored once in NVS `c5vrx4/last_std`.
- **Failure.** If the menu raster cannot get its DMA descriptors, live video
  continues and the raster retries after 10 s.

| Key | Action |
|---|---|
| `_` | Toggle the idle raster (NVS `c5vrx4/idle_raster`, default on), reboot |
| `!` | `IDLE_RASTER` line: enabled/active, entries/exits/failures, last event, standard, stored last standard |
| `v` | Existing standard mode AUTO/NTSC/PAL; AUTO uses live, then stored |

Opting out (`_`) restores snow, for example for a crash search with
tolerant goggles. Sync fragments keep live video either way, but very faint
carriers below `q_phase` 40 without sync are hidden by the raster.

## Hardware acceptance (pending)

1. HDZero, VTX off: after ~2 s, black with the status line, no green screen,
   no PAL/NTSC toggling; `!` shows `active=1`.
2. Power on the VTX: live picture without a standard switch, and time to
   picture compared with `_` off. Repeat with a PAL camera after one PAL
   session (stored standard).
3. Walk to the range edge: the raster must not appear while any sync is
   visible on a tolerant monitor; record the `IDLE_RASTER` enter/exit lines.
4. Native AGC with the mask, menu open/close from the raster, short/long press,
   Safe Flight and the 3 s recovery, `=` with the VTX off.
5. Fatshark: unchanged live picture; black instead of snow without a carrier.

## Limits

- The raster hides receiver noise only when no carrier is present. It cannot
  keep HDZero locked through a weak, damaged signal: the live path still
  recovers the transmitted waveform and does not regenerate sync (AGENTS
  invariant; the flywheel experiment stays parked).
- Carrier detection uses the existing coherence metric at maximum gain. The
  thresholds (25 / 40) come from the BW calibration and witness gates and are
  not measured at the range edge.
- The TP2825 status bits and their reaction to the C5VRX waveform are not
  measured here. The firmware behaviour above is from source, not a bench
  log.
