/* C5VRX-5 by Twotoz and the C5VRX contributors: per-channel DC tables in NVS
 * (pure logic, host test test/test_dco_slots.c).
 *
 * The DC DAC codes are measured per gain row for one tuning. One stored
 * table meant that every channel change was followed by a new DC search at
 * the next boot. Tables are now kept per frequency in DCO_SLOTS fixed NVS
 * blobs ("dco0".."dco5", bounded NVS use, no erase needed) with a small
 * index of their frequencies and use order. A frequency without its own
 * table starts from the nearest stored one: the baseband offset per gain
 * row changes little across the band (calibration_dc.c), and the live drift
 * nudges trim the rest. The first measured table is thus the template for
 * every channel. Per row, the index remembers which rows of a channel still
 * come from a template; a measurement on the own channel (VTX off at boot,
 * CALIBRATE, or on the carrier) replaces exactly those rows. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define DCO_SLOTS          6u
/* 3: tables per (frequency, filter): the codes depend on the analog
 * filter (2026-10-19), and the profiles use different filters. */
#define DCO_INDEX_VERSION  3u
#define DCO_MASK_ROWS      96u   /* >= ARC_VENDOR_GAIN_MAX + 1 gain rows */
#define DCO_MASK_BYTES     (DCO_MASK_ROWS / 8u)

typedef uint8_t dco_mask_t[DCO_MASK_BYTES];

typedef struct {
    uint16_t version;
    uint16_t freq[DCO_SLOTS];   /* MHz, 0 = empty slot */
    uint16_t seq[DCO_SLOTS];    /* use order, higher = more recent */
    uint16_t next_seq;
    dco_mask_t templ[DCO_SLOTS]; /* rows still taken from another channel */
    uint16_t filt[DCO_SLOTS];   /* dco_filter_key() of the slot's table */
} dco_index_t;

/* Version 2 layout, for the one-time upgrade (calibration_dc.c). */
typedef struct {
    uint16_t version;
    uint16_t freq[DCO_SLOTS], seq[DCO_SLOTS], next_seq;
    dco_mask_t templ[DCO_SLOTS];
} dco_index_v2_t;

/* One number per physical filter setting (code -1..63, skirt, BW20). */
static inline uint16_t dco_filter_key(int code, int skirt, unsigned bw20)
{
    return (uint16_t)((bw20 ? 0x8000u : 0u) | (((unsigned)(code + 1) & 0x7fu) << 8) | ((unsigned)skirt & 0xffu));
}

static inline bool dco_mask_get(const uint8_t *m, unsigned row)
{
    return row < DCO_MASK_ROWS && (m[row / 8u] >> (row % 8u)) & 1u;
}
static inline void dco_mask_put(uint8_t *m, unsigned row, bool on)
{
    if (row >= DCO_MASK_ROWS) return;
    if (on) m[row / 8u] |= (uint8_t)(1u << (row % 8u));
    else m[row / 8u] &= (uint8_t)~(1u << (row % 8u));
}
static inline unsigned dco_mask_count(const uint8_t *m, unsigned lo, unsigned hi)
{
    unsigned n = 0;
    for (unsigned r = lo; r <= hi && r < DCO_MASK_ROWS; ++r) n += dco_mask_get(m, r);
    return n;
}

static inline void dco_index_init(dco_index_t *x)
{
    *x = (dco_index_t){.version = DCO_INDEX_VERSION, .next_seq = 1u};
}

static inline int dco_slot_of(const dco_index_t *x, uint16_t freq, uint16_t key)
{
    for (unsigned k = 0; k < DCO_SLOTS; ++k)
        if (freq && x->freq[k] == freq && x->filt[k] == key) return (int)k;
    return -1;
}

/* Slot to store `freq` in: its own slot, else an empty one, else the least
 * recently used. */
