#!/usr/bin/env python3
"""Simulate Phase8 'false colour' on flat picture areas (blacks turning blue).

Model of the live path (tools/gen_phase8_hr_live.py, main/fm_phase8_hr_live.bsasm):
  40 MS/s complex FM -> Q4/I4 cells -> 256-code Phase8 LUT (cell centres)
  -> one sample of every 16-bit read (input bits 8..15), i.e. the 20 MS/s
     endpoint stream -> dac = top6((128 + phase[m] - phase[m-1]) & 255), so the
     live delta spans 50 ns exactly like Golden/Phase5.
  "adj25" (a 25 ns adjacent-sample delta) is kept only as a reference.

A flat area is a constant instantaneous frequency. Q4 angular quantisation
error is then a deterministic periodic function of the carrier phase, so the
DAC output carries spurious tones. A tone near the colour subcarrier is decoded
by the TV as colour. This reports that energy ('false chroma', in IRE rms)
versus picture level for different Q4 radii and demodulator mappings.

Usage: python tools/sim_phase8_false_colour.py [--self-test]
"""

import argparse
import math
import sys

import numpy as np

FS = 40e6
KHZ_PER_IRE = 57.0          # assumed VTX deviation (~8 MHz over 140 IRE)
FSC = {"NTSC": 3579545.0, "PAL": 4433618.75}
CHROMA_BW = 0.6e6           # +/- band a TV chroma decoder passes
N = 1 << 15


def phase8_lut():
    lut = np.zeros(256, dtype=np.int64)
    for raw in range(256):
        q_code, i_code = raw & 15, raw >> 4
        q = (q_code if q_code < 8 else q_code - 16) * 64 + 31.5
        i = (i_code if i_code < 8 else i_code - 16) * 64 + 31.5
        lut[raw] = round(math.atan2(q, i) * 128 / math.pi) & 255
    return lut


LUT = phase8_lut()


def arc_mid_lut(design_radius):
    """Per-cell angle = midpoint of the arc of a ring of design_radius (cells)
    that crosses the cell; cell centre when the ring misses the cell."""
    lut = LUT.copy()
    for raw in range(256):
        q_code, i_code = raw & 15, raw >> 4
        i0 = i_code if i_code < 8 else i_code - 16
        q0 = q_code if q_code < 8 else q_code - 16
        angles = []
        for k in range(720):
            a = 2 * math.pi * k / 720
            x, y = design_radius * math.cos(a), design_radius * math.sin(a)
            if i0 <= x < i0 + 1 and q0 <= y < q0 + 1:
                angles.append(a)
        if angles:
            c = math.atan2(sum(math.sin(a) for a in angles), sum(math.cos(a) for a in angles))
            lut[raw] = round(c * 128 / math.pi) & 255
    return lut


LUTS = {"centre": LUT}
_VIDEO_STATE = _VIDEO_CODES = None


def q4_bytes(freq_hz, radius, noise, rng, n=N):
    t = np.arange(n)
    ph = 2 * np.pi * freq_hz * t / FS + rng.uniform(0, 2 * np.pi)
    i = radius * np.cos(ph) + noise * rng.standard_normal(n)
    q = radius * np.sin(ph) + noise * rng.standard_normal(n)
    ic = np.clip(np.floor(i), -8, 7).astype(np.int64) & 15
    qc = np.clip(np.floor(q), -8, 7).astype(np.int64) & 15
    return (ic << 4) | qc


def demod(raw, mode):
    """Return DAC codes (float, 0..63) at the demodulator's output rate."""
    name, _, lut_name = mode.partition(":")
    global _VIDEO_STATE, _VIDEO_CODES
    if name == "video32":
        if _VIDEO_STATE is None:
            from gen_phase8_video import build
            _, _, states, _, codes = build()
            _VIDEO_STATE, _VIDEO_CODES = np.array(states), np.array(codes)
        st = _VIDEO_STATE[raw][1::2]
        return _VIDEO_CODES[(st[:-1] << 5) | st[1:]].astype(float), 20e6, 6.0
    key = lut_name or "centre"
    if key not in LUTS:
        LUTS[key] = arc_mid_lut(float(key[1:]))
    lut = LUTS[key]
    p = lut[raw]
    if name == "adj25":          # reference only: adjacent pair, 25 ns
        d = (128 + p[1::2] - p[0::2]) & 255
        return (d >> 2).astype(float), 20e6, 1.0
    if name == "live50":         # live: endpoint stream, 50 ns delta
        po = p[1::2]
        d = (128 + po[1:] - po[:-1]) & 255
        return (d >> 2).astype(float), 20e6, 2.0
    raise ValueError(mode)


