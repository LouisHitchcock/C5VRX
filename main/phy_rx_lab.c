/* C5VRX by Twotoz and contributors; pinned C5 PHY investigation #150-153.
 * No production overrides. Polling cannot prove absence of short gate pulses. */
#include "phy_rx_lab.h"
#include "rf.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include <inttypes.h>
#include <stdio.h>

#define REG(a) (*(volatile uint32_t *)(uintptr_t)(a))
#define PHYBIT(n) (UINT32_C(1) << (n))
#define EVENT_CAP 64u
#define PROFILE_US 10000000LL
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

typedef struct {
    uint32_t addr, baseline, current, changes;
    int64_t changed_us;
} invariant_t;
static invariant_t s_regs[] = {
    { .addr=0x600A0890 }, { .addr=0x600A9C18 },
    { .addr=0x600A7C3C }, { .addr=0x600A7C40 }, { .addr=0x600A7C08 },
    { .addr=0x600A0800 }, { .addr=0x600A0400 }, { .addr=0x600A7C80 },
    { .addr=0x600A7010 }, { .addr=0x600A7014 }, { .addr=0x600A7044 },
    { .addr=0x600A7108 }, { .addr=0x600A0814 }, { .addr=0x600A0874 },
    { .addr=0x600A0430 }, { .addr=0x600A0448 },
    { .addr=0x600A702C }, { .addr=0x600A7030 },
    { .addr=0x600A4400 }, { .addr=0x600A7CE0 }, { .addr=0x600A7CE4 },
    { .addr=0x600A7904 }, { .addr=0x600A7074 },
    { .addr=0x600A0438 }, { .addr=0x600A0C0C }, { .addr=0x600A043C },
    { .addr=0x600A0C38 }, { .addr=0x600A0C3C }, { .addr=0x600A7068 },
    { .addr=0x600A7018 }, { .addr=0x600A701C },
    { .addr=0x600A7C44 }, { .addr=0x600A7C50 }, { .addr=0x600A4C5C },
    /* Only slot zero is decoded here; do not infer arbitrary slot bounds. */
    { .addr=0x600A7C14 }, { .addr=0x600A7810 }, { .addr=0x600A7814 },
    { .addr=0x600A7830 },
};
#define NREG (sizeof(s_regs) / sizeof(s_regs[0]))
typedef struct {
    int64_t time_us;
    uint32_t generation, addr, old_value, new_value;
} event_t;
static event_t s_events[EVENT_CAP];
static uint32_t s_event_total, s_generation, s_depth;
static bool s_ready, s_monitor;
static uint32_t s_vendor[NREG];
static int64_t s_last_poll;
/* OSI fields are shared with Wi-Fi tasks. Protected by the same short lock. */
static uint32_t s_enables, s_disables;
static int64_t s_enable_us, s_disable_us;
static const char *s_reason = "boot";

typedef struct { uint32_t addr, mask, value; } patch_t;
typedef struct { const char *name; unsigned count; patch_t patches[3]; } profile_t;
/* All masks recovered from the pinned binary. Names describe writes, not
 * alleged bypass/sensitivity semantics. Never change ADC/filter tuples here. */
static const profile_t s_profiles[] = {
    {"STOCK", 0, {{0}}},
    {"PKDADC_BIT31_CLEAR", 1, {{0x600A0C38, PHYBIT(31), 0}}},
    {"NF_AUTO_FREEZE", 3, {{0x600A7018, PHYBIT(23)|PHYBIT(28), 0},
                           {0x600A7C44, PHYBIT(0), 0}, {0x600A7C50, PHYBIT(0), 0}}},
    {"BB_CCA_BIT30_CLEAR", 1, {{0x600A7018, PHYBIT(30), 0}}},
    /* phy_wifi_fbw_sel(0): multiple fields, not just one bit. */
    {"FRONT_BW_ARG0", 1, {{0x600A0874, 0x003F0000u, 0}}},
    /* phy_bb_bss_cbw40_dig(0) clears BOTH bits 2 and 3. */
    {"DIG_BW_ARG0", 1, {{0x600A9C18, PHYBIT(2)|PHYBIT(3), 0}}},
    {"CHAN_A_ARG0", 1, {{0x600A7904, PHYBIT(22)|7u, PHYBIT(22)}}},
    {"CHAN_A_ARG1", 1, {{0x600A7904, PHYBIT(22), 0}}},
    {"CHAN_B_ARG0", 1, {{0x600A7074, PHYBIT(13), PHYBIT(13)}}},
    {"CHAN_B_ARG1", 1, {{0x600A7074, PHYBIT(13), 0}}},
    {"SPUR_SLOT0_BIT13_CLEAR", 1, {{0x600A7C14, PHYBIT(13), 0}}},
};
static unsigned s_profile;
#ifdef C5VRX_PHY_RX_LAB_PINNED
static unsigned s_next_profile = 1;
#endif
static uint32_t s_saved[3];
static int64_t s_deadline;

