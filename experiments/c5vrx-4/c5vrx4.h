#pragma once
#include <stdbool.h>
/* Experimental native tracking gate; it never selects a gain index. */
void c5vrx4_start(void);
void c5vrx4_suspend(void);
void c5vrx4_resume(void);
bool c5vrx4_console(int key);
bool c5vrx4_history_enabled(void);
/* Fixed-lane comparison: coarse by default; Z selects fixed ultrafine. */
bool c5vrx4_ultrafine_forced(void);
