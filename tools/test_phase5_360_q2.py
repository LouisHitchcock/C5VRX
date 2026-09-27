#!/usr/bin/env python3
"""Check the generated two-stage program against an independent stream model."""

from pathlib import Path
import importlib.util
import random

import train_phase5_360_q2 as design

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("bs_model", ROOT / "legacy/c5vrx2/tools/bs_model.py")
model = importlib.util.module_from_spec(spec)
spec.loader.exec_module(model)


def code(previous_raw: int, middle_raw: int, current_raw: int, words: list[int]) -> int:
    previous = design.phase5(previous_raw)
    stage1 = words[design.stage1_address(previous, middle_raw, current_raw)]
    token = ((stage1 >> 6) & 3) | (((stage1 >> 13) & 7) << 2)
    return words[design.stage2_address(previous, token)] & 63


def main() -> None:
    asm = design.ASM.read_text(encoding="utf-8")
    words = design.parse_lut_words(asm)
    rng = random.Random(0x36050)
    raw = bytes(rng.randrange(256) for _ in range(16384))
    output = model.simulate(asm, raw, 16382)
    assert len(output) == 16382
    for pair in range(8191):
        expected = code(raw[2 * pair + 1], raw[2 * pair + 2],
                        raw[2 * pair + 3], words)
        assert output[2 * pair] == output[2 * pair + 1] == expected, pair
    print("8191 raw-IQ pairs: BS source model and trajectory LUT agree byte for byte")


if __name__ == "__main__":
    main()
