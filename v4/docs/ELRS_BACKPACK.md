# ELRS VRx backpack link

Status: **working end to end on the bench, 2026-10-07.**
Operator request 2026-10-07: follow the radio's VTX channel like an ELRS
VRx backpack.

## Provenance

The sender is the [ExpressLRS Backpack](https://github.com/ExpressLRS/Backpack)
(GPL-3.0) HDZero VRx target at release `1.5.9`, built for an ESP32-S3
SuperMini with three local changes (branch `c5vrx-s3-supermini` in a local
clone): a SuperMini env on the stock ESP32-S3 base, a USB echo/debug-log
build, and a one-wire mode (`C5VRX_ONE_WIRE`: no GET round trip, current
channel repeated every 2 s). The C5 side decodes ExpressLRS's MSPv2 framing
(`lib/MSP`), CRC (`lib/CRC`, poly 0xD5) and channel table (`src/rx5808.h`).

## Why an external backpack

ESP-NOW is 2.4 GHz Wi-Fi. The C5's one radio is the 5.8 GHz video source
(MODEM_DIAG), so listening would interrupt IQ production. The S3 has its own
radio and listens continuously; the C5 only reads a UART.

## Wiring

```
ESP32-S3 SuperMini            XIAO ESP32-C5
  TX (GPIO43) ───[1 kΩ]──────► D10 (GPIO10)
  GND         ──────────────── GND
  5V          ──────────────── 5V (final install only; not with both on USB)
```

D10 is free because IQ bit I[9] moved from GPIO10 to GPIO2 (the
unconnected MTMS pad, beside the GPIO3/4/5 JTAG pads that already carry IQ).
The IQ GPIOs are internal routes; the PARLIO bit order is unchanged, so the
IQ data is identical but now leaves through a different pad (operator
picture check passed 2026-10-07). GPIO10 is not a strapping
pin. GPIO2 as an IQ output is also safe at reset: it has no boot-mode, SDIO,
ROM-print or JTAG strap role (ESP32-C5 datasheet v1.5 section 3). The DAC
pins D4..D9 and the resistor network are unchanged. The 1 kΩ series resistor
limits back-powering when only one board is powered.

## Protocol and behaviour

- UART1 RX = GPIO10 (D10), 115200 8N1, pulled up. Frames: `$X<` + flags +
  function16 + size16 + payload + crc8_dvb_s2. Only
  `MSP_ELRS_BACKPACK_SET_CHANNEL_INDEX` (0x0301) is used; responses, bad CRC
  and payloads over 16 bytes are dropped and the parser resyncs.
- ELRS index (A,B,E,F,R,L x 8) maps by band and channel to the C5VRX table
  (R,A,B,E,F,L). **ELRS L band is refused**: its 5333..5613 MHz plan is not
  the C5VRX L band (5362..5621 MHz), so tuning by name would be up to
  29 MHz off.
- A low-priority task (`elrs_bp`, priority 1) only posts the request. The
  `analog_agc` control task applies it with the same steps as a button
  channel change (CFO reset, gain SEARCH, profile generation, standard
  detector reset, settings save), never from the sample path. It is
  deferred while the standalone menu owns TX.
- Only a **new** radio channel is acted on (first frame after boot always
  counts), so a channel picked on the C5 sticks until the radio sends a
  different one.
- Menu SETUP `ELRS BACKPACK` (NVS `c5vrx4/elrs_bp`, default on, reboot to
  apply). Serial `U` prints frames / CRC errors / oversize counts.

## Evidence

- `tools/test_elrs_backpack.c`: CRC equals the Backpack table CRC for all
  65,536 inputs; noisy/split/oversize/response frames; every single-bit
  error is rejected with no wrong index; 40-channel map, L refused.
- `tools/test_elrs_backpack_table.py`: all 40 R/A/B/E/F ELRS indices match
  `main/rf.c` names and frequencies.
- S3 bench (2026-10-07): bound with the operator's phrase, received the
  radio's R3 at boot and a live R5 Send VTx, emitted
  `SET_CHANNEL_INDEX 34`/`36`.
- C5 bench (2026-10-07, app `871544c1a6d0fc95`, wired S3 TX -> D10 + GND):
  a radio Send VTx R5 produced `[BACKPACK] Channel switched to R5
  (5806 MHz)`; `U` then read frames=13 crc_errors=0 oversize=0. Before the
  wiring test the C5 locked R3 at Q 55-95 %, G71-72 with I[9] on GPIO2.
  Operator (Louis), same day: picture with I[9] on GPIO2 "works fine"
  (visual comparison, not a measured equivalence).
