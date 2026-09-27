#!/usr/bin/env python3
"""Independent deterministic comparison of the Q2 LUT with live Phase5."""

import math
import random

import range_demod_bench as reference
import test_phase5_360_q2 as candidate


def scene(name, count, amplitude, sigma, seed):
    rng = random.Random(seed)
    phase = 0.0
    raw = bytearray()
    steps = []
    for index in range(count):
        step = 0.42 * math.sin(reference.TAU * index / 173.0)
        step += 0.08 * math.sin(reference.TAU * index / 41.0)
        if name == "weak":
            step = (0.72 * math.sin(reference.TAU * index / 71.0)
                    + 0.34 * math.sin(reference.TAU * index / 19.0))
        if name == "edges" and index % 251 in (117, 118):
            step = 2.25 if (index // 251) % 2 == 0 else -2.25
        phase += step
        i = amplitude * math.cos(phase) + rng.gauss(0, sigma)
        q = amplitude * math.sin(phase) + rng.gauss(0, sigma)
        ii = max(-8, min(7, round(i))) & 15
        qq = max(-8, min(7, round(q))) & 15
        raw.append((ii << 4) | qq)
        steps.append(step)
    return raw, steps


def measure(name, count, amplitude, sigma, seed):
    raw, steps = scene(name, count, amplitude, sigma, seed)
    words = candidate.design.parse_lut_words(candidate.design.ASM.read_text())
    total_g = total_c = hard_g = hard_c = 0
    winding = wing_g = wing_c = 0
    pairs = 0
    for end in range(3, len(raw), 2):
        p, m, c = raw[end - 2:end + 1]
        target = reference.map_pair_sum_rad(steps[end - 1] + steps[end])
        golden = reference.golden_code(p, c)
        code = candidate.code(p, m, c, words)
        ge, ce = abs(golden - target), abs(code - target)
        total_g += ge
        total_c += ce
        hard_g += ge >= 16
        hard_c += ce >= 16
        if abs(steps[end - 1] + steps[end]) > math.pi:
            winding += 1
            wing_g += ge
            wing_c += ce
        pairs += 1
    result = {
        "scene": name, "pairs": pairs,
        "golden_mae": round(total_g / pairs, 3),
        "q2_mae": round(total_c / pairs, 3),
        "golden_hard": hard_g, "q2_hard": hard_c,
        "true_winding": winding,
        "golden_winding_mae": round(wing_g / max(1, winding), 3),
        "q2_winding_mae": round(wing_c / max(1, winding), 3),
    }
    print(result)
    return result


if __name__ == "__main__":
    measure("clean", 20000, 6.0, 0.35, 0x3601)
    measure("weak", 30000, 4.8, 0.60, 0x3602)
    measure("edges", 30000, 6.0, 0.35, 0x3603)
