#!/usr/bin/env python3
"""Host-execute actual RF policy shim with mock MMIO/time; no silicon simulation."""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / "main/rf.c").read_text()
start = source.index("#define ANALOG_AGC_REG")
code = source[start:source.index("esp_err_t rf_start(void)", start)]
code = re.sub(r'__asm__ __volatile__\("fence iorw, iorw" ::: "memory"\);', '', code)
harness = r'''
#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include "native_analog_agc.h"
static unsigned accesses;
static uint32_t regs[0x10000 / 4];
static uint32_t *reg_at(uintptr_t a) { ++accesses; return &regs[(a & 0xffff) / 4]; }
#define REG32(a) (*reg_at(a))
#define RX_AGC_CTRL_REG 0x600a7030u
#define NATIVE_ACQ_COUNT 11u
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(x) ((void)(x))
#define portEXIT_CRITICAL(x) ((void)(x))
static bool s_native_agc = true, s_bb_agc_off, s_iq_fine, s_native_wdg_blocked;
static unsigned s_agc_tune, s_native_acq_profile, s_native_initgain;
static unsigned s_analog_rf_transitions;
static int rf_agc_offset_db(void) { return 0; }
static unsigned rf_native_initgain(void) { return 82; }
static unsigned rf_native_initgain_vendor(void) { return 82; }
static int64_t time_us;
static int64_t esp_timer_get_time(void) { return time_us; }
static struct { const char *name; } channel = { "A1" };
#define rf_get_current_channel() (&channel)
static unsigned rf_get_frequency_mhz(void) { return 5865; }
'''
harness += code + r'''
int main(void) {
    assert(!rf_analog_agc_service());
    assert(!analog_agc_cancel());
    assert(accesses == 0); /* Default path never touches PHY. */
    REG32(0x600a7034u) = 10u << 24;
    REG32(0x600a7158u) = 13;
    REG32(0x600a71b0u) = 30u << 21;
    REG32(ANALOG_AGC_REG) = 0x40000180u;
    rf_analog_agc_request(1);
    assert(rf_analog_agc_service());
    assert(s_analog_agc.active && REG32(ANALOG_AGC_REG) == 0x400001a0u);
    rf_analog_agc_request(2); /* Repeated commands cannot extend the deadline. */
    assert(!rf_analog_agc_service());
    assert(s_analog_agc.deadline_us == ANALOG_AGC_TRIAL_US);
    time_us = ANALOG_AGC_TRIAL_US - 1;
    assert(!rf_analog_agc_service());
    assert(REG32(ANALOG_AGC_REG) == 0x400001a0u);
    time_us++;
    assert(rf_analog_agc_service());
    assert(!s_analog_agc.active && REG32(ANALOG_AGC_REG) == 0x40000180u);
    rf_analog_agc_request(2);
    assert(rf_analog_agc_service());
    s_iq_fine = true; /* Context change restores the policy. */
    assert(rf_analog_agc_service());
    assert(REG32(ANALOG_AGC_REG) == 0x40000180u);
    rf_analog_agc_request(1);
    assert(!rf_analog_agc_service()); /* Fine IQ refused. */
    s_iq_fine = false;
    rf_analog_agc_request(1);
    assert(rf_analog_agc_service());
    REG32(ANALOG_AGC_REG) = 0x60000180u; /* Vendor overwrite: keep new bits. */
    assert(rf_analog_agc_service());
    assert(!s_analog_agc.active && REG32(ANALOG_AGC_REG) == 0x60000180u);
    rf_analog_agc_request(1);
    assert(rf_analog_agc_service());
    rf_analog_agc_request(0);
    assert(rf_analog_agc_service());
    assert(!s_analog_agc.active && REG32(ANALOG_AGC_REG) == 0x60000180u);
    rf_analog_agc_request(1);
    analog_agc_transition_begin(); /* Retune cancels a queued start. */
    assert(!rf_analog_agc_service());
    rf_analog_agc_request(1); /* Requests cannot apply while vendor retunes. */
    assert(!rf_analog_agc_service());
    assert(!s_analog_agc.active);
    analog_agc_transition_end();
    REG32(0x600a702cu) = 1u << 23; /* Forced gain refused. */
    rf_analog_agc_request(1);
    assert(!rf_analog_agc_service());
    assert(!s_analog_agc.active);
    REG32(0x600a702cu) = 0;
    s_native_acq_profile = 10;
    REG32(0x600a7034u) = 127u << 24;
    rf_analog_agc_request(1);
    assert(rf_analog_agc_service()); /* Retain the user's selected 127 timing. */
    assert(REG32(0x600a7034u) == 127u << 24);
    rf_analog_agc_request(0);
    assert(rf_analog_agc_service());
    s_native_acq_profile = 3;
    rf_analog_agc_request(1);
    assert(!rf_analog_agc_service()); /* Other timing trials are separate. */
    return 0;
}
'''
with tempfile.TemporaryDirectory() as directory:
    cfile, binary = Path(directory) / "policy.c", Path(directory) / "policy"
    cfile.write_text(harness)
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-I" + str(root / "main"), str(cfile), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True, stdout=subprocess.DEVNULL)
print("Native analog RF shim: idle has no MMIO, refusal/expiry/overwrite/manual rollback passed")
