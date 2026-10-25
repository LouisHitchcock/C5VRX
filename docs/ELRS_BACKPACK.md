# ELRS backpack on C5VRX-5

Follow-up to [C5VRX PR #189](https://github.com/Twotoz/C5VRX/pull/189):
restore the earlier HDZero/MSPv2 backpack control link in V5. The port was
built and bench-tested on V5 `5.0.493`
(`057e3971ffd3d716536a121e83013b61a43b73d8`) as
`5.0.493-backpack.1`; it is a development adaptation, not an official release.

## Wiring and binding

- C3 SuperMini GPIO21 TX -> 1 kOhm -> XIAO ESP32-C5 D10/GPIO10.
- Common GND. No return wire. Both UARTs use 115200 8N1, 3.3 V logic.
- The C3 runs ExpressLRS Backpack's HDZero VRx firmware with the one-wire
  option. Bind it to the radio's TX backpack with the same binding phrase.
  Binding belongs to the C3, not the C5; the C5 needs no phrase.
- D10 is now UART1 RX. Leave the proposed D10-to-CVBS DAC resistor
  unpopulated. This build retains six DAC bits, the GPIO27/GPIO2 IQ mapping
  of the base release, and the existing D4..D9 resistor network.

## Behaviour

The existing 50 ms control task reads at most 64 UART bytes per iteration,
without waiting. A CRC-checked MSPv2 `$X<` SET_CHANNEL_INDEX (`0x0301`)
with zero flags and exactly one payload byte supplies an ELRS channel index.
R/A/B/E/F are translated to the same named V5 channel. ELRS L is refused:
its frequencies differ from the receiver's L plan. R8 and E6..E8 retain
V5's warning about frequencies above the documented C5 range.

Only a changed radio index schedules a retune. Repeated announcements do
not keep retuning or undo a later local/menu selection. During calibration
or the splash screen the newest valid request is retained until the owner
finishes. A newer unsupported channel cancels an older pending request.
Successful retunes use the normal control path: AFC reset, profile update,
channel persistence, flight-log event and menu refresh. No CPU stage is
added to the RX40/raw32K/two-bundle/DAC20-at-TX40 hardware dataplane.

USB `U` prints link/frame/error counters; `T` includes the same counters.
An applied request prints `[BACKPACK] SET_CHANNEL_INDEX ... -> R3 5732 MHz`.
The C3's USB log separately echoes its outgoing command. Seeing both logs
and the changed C5 channel confirms the control link; it does not prove RF
picture quality or range. On startup look for `UART1 RX=GPIO10 ... ESP_OK`.

Host tests cover CRC, framing/resynchronisation, malformed packets,
single-bit corruption, channel-map equivalence, deferred latest requests
and repeat suppression. Firmware and board results are recorded in
[FINDINGS.md](FINDINGS.md). On 2026-10-25 an ELRS radio and C3 SuperMini
sender changed the XIAO C5 from R3 (5732 MHz) to R5 (5806 MHz):
`SET_CHANNEL_INDEX 36 -> R5 5806 MHz: ESP_OK`. Counters continued to
increase through 114 frames with zero CRC/UART/rejected errors, without
repeated retunes. This validates channel control; RF video quality and
range were not tested. The C3 sender target and one-wire changes are in
a separate ExpressLRS Backpack clone, outside this PR.

## Sources and modifications

Adapted from C5VRX by Twotoz and the C5VRX contributors:
[canonical repository](https://github.com/Twotoz/C5VRX),
[official website and Discord invite](https://twotoz.github.io/C5VRX/).
Original V4 implementation: PR #189, head
`ff3135decac87f3d8c8df0a4b4313265c79cb2fd`.
The later source revision read for this port was
`8fef6f8175b3392956d98240dcf5cdf458279964`:
`v4/main/video_backpack.c`, `v4/firmware/elrs_backpack.c`,
`v4/firmware/include/elrs_backpack.h`, and `v4/tools/test_elrs_backpack.c`.
The port removes the extra UART task, retains only the latest pending
request, tightens command validation, adds UART error recovery/counters,
and uses V5's control/retune lifecycle. All C5VRX material remains
GPL-3.0-only; see [LICENSE](../LICENSE).

The wire format and ELRS frequency plan follow
[ExpressLRS/Backpack](https://github.com/ExpressLRS/Backpack) release 1.5.9,
`src/hdzero.cpp`, `src/rx5808.h` and `lib/MSP` (GPL-3.0).
