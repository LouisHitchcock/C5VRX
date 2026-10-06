/* C5VRX by Twotoz and contributors: vendor RX DC/IQ calibration at the
 * actual receive frequency (C5VRX-4 lab, '~').
 *
 * Adapted from ESPARGOS esp-sdr main/common/rx_recalibration.c (ESP32-C5
 * branch), commit 06a5ca4027d28626c8e8a8b38176814dfe774a1c, GPL-3.0,
 * https://github.com/ESPARGOS/esp-sdr - credit to the ESPARGOS authors.
 * Changes for C5VRX: re-verified against C5VRX's pinned PHY (esp-phy-lib
 * 59c1234, libphy.a SHA256 dbf33c41...104fffb) instead of ESPARGOS's newer
 * pin; only the flag bits that phy_bb_init() of this binary tests are
 * cleared (0x80 RX DC, 0x400 RX IQ; ESPARGOS also clears 0x200, which this
 * binary does not use for RX calibration); wrapped in C5VRX's lab
 * transaction and restore by the caller.
 *
 * phy_bb_init() of this binary calibrates 5 GHz RX DC with
 * phy_set_rx_gain_cal_dc(0, 1, phy_param+384, phy_param+912) at internal
 * reference channels (5210..5855 MHz) and RX IQ with
 * phy_set_rx_gain_cal_iq(0, 5520, phy_param+222, 1, 0, 0). Wrapping
 * phy_chip_set_chan_ana() for the duration of this synchronous call keeps
 * every internal reference tune on the requested frequency, so DC and IQ
 * are measured where the receiver actually listens (e.g. 5865/5917 MHz,
 * above the vendor's last 5855 MHz point). phy_set_rx_gain_table() then
 * reinstalls the gain memory with the fresh corrections. */
#include "rx_recal.h"

#include <stdint.h>
#include "soc/soc.h"

#ifdef C5VRX_PHY_RX_LAB_PINNED
extern unsigned char phy_param[];
static volatile unsigned s_measurement_mhz;

extern void __real_phy_chip_set_chan_ana(unsigned mhz);
void __wrap_phy_chip_set_chan_ana(unsigned mhz)
{
    __real_phy_chip_set_chan_ana(s_measurement_mhz ? s_measurement_mhz : mhz);
}

extern void phy_set_rx_gain_cal_dc(unsigned, unsigned, void *, void *);
extern void phy_set_rx_gain_cal_iq(unsigned, unsigned, void *, unsigned, unsigned, unsigned);
extern void phy_adc_rate_cal_rxdc(void);
extern void phy_set_rx_gain_table(unsigned, unsigned);

#define PHY_PARAM_BAND5   42u
#define PHY_PARAM_FLAGS   148u
#define FLAG_RX_DC        0x080u
#define FLAG_RX_IQ        0x400u

bool rx_recal_supported(void) { return true; }

void rx_recal_run(unsigned mhz)
{
    s_measurement_mhz = mhz;
    /* Manual gain (0x600A702C bit 23) must not override the vendor's sweep. */
    REG_CLR_BIT(0x600a702cu, 1u << 23);
    __real_phy_chip_set_chan_ana(mhz);
    unsigned band5 = phy_param[PHY_PARAM_BAND5];
    volatile uint32_t *flags = (volatile uint32_t *)(void *)(phy_param + PHY_PARAM_FLAGS);
    *flags &= ~(FLAG_RX_DC | FLAG_RX_IQ);
    if (band5) {
        phy_set_rx_gain_cal_dc(0, 1, phy_param + 384, phy_param + 912);
        phy_set_rx_gain_cal_iq(0, mhz, phy_param + 222, 1, 0, 0);
    } else {
        phy_set_rx_gain_cal_dc(0, 1, phy_param + 312, phy_param + 896);
        phy_adc_rate_cal_rxdc();
        phy_set_rx_gain_cal_dc(1, 1, phy_param + 348, phy_param + 904);
        phy_set_rx_gain_cal_iq(0, mhz, phy_param + 172, 0, 0, 0);
    }
    *flags |= FLAG_RX_DC | FLAG_RX_IQ;
    phy_set_rx_gain_table(mhz, 0);
    s_measurement_mhz = 0;
}
#else
bool rx_recal_supported(void) { return false; }
void rx_recal_run(unsigned mhz) { (void)mhz; }
#endif