static void record_locked(int64_t now, uint32_t addr, uint32_t old, uint32_t value)
{
    s_events[s_event_total % EVENT_CAP] = (event_t){now, s_generation, addr, old, value};
    ++s_event_total;
}

static void capture_locked(void)
{
    for (unsigned i=0; i<NREG; ++i) {
        s_regs[i].baseline = s_regs[i].current = REG(s_regs[i].addr);
    }
}

static void stock_locked(void)
{
    const profile_t *p = &s_profiles[s_profile];
    for (unsigned i=0; i<p->count; ++i) {
        patch_t x = p->patches[i];
        /* Restore exactly the owned fields. Do not replay unrelated volatile
         * status or a vendor update in another field of the same register. */
        REG(x.addr) = (REG(x.addr) & ~x.mask) | (s_saved[i] & x.mask);
    }
    s_profile = 0;
    s_deadline = 0;
}

void phy_rx_lab_osi_event(bool enable)
{
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_mux);
    if (enable) { ++s_enables; s_enable_us=now; }
    else { ++s_disables; s_disable_us=now; }
    if (s_ready) record_locked(now, enable ? 1u : 2u, s_depth,
                              enable ? s_enables : s_disables);
    portEXIT_CRITICAL(&s_mux);
}

void phy_rx_lab_begin(const char *reason)
{
    portENTER_CRITICAL(&s_mux);
    if (s_depth++ == 0) {
        stock_locked(); /* Never carry a previous-channel snapshot into a tune. */
        s_reason = reason;
        if (s_ready) record_locked(esp_timer_get_time(), 3, 0, 1);
    }
    portEXIT_CRITICAL(&s_mux);
}

void phy_rx_lab_capture_vendor(void)
{
    portENTER_CRITICAL(&s_mux);
    for (unsigned i=0; i<NREG; ++i) s_vendor[i]=REG(s_regs[i].addr);
    portEXIT_CRITICAL(&s_mux);
}

void phy_rx_lab_end(void)
{
    portENTER_CRITICAL(&s_mux);
    if (s_depth && --s_depth == 0) {
        ++s_generation;
        capture_locked();
        s_ready = true;
        record_locked(esp_timer_get_time(), 3, 1, 0);
    }
    portEXIT_CRITICAL(&s_mux);
}

uint32_t phy_rx_lab_generation(void)
{
    portENTER_CRITICAL(&s_mux);
    uint32_t generation=s_generation;
    portEXIT_CRITICAL(&s_mux);
    return generation;
}

bool phy_rx_lab_busy(void)
{
    portENTER_CRITICAL(&s_mux);
    bool busy=s_depth != 0;
    portEXIT_CRITICAL(&s_mux);
    return busy;
}

void phy_rx_lab_poll(void)
{
    int64_t now=esp_timer_get_time();
    portENTER_CRITICAL(&s_mux);
    if (s_ready && !s_depth && s_profile && now >= s_deadline) {
        stock_locked();
        ++s_generation;
        capture_locked();
        record_locked(now, 4, 1, 0); /* timeout, no UART in polling */
    }
    if (!s_ready || s_depth || !s_monitor || now-s_last_poll < 50000) {
        portEXIT_CRITICAL(&s_mux);
        return;
    }
    s_last_poll=now;
    for (unsigned i=0; i<NREG; ++i) {
        invariant_t *r=&s_regs[i];
        uint32_t value=REG(r->addr);
        if (value != r->current) {
            record_locked(now, r->addr, r->current, value);
            r->current=value;
            ++r->changes;
            r->changed_us=now;
        }
    }
    portEXIT_CRITICAL(&s_mux);
}

void phy_rx_lab_toggle_monitor(void)
{
    portENTER_CRITICAL(&s_mux);
    s_monitor=!s_monitor;
    bool enabled=s_monitor;
    if (s_ready && !s_depth) capture_locked();
    portEXIT_CRITICAL(&s_mux);
    printf("PHYINV monitor=%u poll_us=50000 ring=%u\n", enabled, EVENT_CAP);
}

void phy_rx_lab_stock(void)
{
    portENTER_CRITICAL(&s_mux);
    bool active=s_profile != 0;
    stock_locked();
    if (active) { ++s_generation; capture_locked(); }
    portEXIT_CRITICAL(&s_mux);
    printf("PHYLAB profile=STOCK restored=%u\n", active);
}

bool phy_rx_lab_profile_active(void)
{
    portENTER_CRITICAL(&s_mux);
    bool active=s_profile != 0;
    portEXIT_CRITICAL(&s_mux);
    return active;
}

