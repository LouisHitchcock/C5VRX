/* C5VRX by Twotoz and the C5VRX contributors: the ultrafine lane's 4-bit
 * token of one IQ component, for the host models (test_agc_compare.c,
 * test_keel_track.c, test_cn_auto.c).
 *
 * MODEM_DIAG keeps ADC bits {9,6,5,4} of the 10-bit code (cells = code / 16):
 * the sign is the ADC's own and never folds, the magnitude folds modulo 8
 * cells (board, PR #228: +288 codes reads +32), and the ADC saturates at
 * +-32 cells. Same as research/iq_lane_fusion.py component_code(adc10(x)).
 * The host models used to wrap all four bits ((c + 8) & 15) - 8, which turns
 * +9 cells into -7 (the far rail) where the board reads +1 (near zero): a
 * folded window read large in the models and small on the board. */
#pragma once
#include <math.h>

static inline int adc_fold4(double cells)
{
    int code = (int)floor(cells * 16.0);
    if (code > 511) code = 511;
    if (code < -512) code = -512;
    const int mag = ((code & 0x3ff) >> 4) & 7;   /* bits 6..4 */
    return code < 0 ? mag - 8 : mag;             /* bit 9: the sign */
}
