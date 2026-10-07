#!/usr/bin/env python3
"""HC50: span50 Phase8 with a history-conditioned endpoint decoder.

Same program as fm_phase8_hr_live (two bundles per pair, endpoint delta
mod 256, DAC = top six bits); only the LUT differs. The decode lookup is
addressed by the raw endpoint byte plus two bank bits, which the program
takes from the low two bits of the retained minus term. HC50 makes those two
bits the QUADRANT of the previously decoded endpoint:

  * every endpoint phase is rounded to a multiple of 4 bins (Phase6), so the
    minus term's low two bits are free and the tag never carries into the
    DAC bits (plus has zero low bits);
  * bank b decodes raw r with the previous quadrant b: an origin cell
    (I, Q in {-1, 0}) takes the angle 4 bins inside its own quadrant edge
    that faces quadrant b when b is adjacent, else its cell centre.

Host study: tools in v4/tools/detector_study (hw50.py): ~10 % fewer clicks and
+0.1..0.2 dB SINAD near threshold versus the plain span50 program.
"""
from pathlib import Path
import random
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
from gen_phase8_hr_live import build as build_live  # noqa: E402
from gen_phase8_hr import phase8  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
TARGET = ROOT / "main/fm_hc50.bsasm"
BIAS = 128


def cell(raw):
    i, q = raw >> 4, raw & 15
    return (i - 16 if i > 7 else i), (q - 16 if q > 7 else q)


def decode(raw, prev_quadrant):
    p = (round(phase8(raw) / 4) * 4) & 255
    i, q = cell(raw)
    if i in (-1, 0) and q in (-1, 0):
        cq = p // 64
        dq = (prev_quadrant - cq) % 4
        if dq == 1:
            p = cq * 64 + 60
        elif dq == 3:
            p = cq * 64 + 4
    return p


def word(raw, bank):
    p = decode(raw, bank)
    minus = ((BIAS - p) & 0xFC) | (p // 64)
    return minus | (p << 8)


def build():
    import re
    source = build_live()
    words = [word(raw, bank) for bank in range(4) for raw in range(256)]
    source = re.sub(r"(?m)^lut .*$", "lut " + " ".join(map(str, words)), source)
    source = source.replace("# Full signed Phase8 delta mapped across all 64 DAC levels.",
                            "# HC50: span50 Phase8, history-conditioned endpoint decode (bank =\n"
                            "# previous quadrant, tag in the minus term's low bits), 64 DAC levels.")
    return source


def reference(stream):
    """Expected DAC code per pair: endpoint delta of the HC decode, mod 256."""
    ends = stream[1::2]
    out, prev_q, prev_p = [], 0, None
    for raw in ends:
        p = decode(raw, prev_q)
        if prev_p is not None:
            out.append((((BIAS - prev_p) & 0xFC) + p) % 256 >> 2)
        prev_q, prev_p = p // 64, p
    return out


if __name__ == "__main__":
    TARGET.write_text(build(), encoding="utf-8")
    print(f"wrote {TARGET}")
