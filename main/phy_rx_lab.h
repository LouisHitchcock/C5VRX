#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/* Observation only: no UART or PHY calls from OSI wrappers. */
void phy_rx_lab_osi_event(bool enable);
void phy_rx_lab_begin(const char *reason);
void phy_rx_lab_end(void);
void phy_rx_lab_capture_vendor(void);
uint32_t phy_rx_lab_generation(void);
bool phy_rx_lab_busy(void);
void phy_rx_lab_poll(void);
void phy_rx_lab_toggle_monitor(void);
void phy_rx_lab_dump(bool analog_i2c);
void phy_rx_lab_mark(void);
/* Single isolated, ten-second override. Every transaction cancels it first. */
void phy_rx_lab_next_profile(void);
void phy_rx_lab_stock(void);
bool phy_rx_lab_profile_active(void);

/* Synchronous task-only A/B. Callback observes fresh samples without RF writes.
 * Caller pauses all controllers; ESP_FAIL means rollback failed, reboot required. */
esp_err_t phy_rx_lab_run_11p_probe(void (*observe)(const char *stage));
