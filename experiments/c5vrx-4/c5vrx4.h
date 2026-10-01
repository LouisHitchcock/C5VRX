#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
/* Experimental native tracking gate; it never selects a gain index. */
void c5vrx4_start(void);
void c5vrx4_suspend(void);
void c5vrx4_resume(void);
bool c5vrx4_console(int key);
bool c5vrx4_history_enabled(void);
bool c5vrx4_lane_window_ready(uint64_t now_us);
uint8_t c5vrx4_lane_target(uint8_t current, uint8_t requested,
                         const uint8_t *sample, size_t bytes, uint64_t now_us);
void c5vrx4_lane_print(void);
