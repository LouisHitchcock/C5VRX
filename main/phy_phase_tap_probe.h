#pragma once
#include <stdbool.h>
#include "esp_err.h"

/* Bounded boot-only MODEM_DIAG capture. Restores RF routing before video starts. */
void phy_phase_tap_probe_run(void);
/* Explicit one-shot lab request. Capture happens before live DMA starts. */
esp_err_t phy_agc_capture_arm(void);
bool phy_agc_capture_boot_run(void);
