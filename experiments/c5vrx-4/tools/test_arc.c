#include <assert.h>
#include <stdio.h>
#include "arc_phy.h"
#include "arc_controller.h"

/* Host stubs: arc_phy_capture_gain_table() is not used in this test. */
unsigned char phy_param[0x500];

int main(void)
{
    arc_gain_table_t table;
    arc_gain_tuple_t t;

    /* 5 GHz, the receiver's band: the vendor generator's constants
     * (esp-phy-lib 59c1234, verified by disassembly) and tuples emitted by
     * the unmodified vendor code under emulation (external audit, PR #174). */
    arc_gain_table_from_bytes(&table, NULL, 200);
    assert(table.band5 && table.max_index == 83);
    assert(arc_gain_highest_rf_stage_start(&table) == 54);
    const uint8_t starts5[] = {0, 10, 17, 25, 30, 38, 43, 47, 54};
    const uint16_t codes5[] = {24, 20, 0, 65, 97, 162, 354, 419, 487};
    const uint8_t first_counter5[] = {12, 13, 15, 12, 12, 12, 12, 12, 12};
    static const uint8_t bb[7] = {1, 3, 7, 15, 31, 63, 127};
    for (unsigned i = 0; i < ARC_RX_STAGE_COUNT; ++i) {
        assert(arc_gain_tuple_decode(&table, starts5[i], &t));
        assert(t.rf_stage == i && t.rf_code == codes5[i]);
        assert(t.bb_code == bb[first_counter5[i] / 6] && t.fine_code == 5 - first_counter5[i] % 6);
        if (starts5[i]) {   /* the entry before is the previous stage */
            assert(arc_gain_tuple_decode(&table, (uint8_t)(starts5[i] - 1u), &t));
            assert(t.rf_stage == i - 1u);
        }
    }
    const struct { uint8_t g; uint16_t rf; uint8_t bb, fine; } vendor[] = {
        {54, 487, 7, 5}, {62, 487, 15, 3}, {66, 487, 31, 5},
        {72, 487, 63, 5}, {80, 487, 127, 3}, {83, 487, 127, 0},
    };
    for (unsigned i = 0; i < sizeof(vendor) / sizeof(vendor[0]); ++i) {
        assert(arc_gain_tuple_decode(&table, vendor[i].g, &t));
        assert(t.rf_code == vendor[i].rf && t.bb_code == vendor[i].bb && t.fine_code == vendor[i].fine);
        assert(t.packed_state == (((uint32_t)vendor[i].rf << 12) | ((uint32_t)vendor[i].bb << 4) | vendor[i].fine));
    }
    assert(!arc_gain_tuple_decode(&table, 84, &t));

    /* Capture: the 5 GHz branch (phy_param[0x2a] != 0) takes its own maximum
     * at +0x126, not the minimum of the three (which refused G81..G83). */
    phy_param[0x2a] = 1; phy_param[0x124] = 80; phy_param[0x125] = 0; phy_param[0x126] = 83;
    arc_phy_capture_gain_table(&table);
    assert(table.band5 && table.max_index == 83);
    phy_param[0x126] = 81;
    arc_phy_capture_gain_table(&table);
    assert(table.max_index == 81);

    /* 2.4 GHz: runtime spans from +0x422 with the vendor start counters. */
    const uint8_t spans[ARC_RX_STAGE_COUNT] = {15, 13, 5, 8, 6, 4, 4, 6, 0};
    arc_gain_table_from_bytes(&table, spans, 80);
    assert(!table.band5 && table.runtime_spans_valid && table.max_index == 80);
    assert(arc_gain_highest_rf_stage_start(&table) == 61);
    const uint8_t starts24[] = {0, 15, 28, 33, 41, 47, 51, 55, 61};
    const uint16_t codes24[] = {64, 100, 93, 94, 107, 119, 124, 125, 127};
    const uint8_t first_counter24[] = {0, 11, 20, 20, 22, 22, 23, 22, 22};
    for (unsigned i = 0; i < ARC_RX_STAGE_COUNT; ++i) {
        assert(arc_gain_tuple_decode(&table, starts24[i], &t));
        assert(t.rf_stage == i && t.rf_code == codes24[i]);
        assert(t.bb_code == bb[first_counter24[i] / 6] && t.fine_code == 5 - first_counter24[i] % 6);
    }
    const uint8_t empty_spans[ARC_RX_STAGE_COUNT] = {0};
    arc_gain_table_from_bytes(&table, empty_spans, 200);
    assert(!table.runtime_spans_valid && table.max_index == 80);   /* 61 + counters 22..41 */

    arc_iq_correction_t iq = arc_iq_correction_decode(
        (7u << 29) | (0x7fu << 22) | (0x20u << 16));
    assert(iq.enable == 7 && iq.coef0 == -1 && iq.coef1 == -32);
    iq = arc_iq_correction_decode((1u << 29) | (0x40u << 22) | (0x1fu << 16));
    assert(iq.enable == 1 && iq.coef0 == -64 && iq.coef1 == 31);

    arc_gain_table_from_bytes(&table, spans, 80);

    arc_controller_t arc;
    arc_controller_reset(&arc, &table, 62);
    arc.settle = 0;
    arc_observation_t clean = {
        .sync = true, .sync_quality = 90, .p_median = 24, .q_phase = 80,
        .clip_permille = 0, .origin_permille = 100, .winding_permille = 20,
    };
    assert(arc_controller_tick(&arc, &clean) == 62);
    assert(arc.state == ARC_LOCK);
    arc_controller_t locked = arc;
    for (unsigned i = 0; i < 100; ++i) assert(arc_controller_tick(&arc, &clean) == 62);
    assert(arc.gain == locked.gain && arc.survival_gain == locked.survival_gain);
    assert(arc.table.max_index == locked.table.max_index);
    for (unsigned i = 0; i < ARC_RX_STAGE_COUNT; ++i)
        assert(arc.table.spans[i] == locked.table.spans[i]);

    /* No-sync noise with plausible phase must never increase gain. */
    arc_observation_t lost = {
        .q_phase = 35, .p_median = 8, .origin_permille = 300,
    };
    for (unsigned i = 0; i < 40; ++i) {
        uint8_t before = arc.gain;
        uint8_t after = arc_controller_tick(&arc, &lost);
        assert(after <= before);
    }
    assert(arc.gain == 61);
    for (unsigned i = 0; i < 100; ++i)
        assert(arc_controller_tick(&arc, &lost) == 61);

    /* Severe clipping bypasses the ordinary post-write settle interval. */
    arc_controller_reset(&arc, &table, 61);
    assert(arc.settle == 10);
    arc_observation_t clipped = {.clip_permille = 100, .p_median = 40};
    assert(arc_controller_tick(&arc, &clipped) == 57);
    assert(arc.settle == 10);
    /* Settling must not immediately reverse the cut on an origin collapse. */
    arc_observation_t transient = {.p_median = 2, .origin_permille = 950};
    for (unsigned i = 0; i < 30; ++i)
        assert(arc_controller_tick(&arc, &transient) == 57);

    /* A clipped carrier often loses sync. Previously this repeatedly cut
     * G61->G57 and then forced G61 again even with ample Q4 amplitude. */
    arc_observation_t unsynced_carrier = {
        .p_median = 24, .q_phase = 35, .origin_permille = 100,
    };
    for (unsigned i = 0; i < 200; ++i)
        assert(arc_controller_tick(&arc, &unsynced_carrier) == 57);

    /* Genuine amplitude collapse may recover sensitivity after the hold. */
    arc_observation_t collapse = {
        .p_median = 2, .q_phase = 5, .origin_permille = 950,
    };
    assert(arc_controller_tick(&arc, &collapse) == 61);
    for (unsigned i = 0; i < 100; ++i)
        assert(arc_controller_tick(&arc, &collapse) == 61);

    /* Moderate overload also needs protection when semantic sync is absent. */
    arc_controller_reset(&arc, &table, 61);
    arc_observation_t moderate_hot = {
        .p_median = 40, .clip_permille = 50, .origin_permille = 0,
    };
    for (unsigned i = 0; i < 20; ++i)
        arc_controller_tick(&arc, &moderate_hot);
    assert(arc.gain == 60);
    for (unsigned i = 0; i < 100; ++i)
        assert(arc_controller_tick(&arc, &unsynced_carrier) == 60);

    /* Sparse sync misses must not release a previously good lock. */
    arc_controller_reset(&arc, &table, 62);
    arc.settle = 0;
    arc_controller_tick(&arc, &clean);
    for (unsigned i = 0; i < 100; ++i) {
        assert(arc_controller_tick(&arc, &lost) == 62);
        assert(arc_controller_tick(&arc, &clean) == 62);
    }
    assert(arc.state == ARC_LOCK);

    /* Non-consecutive weak/hot observations must not accumulate as a fade. */
    arc_observation_t weak = clean;
    weak.p_median = 8;
    weak.origin_permille = 600;
    arc_observation_t hot = clean;
    hot.p_median = 38;
    hot.clip_permille = 40;
    for (unsigned i = 0; i < 100; ++i) {
        assert(arc_controller_tick(&arc, &weak) == 62);
        assert(arc_controller_tick(&arc, &hot) == 62);
    }
    assert(arc.state == ARC_LOCK);

    /* A continuous weak-sync run must progress; lost_ticks used to be reset
     * immediately before its increment, so this exit could never complete. */
    arc_observation_t poor_sync = clean;
    poor_sync.sync_quality = 30;
    for (unsigned i = 0; i < 10; ++i)
        arc_controller_tick(&arc, &poor_sync);
    assert(arc.state == ARC_ACQUIRE);

    puts("ARC PHY/controller tests passed");
    return 0;
}
