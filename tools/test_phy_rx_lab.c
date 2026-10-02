#define _GNU_SOURCE
#include <assert.h>
#include <sys/mman.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
static int64_t clock_us;
static bool native;
unsigned char phy_param[0x500];
int64_t esp_timer_get_time(void) { return clock_us; }
bool rf_native_agc_active(void) { return native; }
bool rf_get_analog_bandwidth(void) { return true; }
uint16_t rf_get_frequency_mhz(void) { return 5865; }
int rf_get_frequency_offset_khz(void) { return 0; }
uint8_t phy_i2c_readReg(uint8_t b, uint8_t h, uint8_t r)
{ assert(b==0x67 && h==1); return r; }
#include "../main/phy_rx_lab.c"
int main(void)
{
    void *memory=mmap((void *)0x600A0000, 0x10000, PROT_READ|PROT_WRITE,
                      MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED, -1, 0);
    assert(memory != MAP_FAILED);
    memset(memory, 0xA5, 0x10000);
    phy_rx_lab_osi_event(true);
    phy_rx_lab_begin("boot");
    phy_rx_lab_capture_vendor();
    phy_rx_lab_end();
    assert(s_enables==1 && s_disables==0 && s_generation==1);
    uint32_t baseline=REG(0x600A0890);
    REG(0x600A0890)^=1;
    clock_us+=50000;
    phy_rx_lab_poll();
    assert(s_regs[0].changes==0); /* monitoring off by default */
    phy_rx_lab_toggle_monitor();
    REG(0x600A0890)=baseline;
    clock_us+=50000;
    phy_rx_lab_poll();
    assert(s_regs[0].changes==1);
#ifdef C5VRX_PHY_RX_LAB_PINNED
    for (unsigned n=1; n<sizeof(s_profiles)/sizeof(s_profiles[0]); ++n) {
        memset(memory, 0xA5, 0x10000);
        phy_rx_lab_next_profile();
        assert(s_profile==n);
        const profile_t *p=&s_profiles[n];
        for (unsigned i=0; i<p->count; ++i) {
            patch_t x=p->patches[i];
            assert((REG(x.addr)&x.mask)==x.value);
            /* An unrelated field changes while the lab owns its mask. */
            uint32_t outside=(~x.mask)&(x.mask+1u);
            if (!outside) outside=(~x.mask)&(0u-~x.mask);
            REG(x.addr)^=outside;
            s_saved[i]^=outside;
        }
        if (n%3==0) { clock_us+=PROFILE_US; phy_rx_lab_poll(); }
        else if (n%3==1) { phy_rx_lab_begin("retune"); phy_rx_lab_end(); }
        else phy_rx_lab_stock();
        assert(!s_profile);
        for (unsigned i=0; i<p->count; ++i) assert(REG(p->patches[i].addr)==s_saved[i]);
    }
    native=true;
    phy_rx_lab_next_profile();
    assert(!s_profile);
    native=false;
    phy_rx_lab_begin("outer");
    uint32_t generation=s_generation;
    phy_rx_lab_begin("inner");
    phy_rx_lab_next_profile();
    assert(!s_profile);
    phy_rx_lab_end();
    assert(s_generation==generation && phy_rx_lab_busy());
    phy_rx_lab_end();
    assert(s_generation==generation+1 && !phy_rx_lab_busy());
#else
    phy_rx_lab_next_profile();
    assert(!s_profile);
#endif
    for (unsigned i=0; i<100; ++i) { ++clock_us; phy_rx_lab_osi_event(false); }
    assert(s_disables==100);
    assert(s_event_total>EVENT_CAP);
    phy_rx_lab_mark();
    munmap(memory,0x10000);
    puts("PHY RX lab lifecycle tests passed");
}
