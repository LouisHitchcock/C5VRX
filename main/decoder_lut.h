#pragma once
#include <stdbool.h>
#include <stdint.h>

/* C5VRX-3 digital IQ DC recentring of the Phase8 FULL decoder table
 * (fm_phase8_hr_live.bsasm, 1024x16 TX BitScrambler LUT). The receiver's
 * I/Q centre is subtracted inside the decode geometry: every raw byte's
 * Phase8 term is recomputed around the measured centre. No PHY write, no raw
 * ring change, no extra realtime stage. Ported from C5VRX-4 (#166).
 *
 * The offset is kept in lane-0 milli-steps (an ADC-code quantity) and the
 * table is written for the current IQ lane, so a lane change only needs a
 * rewrite, not a new measurement. */

/* Engine stopped (after load_program, before start). Verifies the loaded
 * table is the pristine Phase8 FULL table, proves host LUT access with a
 * differential probe and re-applies the current offset. Returns false when
 * the probe may have left the table modified: reload the program. */
bool decoder_lut_prepare(bool phase8_full, unsigned lane);
/* Live: write the table for this centre (lane-0 milli-steps) and lane.
 * (0, 0) restores the pristine table. False: not ready or a read-back
 * mismatch (then the table is blocked until the next prepare). */
bool decoder_lut_set(int lane0_i, int lane0_q, unsigned lane);
/* Before the BitScrambler is disabled or another owner takes TX. */
void decoder_lut_stop(void);
bool decoder_lut_ready(void);
void decoder_lut_applied(int lane0[2], unsigned *lane);
void decoder_lut_print(void);
