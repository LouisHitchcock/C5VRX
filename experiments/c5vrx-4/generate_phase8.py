#!/usr/bin/env python3
"""Generate three-bundle Phase8 programs; does not run tests or simulations."""
from pathlib import Path
import math

HERE = Path(__file__).resolve().parent
SPAN_S = 75e-9
FREQUENCY_BOUND_HZ = 6e6
RADII = (0.5, 0.75, 1.0, 1.5, 2.0, 2.5)
NOISE_SIGMAS = (0.56, 0.95)
MAX_CORRECTION_BINS = 8


def signed(n):
    return n if n < 8 else n - 16


def phase8(raw):
    i = signed(raw >> 4) + 31.5 / 64
    q = signed(raw & 15) + 31.5 / 64
    return round(math.atan2(q, i) * 128 / math.pi) & 255


def wrap(value):
    return (value + 128) % 256 - 128


def prior(bank):
    # The bank is bit 7 of the retained (-phase) byte. It describes only
    # a semicircle, not the full previous angle. Uniform frequency increments
    # include CFO and deviation up to +/-6 MHz; no picture content is learned.
    previous = [p for p in range(256) if ((-p) & 255) >> 7 == bank]
    bound = math.floor(FREQUENCY_BOUND_HZ * SPAN_S * 256)
    return [sum(abs(wrap(p - old)) <= bound for old in previous)
            for p in range(256)]


def gaussian_cell(lo, mean, sigma):
    scale = math.sqrt(2) * sigma
    return (math.erf((lo + 1 - mean) / scale) -
            math.erf((lo - mean) / scale)) * 0.5


def decoder(history):
    tables = []
    for bank in range(2):
        weights = prior(bank)
        phases = []
        for raw in range(256):
            base = phase8(raw)
            i, q = signed(raw >> 4), signed(raw & 15)
            radius2 = (i + 31.5 / 64) ** 2 + (q + 31.5 / 64) ** 2
            if not history or radius2 > 2.6:
                phases.append(base)
                continue
            x = y = 0.0
            for p in range(256):
                angle = p * math.pi / 128
                c, s = math.cos(angle), math.sin(angle)
                likelihood = sum(
                    gaussian_cell(i, radius * c, sigma) *
                    gaussian_cell(q, radius * s, sigma)
                    for radius in RADII for sigma in NOISE_SIGMAS)
                weight = likelihood * weights[p]
                x += weight * c
                y += weight * s
            estimate = round(math.atan2(y, x) * 128 / math.pi) & 255
            correction = max(-MAX_CORRECTION_BINS,
                             min(MAX_CORRECTION_BINS, wrap(estimate - base)))
            phases.append((base + correction) & 255)
        tables.append(phases)
    return tables


def build(history):
    phases = decoder(history)
    resistance = (8200, 3900, 2000, 1000, 470, 240)
    voltage = [sum(1 / r for bit, r in enumerate(resistance)
                   if code & (1 << bit)) for code in range(64)]
    dac = [min(range(64), key=lambda code:
               abs(voltage[code] - voltage[63] * delta / 255))
           for delta in range(256)]
    words = []
    for bank in range(2):
        for index in range(256):
            p = phases[bank][index]
            # Independent bit planes at every address. DAC is identical in
            # both banks: mapper address bit 8 aliases the negative phase LSB.
            words.append(((128 + p) & 255) | (((-p) & 255) << 8) |
                         (dac[index] << 16))
    mode = 'history' if history else 'static'
    return f"""# C5VRX-4: Phase8 {mode}, 75 ns endpoint difference.
# Three bundles consume 3 IQ bytes and emit [D,D,D] at 40 MHz.
# 512x32=2048 bytes. Low byte: biased phase; next byte: -phase;
# bits 16..21: nominal DAC transfer, independent of bank.
# Only the near-origin history decoder uses the previous -phase MSB.
cfg prefetch true
cfg eof_on downstream
cfg trailing_bytes 0
cfg lut_width_bits 32
lut """ + ' '.join(map(str, words)) + """

accumulate:
    # L comes from decode_next. A.high retains -previous; A.low is ignored.
    set 0..5 O0..O5,
    set 8..15 L8..L15,
    set 24..31 L0..L7,
    read 16,
    write 8,
    addctiah

map_delta:
    # A.high is the biased delta. Loading only A.high prevents low-byte
    # LUT-address bits from contributing a carry to phase arithmetic.
    set 0..5 O0..O5,
    set 8..13 O0..O5,
    set 16..23 A8..A15,
    set 24..31 O8..O15,
    read 8,
    write 16,
    ldctiah

decode_next:
    # L comes from map_delta. Keep its DAC for both following writes.
    # Reads above advanced three bytes; byte 1 is the next endpoint.
    set 0..5 L16..L21,
    set 16..23 8..15,
    set 24 O31,
    jmp accumulate
"""


if __name__ == '__main__':
    for history in (False, True):
        name = 'history' if history else 'static'
        path = HERE / f'c5vrx4_phase8_{name}.bsasm'
        path.write_text(build(history), encoding='utf-8')
        print(f'Generated {path.name}: three bundles, 512x32 LUT')
