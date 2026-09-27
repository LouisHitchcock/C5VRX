#!/usr/bin/env python3
"""Source-level timing model for the three-stage external Phase5-360 pipeline.

This does not simulate or synthesize the Verilog. It checks the exact output
sequence implied by its registers across continuous raw IQ pairs.
"""

from gen_phase5_360_11bit_rom import roms
from prove_golden360_capacity import GOLDEN, interval_winding


def expected_dac(previous, middle, current):
    k = interval_winding(previous, middle, current)
    return (GOLDEN[(previous << 5) | current] & 63) if not k else (63 if k > 0 else 0)


def run():
    phase_rom, dac_rom = roms()
    # Exhaust the RTL's unsigned five-bit predicates independently of the
    # generator's signed wrap32 interval function.
    for p in range(32):
        for m in range(32):
            for c in range(32):
                u = (m - p) & 31
                e = (c - p) & 31
                rtl_flag = ((e < 16 and u >= 16 and u <= 16 + e)
                            or (e >= 16 and u < 16 and u > e - 16))
                k = interval_winding(p, m, c)
                assert rtl_flag == (k != 0), (p, m, c)
                assert dac_rom[(int(rtl_flag) << 10) | (p << 5) | c] == expected_dac(p, m, c)
    state = 0xA5D13E97
    raw_pairs = []
    for _ in range(18000):
        state = (1664525 * state + 1013904223) & 0xffffffff
        middle = (state >> 16) & 255
        state = (1664525 * state + 1013904223) & 0xffffffff
        current = (state >> 16) & 255
        raw_pairs.append((middle, current))

    # Mirrors the RTL's first/second edges: phase decode -> P/M/C registers,
    # interval/ROM-address register -> DAC register on the following pair.
    previous = None
    triplet = None
    address = None
    observed = []
    expected = []
    for middle_raw, current_raw in raw_pairs:
        middle = phase_rom[middle_raw]
        if triplet is None:
            address = None
        else:
            p, m, c = triplet
            address = ((int(interval_winding(p, m, c) != 0) << 10)
                       | (p << 5) | c)
        current = phase_rom[current_raw]
        observed.append(20 if address is None else dac_rom[address])
        triplet = None if previous is None else (previous, middle, current)
        previous = current
        expected.append(None if triplet is None else expected_dac(*triplet))

    assert observed[:2] == [20, 20]
    assert all(observed[index] == expected[index - 1]
               for index in range(2, len(observed)))
    assert all(observed[index] == expected[index - 1]
               for index in (2048, 4096, 8192, 16384))
    print(f"Three-stage direct path: {len(observed)-2} continuous pair outputs"
          " match the exact oracle with one-pair latency")


if __name__ == "__main__":
    run()