void phy_rx_lab_next_profile(void)
{
#ifndef C5VRX_PHY_RX_LAB_PINNED
    printf("PHYLAB refused=unverified_PHY_binary read_only=1\n");
    return;
#else
    if (rf_native_agc_active()) {
        printf("PHYLAB refused=native_AGC_owner\n");
        return;
    }
    portENTER_CRITICAL(&s_mux);
    if (!s_ready || s_depth) {
        portEXIT_CRITICAL(&s_mux);
        printf("PHYLAB refused=transaction_active\n");
        return;
    }
    stock_locked();
    s_profile=s_next_profile++;
    if (s_next_profile >= sizeof(s_profiles)/sizeof(s_profiles[0])) s_next_profile=1;
    const profile_t *p=&s_profiles[s_profile];
    for (unsigned i=0; i<p->count; ++i) {
        patch_t x=p->patches[i];
        s_saved[i]=REG(x.addr);
        REG(x.addr)=(s_saved[i] & ~x.mask) | x.value;
    }
    __sync_synchronize();
    bool verified=true;
    for (unsigned i=0; i<p->count; ++i) {
        patch_t x=p->patches[i];
        if ((REG(x.addr)&x.mask) != x.value) verified=false;
    }
    const char *name=p->name;
    if (!verified) stock_locked();
    s_deadline=s_profile ? esp_timer_get_time()+PROFILE_US : 0;
    ++s_generation;
    capture_locked();
    record_locked(esp_timer_get_time(), 4, 0, s_profile);
    portEXIT_CRITICAL(&s_mux);
    printf("PHYLAB profile=%s verified=%u timeout_ms=10000 persistent=0\n", name, verified);
#endif
}

void phy_rx_lab_mark(void)
{
    phy_rx_lab_poll();
    portENTER_CRITICAL(&s_mux);
    int64_t now=esp_timer_get_time();
    record_locked(now, 5, 0, 0);
    uint32_t total=s_event_total;
    portEXIT_CRITICAL(&s_mux);
    printf("PHYMARK t=%" PRId64 " events=%" PRIu32 "\n", now, total);
    uint32_t start=total>EVENT_CAP ? total-EVENT_CAP : 0;
    for (uint32_t i=start; i<total; ++i) {
        portENTER_CRITICAL(&s_mux);
        event_t e=s_events[i%EVENT_CAP];
        portEXIT_CRITICAL(&s_mux);
        printf("PHYEV t=%" PRId64 " gen=%" PRIu32 " reg=%08" PRIx32
               " old=%08" PRIx32 " new=%08" PRIx32 "\n",
               e.time_us,e.generation,e.addr,e.old_value,e.new_value);
    }
}

extern unsigned char phy_param[];
extern uint8_t phy_i2c_readReg(uint8_t block, uint8_t host, uint8_t reg);
void phy_rx_lab_dump(bool analog_i2c)
{
    /* Copy under lock; print outside it. No logging in the RF timing path. */
    invariant_t snapshot[NREG];
    uint32_t vendor[NREG];
    portENTER_CRITICAL(&s_mux);
    bool ready=s_ready && !s_depth;
    if (ready) {
        for (unsigned i=0; i<NREG; ++i) {
            snapshot[i]=s_regs[i];
            vendor[i]=s_vendor[i];
            snapshot[i].current=REG(snapshot[i].addr);
        }
    }
    uint32_t generation=s_generation, en=s_enables, dis=s_disables;
    int64_t en_us=s_enable_us, dis_us=s_disable_us;
    const char *reason=s_reason, *profile=s_profiles[s_profile].name;
    portEXIT_CRITICAL(&s_mux);
    if (!ready) { printf("PHYINV unavailable=transaction_or_startup\n"); return; }
    printf("PHYINV gen=%" PRIu32 " reason=%s profile=%s native=%u bw=%u freq=%u offset=%d"
           " enable=%" PRIu32 " enable_us=%" PRId64 " disable=%" PRIu32 " disable_us=%" PRId64 "\n",
           generation,reason,profile,rf_native_agc_active(),rf_get_analog_bandwidth()?40:20,
           rf_get_frequency_mhz(),rf_get_frequency_offset_khz(),en,en_us,dis,dis_us);
    printf("PHYOWN direct_agc_disabled=%u expected=%u\n",
           (unsigned)((REG(0x600A7030) >> 29) & 1u), !rf_native_agc_active());
    for (unsigned i=0; i<NREG; ++i) {
        invariant_t r=snapshot[i];
        printf("PHYREG reg=%08" PRIx32 " vendor=%08" PRIx32 " lock=%08" PRIx32 " now=%08" PRIx32
               " changes=%" PRIu32 " last_us=%" PRId64 "\n",
               r.addr,vendor[i],r.baseline,r.current,r.changes,r.changed_us);
    }
#ifdef C5VRX_PHY_RX_LAB_PINNED
    if (analog_i2c) {
        /* Explicit lab-only vendor I2C reads; never poll these in LOCK. */
        for (unsigned i=0xF5; i<=0xFC; ++i)
            printf("PHYPARAM offset=%03x value=%02x\n",i,phy_param[i]);
        for (uint8_t reg=6; reg<=21; ++reg)
            printf("PHYI2C block=67 reg=%02x value=%02x\n",reg,
                   phy_i2c_readReg(0x67,1,reg));
    }
#else
    (void)analog_i2c;
#endif
}