static inline unsigned dco_slot_for_store(const dco_index_t *x, uint16_t freq, uint16_t key)
{
    int own = dco_slot_of(x, freq, key);
    if (own >= 0) return (unsigned)own;
    /* Evict the table with the most template rows first, then the oldest:
     * a table measured on its own channel is worth more than one that is
     * still mostly another channel's (board, PR #228: tables of channels
     * merely passed through pushed the measured 5805 MHz table out). */
    unsigned best = 0, best_t = 0;
    for (unsigned k = 0; k < DCO_SLOTS; ++k) {
        if (!x->freq[k]) return k;
        const unsigned t = dco_mask_count(x->templ[k], 0, DCO_MASK_ROWS - 1u);
        if (k == 0 || t > best_t || (t == best_t && x->seq[k] < x->seq[best])) { best = k; best_t = t; }
    }
    return best;
}

/* Template for (freq, key) without its own table: the table whose codes
 * are expected closest, -1 when nothing is stored. The DC codes move with
 * the frequency (board, PR #228: G83 225/186 at 5805 MHz, 144/279 at 5925,
 * ~0.7 codes a MHz) more than with the filter (15-40 codes between the
 * filters of one channel), so the cost is DCO_COST_PER_MHZ_X10 / 10 a MHz,
 * DCO_COST_FILTER for another filter and DCO_COST_TEMPLATE for a table
 * that still holds template rows; ties: the more recent. Before, a table of
 * the same filter 120 MHz away won over the own channel's other filter. A
 * template is only a starting point: its rows stay template rows. */
#define DCO_COST_PER_MHZ_X10 7u
#define DCO_COST_FILTER      30u
#define DCO_COST_TEMPLATE    20u
static inline int dco_nearest_slot(const dco_index_t *x, uint16_t freq, uint16_t key)
{
    int best = -1;
    unsigned best_c = 0;
    for (unsigned k = 0; k < DCO_SLOTS; ++k) {
        if (!x->freq[k] || (x->freq[k] == freq && x->filt[k] == key)) continue;
        const unsigned d = x->freq[k] > freq ? (unsigned)(x->freq[k] - freq) : (unsigned)(freq - x->freq[k]);
        const unsigned c = d * DCO_COST_PER_MHZ_X10 / 10u + (x->filt[k] != key ? DCO_COST_FILTER : 0u) +
                           (dco_mask_count(x->templ[k], 0, DCO_MASK_ROWS - 1u) ? DCO_COST_TEMPLATE : 0u);
        if (best < 0 || c < best_c || (c == best_c && x->seq[k] > x->seq[best])) { best = (int)k; best_c = c; }
    }
    return best;
}

static inline void dco_index_touch(dco_index_t *x, unsigned slot, uint16_t freq, uint16_t key)
{
    if (slot >= DCO_SLOTS) return;
    if (x->next_seq == UINT16_MAX) {            /* renumber, keeping the order */
        for (unsigned k = 0; k < DCO_SLOTS; ++k) x->seq[k] = x->freq[k] ? (uint16_t)(x->seq[k] / 2u) : 0u;
        x->next_seq = UINT16_MAX / 2u + 1u;
    }
    x->freq[slot] = freq;
    x->filt[slot] = key;
    x->seq[slot] = x->next_seq++;
}

/* The DC DAC codes are only valid for the analog filter they were measured
 * with: board 2026-10-19 (A1, G71, no carrier) held a table measured with
 * BW40 / calibrated code (-1) under the live BW20 / code 16 / skirt 32
 * normal filter and kept a DC of (-0.9, -3.6) cells against 0.68 cells of
 * receiver noise per axis, which also parked Direct Gain at G71. A table
 * from another filter is re-measured at startup like a manual CALIBRATE of
 * the DC rows (calibration_dc.c dco_startup). */
static inline bool dco_filter_same(int table_code, int table_skirt, unsigned table_bw20, int code, int skirt,
                                   unsigned bw20)
{
    return table_code == code && table_skirt == skirt && (table_bw20 != 0u) == (bw20 != 0u);
}

/* Startup must re-measure every row: a table whose rows exist but whose
 * filter differs from the live one. An empty table (no rows) is the normal
 * missing-row path. */
static inline bool dco_filter_stale(unsigned valid_rows, bool filter_same)
{
    return valid_rows != 0u && !filter_same;
}
