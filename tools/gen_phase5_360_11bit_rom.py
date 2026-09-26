#!/usr/bin/env python3
"""Generate an 11-bit (P5,C5,winding1) DAC ROM for external logic.

This is an FPGA-side reference, not a C5-only live mode. The winding bit must
be calculated from all three decoded Phase5 samples before the ROM address.
"""

import argparse
from pathlib import Path

from prove_golden360_capacity import GOLDEN, interval_winding, winding

ROOT = Path(__file__).resolve().parents[1]
ROM_DIR = ROOT / "hardware/phase5_360"
PHASE_PATH = ROM_DIR / "phase5_256x5.mem"
DAC_PATH = ROM_DIR / "dac_2048x6.mem"


def roms():
    phase = [(GOLDEN[raw] >> 8) & 31 for raw in range(256)]
    dac = []
    for flag in range(2):
        for previous in range(32):
            for current in range(32):
                golden = GOLDEN[(previous << 5) | current] & 63
                # Nonzero winding sign follows the endpoint shortest arc.
                correction = 63 if ((current - previous) & 31) >= 16 else 0
                dac.append(correction if flag else golden)
    assert len(phase) == 256 and len(dac) == 2048
    for previous in range(32):
        for middle in range(32):
            for current in range(32):
                k = winding(previous, middle, current)
                assert interval_winding(previous, middle, current) == k
                addr = ((int(k != 0) << 10) | (previous << 5) | current)
                expected = (GOLDEN[(previous << 5) | current] & 63) if not k else (63 if k > 0 else 0)
                assert dac[addr] == expected
    return phase, dac


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true", help="reject stale checked-in ROMs")
    args = parser.parse_args()
    phase, dac = roms()
    outputs = ((PHASE_PATH, "".join(f"{value:02x}\n" for value in phase)),
               (DAC_PATH, "".join(f"{value:02x}\n" for value in dac)))
    if not args.check:
        ROM_DIR.mkdir(parents=True, exist_ok=True)
    for path, content in outputs:
        if args.check:
            assert path.read_text(encoding="ascii") == content, f"stale {path}"
        else:
            path.write_text(content, encoding="ascii")
    print("11-bit FPGA ROM: 2048 DAC entries, 256 raw phase entries;"
          " 32768/32768 exact Phase5 triples")


if __name__ == "__main__":
    main()
