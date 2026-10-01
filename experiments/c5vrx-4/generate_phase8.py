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


def quadrant(signs):
    i, q = signs & 1, (signs >> 1) & 1
    return (2 if q else 1) if i else (3 if q else 0)


def trajectory_class(index):
    quarters = [quadrant(index >> (2 * k)) for k in range(4)]
    steps = [(b - a) & 3 for a, b in zip(quarters, quarters[1:])]
    if 2 in steps:
        return 3  # Opposite quadrants: direction is not established.
    n = sum(-1 if d == 3 else d for d in steps)
    return 1 if n >= 2 else 2 if n <= -2 else 0


def transfer_delta(index):
    delta = (index & 63) * 4 + 2 - 128
    cls = index >> 6
    if cls == 3:
        return 0  # Explicit neutral output for an ambiguous trajectory.
    if cls == 1 and delta < 0:
        delta += 256
    if cls == 2 and delta >= 0:
        delta -= 256
    return delta


def words_for(history):
    phases = decoder(history)
    resistance = (8200, 3900, 2000, 1000, 470, 240)
    voltage = [sum(1 / r for bit, r in enumerate(resistance)
                   if code & (1 << bit)) for code in range(64)]
    dac = [min(range(64), key=lambda code:
               abs(voltage[code] - voltage[63] *
                   max(0, min(1, (transfer_delta(index) + 128) / 255))))
           for index in range(256)]
    words = []
    for bank in range(4):
        for index in range(256):
            if bank & 1:
                p = phases[bank >> 1][index]
                words.append(((128 + p) & 255) | (((-p) & 255) << 8))
            else:
                # Counter payload bit24 is zero. Bit25 may vary with phase;
                # duplicate trajectory/DAC planes in both even banks.
                words.append(dac[index] | (trajectory_class(index) << 6))
    return words


def build(history):
    words = words_for(history)
    mode = 'history' if history else 'static'
    return f"""# C5VRX-4: unwrapped Phase8 {mode}, 75 ns endpoint difference.
# Three bundles consume 3 IQ bytes and emit [D,D,D] at 40 MHz.
# 1024x16=2048 bytes: odd banks decode complete Phase8 endpoints;
# even banks hold DAC6 and trajectory2 in independent planes.
# O6/O7 retain both endpoint parity bits; DAC only connects bits0..5.
cfg prefetch true
cfg eof_on downstream
cfg trailing_bytes 0
cfg lut_width_bits 16
lut """ + ' '.join(map(str, words)) + """

accumulate:
    # P,M1,M2,C are FIFO bytes1,2,3,4. L is the full decoded C.
    # Counter operands have bit0 cleared to select an even LUT bank.
    # Retained endpoint parity makes the pre-transfer result exact Phase8.
    set 0..6 O0..O6,
    set 7 L0,
    set 8..15 L8..L15,
    set 16 15,
    set 17 11,
    set 18 23,
    set 19 19,
    set 20 31,
    set 21 27,
    set 22 39,
    set 23 35,
    set 24 L,
    set 25..31 L1..L7,
    read 16,
    write 8,
    addctiah

map_delta:
    # LUT16 leaves Counter A and FIFO bits32..47 directly accessible.
    # Address: delta6 + trajectory2. Payload: truncated -C for next delta.
    # Final DAC address is a 4-bin midpoint (<=2 bins from exact delta).
    set 0..7 O0..O7,
    set 8..13 O0..O5,
    set 16..21 A10..A15,
    set 22..23 L6..L7,
    set 24 L,
    set 25..31 O9..O15,
    read 8,
    write 16,
    ldctiah

decode_next:
    # Byte4 keeps the previous endpoint and both middle samples in FIFO
    # for the following accumulate. Select an odd decode bank explicitly.
    set 0..5 L0..L5,
    set 6 O7,
    set 16..23 32..39,
    set 24 H,
    set 25 O31,
    jmp accumulate
"""


if __name__ == '__main__':
    for history in (False, True):
        name = 'history' if history else 'static'
        path = HERE / f'c5vrx4_phase8_{name}.bsasm'
        path.write_text(build(history), encoding='utf-8')
        print(f'Generated {path.name}: three bundles, 1024x16 LUT, trajectory unwrap')
