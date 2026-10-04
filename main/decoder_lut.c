/* C5VRX by Twotoz and contributors. C5VRX-3 Phase8 FULL decoder recentring.
 * LUT host access follows the C5VRX-4 LUT16 lab (cvbs_level_hw.c): register
 * source ESP-IDF v6.0.2 ESP32-C5 bitscrambler_ll/struct headers. It never
 * uses load_lut (which changes the live width) or RUN/HALT/reset. Live RAM
 * arbitration while the engine runs still needs physical acceptance. */
#include "decoder_lut.h"
#include "predemod.h"
#include <stdio.h>

#ifndef DECODER_LUT_HOST
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_err.h"
#include "soc/bitscrambler_struct.h"
#include "hal/bitscrambler_ll.h"
static uint16_t lut_read(unsigned index)
{
    BITSCRAMBLER.lut_cfg[BITSCRAMBLER_DIR_TX].cfg0.lut_idx = index;
    return (uint16_t)BITSCRAMBLER.lut_cfg[BITSCRAMBLER_DIR_TX].cfg1.lut;
}
static void lut_write(unsigned index, uint16_t value)
{
    BITSCRAMBLER.lut_cfg[BITSCRAMBLER_DIR_TX].cfg0.lut_idx = index;
    BITSCRAMBLER.lut_cfg[BITSCRAMBLER_DIR_TX].cfg1.lut = value;
    __asm__ volatile ("fence iorw, iorw" ::: "memory");
}
static bool lut_width16(void)
{
    return bitscrambler_ll_get_lut_width(&BITSCRAMBLER, BITSCRAMBLER_DIR_TX) == 1;
}
static SemaphoreHandle_t s_lock;
static void lock(void)
{
    if (!s_lock) s_lock = xSemaphoreCreateRecursiveMutex();
    ESP_ERROR_CHECK(s_lock ? ESP_OK : ESP_ERR_NO_MEM);
    xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
}
static void unlock(void) { xSemaphoreGiveRecursive(s_lock); }
#else
uint16_t lut_read(unsigned index);
void lut_write(unsigned index, uint16_t value);
bool lut_width16(void);
static void lock(void) {}
static void unlock(void) {}
#endif

#define DECODER_LUT_ENTRIES 1024u
#define DECODER_LUT_LIMIT_MCELLS 3000
static uint16_t s_shadow[DECODER_LUT_ENTRIES];
static bool s_ready, s_blocked;
static int s_applied[2];
static unsigned s_lane;
static uint32_t s_updates, s_writes, s_faults;
static const char *s_state = "not_prepared";

static int clamp_mcells(int v)
{
    return v > DECODER_LUT_LIMIT_MCELLS ? DECODER_LUT_LIMIT_MCELLS :
           v < -DECODER_LUT_LIMIT_MCELLS ? -DECODER_LUT_LIMIT_MCELLS : v;
}

/* Write every entry that differs, bank entries of one raw byte together so
 * each disagreement lasts as short as possible (not an atomic swap). */
static bool write_table(int di, int dq)
{
    for (unsigned raw = 0; raw < 256u; ++raw) {
        uint16_t word = predemod_hr_live_word((uint8_t)raw, di, dq);
        for (unsigned bank = 0; bank < 4u; ++bank) {
            unsigned index = bank * 256u + raw;
            if (s_shadow[index] == word) continue;
            lut_write(index, word);
            if (lut_read(index) != word) {
                s_ready = false; s_blocked = true; ++s_faults;
                s_state = "readback_mismatch_blocked";
                return false;
            }
            s_shadow[index] = word;
            ++s_writes;
        }
    }
    return true;
}

bool decoder_lut_prepare(bool phase8_full, unsigned lane)
{
    lock();
    bool intact = true;
    s_ready = false;
    if (!phase8_full) { s_state = "not_phase8_full"; goto done; }
    if (s_blocked) { s_state = "blocked"; goto done; }
    if (!lut_width16()) { s_state = "lut_width"; goto done; }
    for (unsigned i = 0; i < DECODER_LUT_ENTRIES; ++i) {
        s_shadow[i] = lut_read(i);
        if (s_shadow[i] != predemod_hr_live_word((uint8_t)(i & 255u), 0, 0)) {
            s_state = "unexpected_table";
            goto done;
        }
    }
    /* Differential probe with the engine stopped: one entry changes, all
     * 1024 halfword views are checked, then it is restored and re-checked. */
    const unsigned probe = 32u;
    uint16_t changed = s_shadow[probe] ^ 1u;
    lut_write(probe, changed);
    bool ok = true;
    for (unsigned i = 0; i < DECODER_LUT_ENTRIES; ++i)
        if (lut_read(i) != (i == probe ? changed : s_shadow[i])) ok = false;
    lut_write(probe, s_shadow[probe]);
    for (unsigned i = 0; i < DECODER_LUT_ENTRIES; ++i)
        if (lut_read(i) != s_shadow[i]) { ok = false; intact = false; }
    if (!ok) { ++s_faults; s_state = "probe_refused"; goto done; }
    s_ready = true;
    s_state = "ready";
    if (s_applied[0] || s_applied[1]) {
        /* The program reload restored the pristine table: re-apply now,
         * while the engine is still stopped. */
        int di = clamp_mcells(predemod_lane0_to_mcells(s_applied[0], lane));
        int dq = clamp_mcells(predemod_lane0_to_mcells(s_applied[1], lane));
        if (!write_table(di, dq)) { intact = false; goto done; }
    }
    s_lane = lane;
done:
    unlock();
    return intact;
}

bool decoder_lut_set(int lane0_i, int lane0_q, unsigned lane)
{
    lock();
    bool ok = false;
    if (!s_ready) goto done;
    int di = clamp_mcells(predemod_lane0_to_mcells(lane0_i, lane));
    int dq = clamp_mcells(predemod_lane0_to_mcells(lane0_q, lane));
    if (!lane0_i && !lane0_q) di = dq = 0;
    if (!write_table(di, dq)) goto done;
    s_applied[0] = lane0_i;
    s_applied[1] = lane0_q;
    s_lane = lane;
    ++s_updates;
    ok = true;
done:
    unlock();
    return ok;
}

void decoder_lut_stop(void)
{
    lock();
    s_ready = false;
    if (!s_blocked) s_state = "stopped";
    unlock();
}

bool decoder_lut_ready(void)
{
    lock();
    bool ready = s_ready;
    unlock();
    return ready;
}

void decoder_lut_applied(int lane0[2], unsigned *lane)
{
    lock();
    lane0[0] = s_applied[0];
    lane0[1] = s_applied[1];
    if (lane) *lane = s_lane;
    unlock();
}

void decoder_lut_print(void)
{
    lock();
    printf("DC_RECENTER table=%s ready=%u applied_lane0_msteps=%d/%d lane=%u updates=%lu "
           "writes=%lu faults=%lu atomic_update=0 live_arbitration=hardware_pending\n",
           s_state, s_ready, s_applied[0], s_applied[1], s_lane, (unsigned long)s_updates,
           (unsigned long)s_writes, (unsigned long)s_faults);
    unlock();
}
