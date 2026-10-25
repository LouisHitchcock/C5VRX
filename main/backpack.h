/* C5VRX by Twotoz and the C5VRX contributors, GPL-3.0-only. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

/* UART1 RX-only on XIAO D10/GPIO10, 115200 8N1. Control task owns parsing. */
void backpack_start(void);
void backpack_poll(void);
bool backpack_take(uint8_t *elrs_index);
void backpack_print(void);
