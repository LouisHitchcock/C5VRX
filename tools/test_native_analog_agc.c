#include <assert.h>
#include <stdio.h>
#include "native_analog_agc.h"

int main(void)
{
    native_analog_agc_t s = {0};
    uint32_t out = 0;
    assert(!native_analog_agc_begin(&s, 1, 384, 0, false, &out));
    assert(!native_analog_agc_begin(&s, 0, 384, 0, true, &out));
    assert(!native_analog_agc_begin(&s, 3, 384, 0, true, &out));
    assert(!native_analog_agc_begin(&s, 1, 416, 0, true, &out));
    assert(!s.active);
    for (unsigned profile = 1; profile <= 2; ++profile) {
        uint32_t baseline = 0xa8000180u;
        assert(native_analog_agc_begin(&s, profile, baseline, 1000, true, &out));
        assert((out & ~ANALOG_AGC_MASK) == (baseline & ~ANALOG_AGC_MASK));
        assert((out & ANALOG_AGC_MASK) == (profile == 1 ? 416u : 352u));
        assert(!native_analog_agc_begin(&s, profile, out, 2000, true, &out));
        uint32_t active = out;
        for (int64_t t = 1000; t < s.deadline_us; t += 50000) {
            uint32_t sentinel = 0xdeadbeefu;
            assert(!native_analog_agc_poll(&s, active, t, true, &sentinel));
            assert(sentinel == 0xdeadbeefu); /* Stable trial makes zero writes. */
        }
        /* Preserve another owner's unrelated bits on automatic expiry. */
        active ^= 0x08000000u;
        assert(native_analog_agc_poll(&s, active, s.deadline_us, true, &out));
        assert(out == (baseline ^ 0x08000000u));
        assert(!s.active);
        assert(!native_analog_agc_end(&s, out, &out));
    }
    assert(native_analog_agc_begin(&s, 1, 384, 0, true, &out));
    assert(native_analog_agc_poll(&s, out, 1, false, &out));
    assert(out == 384 && !s.active); /* Context change restores immediately. */
    assert(native_analog_agc_begin(&s, 2, 384, 0, true, &out));
    uint32_t sentinel = 0xdeadbeefu;
    assert(!native_analog_agc_poll(&s, 400, 1, true, &sentinel));
    assert(sentinel == 0xdeadbeefu && !s.active); /* Don't clobber vendor overwrite. */
    assert(native_analog_agc_begin(&s, 1, 384, 0, true, &out));
    assert(native_analog_agc_end(&s, out, &out));
    assert(out == 384 && !s.active); /* Manual restore. */
    puts("Native analog AGC: bounds, zero tracking writes, timeout/context rollback, ownership passed");
    return 0;
}
