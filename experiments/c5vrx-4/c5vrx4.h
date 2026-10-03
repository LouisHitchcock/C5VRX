#pragma once
#include <stdbool.h>
/* Optional native tracking gate; entirely inert under Direct Gain V5.
 * Startup gain ownership comes from the separate c5vrx4 NVS namespace. */
void c5vrx4_start(void);
void c5vrx4_suspend(void);
void c5vrx4_resume(void);
bool c5vrx4_console(int key);
bool c5vrx4_history_enabled(void);
/* Fixed-lane comparison: the available ultrafine set is forced by default. */
bool c5vrx4_ultrafine_forced(void);
bool c5vrx4_cvbs_legacy_enabled(void);
