#!/usr/bin/env python3
"""Generate a two-bundle experimental rail HOLD/RESEED demodulator.

Uses the VIDEO32 codebook and transfer for a direct unguarded comparison.
This is an IQ rail-risk detector, NOT a decoded AGC acquisition flag.
No phase correction is guessed; calibrated correction needs aligned metadata.
"""
from pathlib import Path
from gen_phase8_video import build as video_build

ROOT = Path(__file__).resolve().parents[1]
TARGET = ROOT / "main/fm_agc_guard.bsasm"


def rail_risk(raw):
    # Coarse Q4/I4 only. These outer quantizer cells may contain ADC clipping;
    # they are not proof of it. Do not reject central cells or picture edges.
    return (raw & 15) in (7, 8) or (raw >> 4) in (7, 8)


def build():
    _, _, states, _, codes = video_build()
    words = [states[a & 255] | (int(rail_risk(a & 255)) << 6) |
             (codes[a] << 8) for a in range(1024)]
    source = """# Experimental coarse-IQ rail HOLD/RESEED, not AGC-state detection.
# VIDEO32 transfer/codebook, 40 MS/s input, 20 MS/s unique duplicated DAC.
# Every path consumes two instruction bundles per output pair.
# O26..30: previous phase; O0..5/O8..13: last emitted DAC.
# Raw LUT: bits0..4 phase, bit6 rail risk. Pair LUT: bits8..13 DAC.
# DROP enters SEED; SEED holds the first clean endpoint, then TRACK resumes.
# State continues across DMA boundaries. Zero guessed phase compensation.
cfg prefetch true
cfg eof_on downstream
cfg trailing_bytes 0
cfg lut_width_bits 16
lut """ + " ".join(map(str, words)) + "\n\n"
    controller = """    set 0..5 O0..O5,
    set 8..13 O8..O13,
    set 16..20 L0..L4,
    set 21..25 O26..O30,
    set 26..30 L0..L4,
    if L6 drop

"""
    emit = """    set 0..5 {dac},
    set 8..13 {dac},
    set 16..23 8..15,
    set 26..30 O26..O30,
    read 16,
    write 16,
    jmp {next}

"""
    source += "track:\n" + controller
    source += "emit:\n" + emit.format(dac="L8..L13", next="track")
    source += "drop:\n" + emit.format(dac="O0..O5", next="reseed")
    source += "reseed:\n" + controller
    source += "seed:\n" + emit.format(dac="O0..O5", next="track")
    return source


if __name__ == "__main__":
    TARGET.write_text(build(), encoding="utf-8")
    print(f"Generated {TARGET.name}: experimental rail HOLD/RESEED")
