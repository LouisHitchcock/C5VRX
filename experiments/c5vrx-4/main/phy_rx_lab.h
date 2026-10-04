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
/* Task-only nonblocking actuator ownership. Successful acquire must be paired
 * with end_actuator. Does not change profiles or tune generation. */
bool phy_rx_lab_try_actuator(uint32_t expected_generation);
void phy_rx_lab_end_actuator(void);
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

/* Explicit native-only laboratory exception: reversible BB gate, never RF-AGC
 * destruction or forced gain. Observer must yield; 1 or 100 cycles only. */
esp_err_t phy_rx_lab_run_native_hold(unsigned cycles,
    void (*observe)(const char *stage, unsigned cycle));

/* Pre-demodulation labs (#165). Read-only status; the two A/B labs below
 * require the pinned PHY archive, pause nothing themselves (the caller pauses
 * all controllers), restore every owned field and return ESP_FAIL only when
 * that restore cannot be verified (reboot required). */
void phy_rx_lab_predemod_status(void);
/* RX DC DACs (PBUS blocks 2/3): baseline, 2x2 response, bounded closed-loop
 * correction, then exact restore and PBUS work mode. measure() returns the
 * I/Q centre in milli-cells of the current lane. */
esp_err_t phy_rx_lab_run_dco_probe(bool (*measure)(int dc[2]),
                                   void (*observe)(const char *stage));
/* BBTOP 0x67 registers 6..13 (RX RC filter capacitors): calibrated baseline,
 * +4/+8/+16/+24 codes and the 11p-equivalent 60, then exact restore. */
esp_err_t phy_rx_lab_run_filter_sweep(void (*observe)(const char *stage, int offset));
/* Fixed analog bandwidth. capture_base reads the vendor-calibrated regs 6..13
 * once after PHY init (before any offset). filter_apply writes base+offset
 * (offset 0 restores the base) inside the caller's phy_rx_lab transaction or
 * its own, and verifies the read-back. False: unpinned PHY, no base, or a
 * mismatching read-back (then the base is written back). */
bool phy_rx_lab_filter_capture_base(void);
bool phy_rx_lab_filter_apply(int offset);
int phy_rx_lab_filter_offset(void);
