#pragma once
#include <stdbool.h>
/* Optional native tracking gate; entirely inert under Direct Gain V5.
 * Startup gain ownership comes from the separate c5vrx4 NVS namespace. */
void c5vrx4_start(void);
void c5vrx4_suspend(void);
void c5vrx4_resume(void);
bool c5vrx4_console(int key);
