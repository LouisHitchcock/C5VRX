/* C5VRX by Twotoz and contributors: vendor RX DC/IQ calibration at the
 * actual receive frequency (adapted from ESPARGOS esp-sdr, see rx_recal.c). */
#pragma once
#include <stdbool.h>

/* True only on the pinned PHY binary whose calibration ABI was verified. */
bool rx_recal_supported(void);
/* Synchronous; the caller owns the PHY (lab transaction, AGC paused) and
 * restores channel, filters and gain afterwards. */
void rx_recal_run(unsigned mhz);
