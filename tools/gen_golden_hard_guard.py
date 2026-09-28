#!/usr/bin/env python3
"""Generate the two-bundle Golden pedestal guard experiment.

The current Golden LUT already emits pedestal 20 for |delta| >= 12. This
program preserves its DAC mapping and adds state freeze/reseed after such a
transition. Output is delayed by one 50 ns interval to let a LUT flag select
the following worker without adding a third bundle.
"""

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]
BASE = ROOT / "main/fm.bsasm"
TARGET = ROOT / "main/fm_golden_hard_guard.bsasm"
REJECT_BINS = 12


def wrap32(delta: int) -> int:
    return ((delta + 16) & 31) - 16


def build() -> str:
    match = re.search(r"(?m)^lut (.*)$", BASE.read_text(encoding="utf-8"))
    assert match is not None
    golden = [int(value) for value in match.group(1).split()]
    assert len(golden) == 1024
    words = []
    for address, word in enumerate(golden):
        previous, current = address >> 5, address & 31
        reject = abs(wrap32(current - previous)) >= REJECT_BINS
        if reject:
            assert (word & 63) == 20
        words.append(word | (int(reject) << 6))

    return """# Golden hard guard: exact Golden DAC for |delta| <= 11; pedestal 20
# and phase-history freeze for |delta| >= 12. A rejected endpoint is discarded.
# The next raw endpoint reseeds history while pedestal is emitted once more.
# One pair of A/B bundles reads 2 raw bytes and writes [D,D] every 50 ns.
# Pair LUT result controls the *next* worker; emitted DAC is one pair delayed.
cfg prefetch true
cfg eof_on downstream
cfg trailing_bytes 0
cfg lut_width_bits 16

lut """ + " ".join(map(str, words)) + """

controller_normal:
    # L is the previous pair result. Save its Golden DAC while addressing
    # the next raw endpoint. O26..O30 is the previous candidate phase.
    set 0..5 O0..O5,
    set 6..7 L,
    set 8..13 L0..L5,
    set 14..15 L,
    set 16..23 8..15,
    set 24..25 L,
    set 26..30 O26..O30,
    set 31 L,
    if L6 worker_bad

worker_good:
    # The previous candidate was accepted: emit exact Golden and form the
    # next pair using that candidate as the trusted previous phase.
    set 0..5 O8..O13,
    set 6..7 L,
    set 8..13 O8..O13,
    set 14..15 L,
    set 16..20 L8..L12,
    set 21..25 O26..O30,
    set 26..30 L8..L12,
    set 31 L,
    read 16,
    write 16,
    jmp controller_normal

worker_bad:
    # Reject the previous candidate. The new raw endpoint seeds history;
    # the dummy pair lookup is deliberately ignored by controller_reseed.
    set 0..1 L,
    set 2 H,
    set 3 L,
    set 4 H,
    set 5..7 L,
    set 8..9 L,
    set 10 H,
    set 11 L,
    set 12 H,
    set 13..15 L,
    set 16..20 L8..L12,
    set 21..25 L8..L12,
    set 26..30 L8..L12,
    set 31 L,
    read 16,
    write 16,
    jmp controller_reseed

controller_reseed:
    # Consume the dummy result and decode the following raw endpoint.
    set 0..5 O0..O5,
    set 6..7 L,
    set 8..13 O8..O13,
    set 14..15 L,
    set 16..23 8..15,
    set 24..25 L,
    set 26..30 O26..O30,
    set 31 L,
    jmp worker_reseed

worker_reseed:
    # Hold pedestal for the reseed interval and form the first new pair.
    set 0..1 L,
    set 2 H,
    set 3 L,
    set 4 H,
    set 5..7 L,
    set 8..9 L,
    set 10 H,
    set 11 L,
    set 12 H,
    set 13..15 L,
    set 16..20 L8..L12,
    set 21..25 O26..O30,
    set 26..30 L8..L12,
    set 31 L,
    read 16,
    write 16,
    jmp controller_normal
"""


if __name__ == "__main__":
    TARGET.write_text(build(), encoding="utf-8")
    print(f"wrote {TARGET}")
