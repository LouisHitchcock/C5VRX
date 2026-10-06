/* C5VRX by Twotoz and contributors: live 10-bit band-power / SNR meter.
 * Opens the already-running dump writer's SRAM port for ~40 us, then
 * measures the full 10-bit I/Q on the CPU (SNR_METER.md). The MODEM_DIAG /
 * PARLIO video path and the dump engine registers are never written. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

/* '7' one verbose reading (bands, DC, clips, gain, PSD row);
 * '8' no-carrier floor at the current gain (VTX off), stored in NVS;
 * '9' toggle 5 Hz SNR rows for range walks. Returns true when consumed. */
bool snr_meter_console(int key);
/* Called from the console task loop; runs the periodic rows. */
void snr_meter_tick(void);

/* Provided by video.c: the dump bank is the first 64 KiB of the menu/idle
 * raster, reserved at 0x40830000. region_ok: reservation in force and static
 * RAM below it. idle: neither menu nor idle raster owns TX and no render is
 * running; *gen changes on every render, so a reading is valid only if it is
 * unchanged afterwards. Call idle with interrupts masked. */
bool video_raster_region_ok(void);
bool video_raster_idle(uint32_t *gen);