def false_chroma_ire(freq_hz, radius, noise, mode, std, rng):
    raw = q4_bytes(freq_hz, radius, noise, rng)
    dac, rate, gain = demod(raw, mode)
    # One DAC step in IRE: 4 codes * 156.25 kHz / gain / KHZ_PER_IRE.
    ire_per_step = 4 * 156.25 / gain / KHZ_PER_IRE
    x = (dac - dac.mean()) * ire_per_step
    spec = np.fft.rfft(x * np.hanning(len(x)))
    f = np.fft.rfftfreq(len(x), 1 / rate)
    band = np.abs(f - FSC[std]) <= CHROMA_BW
    # rms of the band-limited component (Hann window power correction 1.5).
    power = 2 * np.sum(np.abs(spec[band]) ** 2) / (len(x) ** 2) / 0.375
    pedestal = 20 if mode.startswith("video32") else 32
    ideal = pedestal + freq_hz / 1e3 / (4 * 156.25 / gain)
    level_err = float((dac.mean() - ideal) * ire_per_step)
    return math.sqrt(power), level_err


def sweep(radius, noise, mode, std, cfo_khz=0.0, seed=1):
    rng = np.random.default_rng(seed)
    rows = []
    for ire in range(-40, 101, 5):
        f = (cfo_khz + KHZ_PER_IRE * ire) * 1e3
        fc, _ = false_chroma_ire(f, radius, noise, mode, std, rng)
        rows.append((ire, fc))
    return rows


def report(std="PAL", noise=0.25):
    modes = ("adj25", "live50", "live50:r2.5", "live50:r4.0", "video32")
    print(f"False chroma (IRE rms in {std} chroma band), noise sigma={noise} cells")
    for radius in (1.5, 2.5, 4.0, 5.5):
        print(f"\nQ4 radius {radius} cells")
        print("  IRE  " + "".join(f"{m:>13}" for m in modes))
        data = {m: sweep(radius, noise, m, std) for m in modes}
        for idx in range(0, len(data[modes[0]]), 2):
            ire = data[modes[0]][idx][0]
            print(f"  {ire:4d} " + "".join(f"{data[m][idx][1]:13.2f}" for m in modes))
        for m in modes:
            dark = [v for i, v in data[m] if 0 <= i <= 20]
            print(f"  dark 0..20 IRE max {m}: {max(dark):.2f}")


def self_test():
    rng = np.random.default_rng(3)
    # Large radius -> less false chroma than small radius for the live path.
    small = max(v for i, v in sweep(1.5, 0.25, "live50", "PAL", seed=4) if 0 <= i <= 20)
    large = max(v for i, v in sweep(5.5, 0.25, "live50", "PAL", seed=4) if 0 <= i <= 20)
    assert large < small, (small, large)
    raw = q4_bytes(1e6, 5.0, 0.0, rng, 64)
    assert raw.min() >= 0 and raw.max() <= 255
    # Constant-SNR comparison: changing RF gain also changes receiver noise.
    # Do not present the fixed-sigma sweep as a measured sensitivity gain.
    same_snr_small = max(v for i, v in sweep(2.5, 0.25, "live50", "PAL", seed=4) if 0 <= i <= 20)
    same_snr_large = max(v for i, v in sweep(5.5, 0.55, "live50", "PAL", seed=4) if 0 <= i <= 20)
    assert same_snr_large < same_snr_small
    candidate, rate, _ = demod(raw, "video32")
    assert candidate.min() >= 0 and candidate.max() <= 63 and rate == 20e6
    print(f"sim_phase8_false_colour: small-radius dark false chroma {small:.2f} IRE > "
          f"large-radius {large:.2f} IRE (self-test passed)")
    print(f"constant-SNR dark chroma: {same_snr_small:.2f} -> {same_snr_large:.2f} IRE; simulation only")


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--self-test", action="store_true")
    ap.add_argument("--std", default="PAL", choices=sorted(FSC))
    ap.add_argument("--noise", type=float, default=0.25)
    a = ap.parse_args()
    if a.self_test:
        self_test()
    else:
        report(a.std, a.noise)
