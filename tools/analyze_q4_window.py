#!/usr/bin/env python3
"""Analyse full 4092-sample Q4/I4 windows dumped by the console 'z' command.

For each window (and averaged over all windows in the log):
  * spectrum of the demodulated instantaneous frequency (Phase8 LUT, 25 ns
    steps), reporting the peak level near the VTX audio subcarriers
    (6.0 / 6.5 MHz) relative to the local noise floor, and near the colour
    subcarriers;
  * native-AGC step timeline: abrupt changes of the Q4 radius (a VTX or fading
    cannot change the envelope within a few 25 ns samples), with count per
    video line;
  * radius statistics.

Usage:
  python tools/analyze_q4_window.py LOG [LOG...]
  python tools/analyze_q4_window.py --self-test
"""

import argparse
import math
import re
import sys

import numpy as np

FS = 40e6
LINE_US = 64.0
WIN_RE = re.compile(r"Q4WIN (\d{4}) ([0-9a-f]+)")


def phase_lut():
    lut = np.zeros(256)
    for raw in range(256):
        q, i = raw & 15, raw >> 4
        q = q - 16 if q >= 8 else q
        i = i - 16 if i >= 8 else i
        lut[raw] = math.atan2(q + 0.5, i + 0.5)
    return lut


LUT = phase_lut()


def radius(raw):
    i = (raw >> 4).astype(int)
    q = (raw & 15).astype(int)
    i = np.where(i >= 8, i - 16, i) + 0.5
    q = np.where(q >= 8, q - 16, q) + 0.5
    return np.hypot(i, q)


def parse(lines):
    windows, cur = [], None
    for line in lines:
        if "Q4WIN begin" in line:
            cur = bytearray()
        elif "Q4WIN end" in line and cur is not None:
            windows.append(np.frombuffer(bytes(cur), dtype=np.uint8))
            cur = None
        elif cur is not None:
            m = WIN_RE.search(line)
            if m:
                cur += bytes.fromhex(m.group(2))
    return windows


def inst_freq_khz(raw):
    ph = np.unwrap(LUT[raw])
    return np.diff(ph) * FS / (2 * np.pi) / 1e3


def spectrum(raw):
    f = inst_freq_khz(raw)
    valid = radius(raw)[1:] >= 1.5
    f = np.where(valid, f, np.median(f[valid]) if valid.any() else 0.0)
    x = (f - f.mean()) * np.hanning(len(f))
    p = np.abs(np.fft.rfft(x)) ** 2
    return np.fft.rfftfreq(len(f), 1 / FS), p


def tone_excess_db(freqs, power, target_hz, half_bw=0.15e6, floor_bw=1.0e6):
    near = np.abs(freqs - target_hz) <= half_bw
    ring = (np.abs(freqs - target_hz) > half_bw * 2) & (np.abs(freqs - target_hz) <= floor_bw)
    if not near.any() or not ring.any():
        return None
    return 10 * np.log10(power[near].max() / np.median(power[ring]))


def agc_steps(raw, jump=0.45, hold=6):
    """Indices where the median radius of the next `hold` samples differs from
    the previous `hold` by more than `jump` (relative)."""
    r = radius(raw)
    steps = []
    k = hold
    while k < len(r) - hold:
        a, b = np.median(r[k - hold:k]), np.median(r[k:k + hold])
        if min(a, b) > 0 and abs(b - a) / min(a, b) > jump:
            steps.append(k)
            k += hold
        else:
            k += 1
    return steps


def analyse(windows):
    report = {"windows": len(windows), "per_window": []}
    if not windows:
        return report
    acc = None
    for w in windows:
        fr, p = spectrum(w)
        acc = p if acc is None else acc + p
        st = agc_steps(w)
        r = radius(w)
        report["per_window"].append({
            "radius_median": float(np.median(r)),
            "radius_p95": float(np.percentile(r, 95)),
            "agc_steps": len(st),
            "steps_per_line": len(st) / (len(w) / FS * 1e6 / LINE_US),
        })
    acc /= len(windows)
    report["audio_6_0_db"] = tone_excess_db(fr, acc, 6.0e6)
    report["audio_6_5_db"] = tone_excess_db(fr, acc, 6.5e6)
    report["pal_fsc_db"] = tone_excess_db(fr, acc, 4.43361875e6)
    report["ntsc_fsc_db"] = tone_excess_db(fr, acc, 3.579545e6)
    return report


def print_report(rep):
    print(f"windows={rep['windows']}")
    for k, w in enumerate(rep["per_window"]):
        print(f"  #{k}: radius median {w['radius_median']:.2f} p95 {w['radius_p95']:.2f}  "
              f"AGC steps {w['agc_steps']} ({w['steps_per_line']:.1f}/line)")
    for key, name in (("audio_6_0_db", "6.0 MHz audio"), ("audio_6_5_db", "6.5 MHz audio"),
                      ("pal_fsc_db", "PAL colour 4.43"), ("ntsc_fsc_db", "NTSC colour 3.58")):
        v = rep.get(key)
        print(f"  {name:16}: {'-' if v is None else f'{v:+.1f} dB above local floor'}")


def synth(audio_khz=700.0, steps_at=(1000, 2500), radius_levels=(2.6, 5.2, 2.6), seed=1):
    rng = np.random.default_rng(seed)
    n = 4092
    t = np.arange(n)
    f = 1500e3 * np.sin(2 * np.pi * 15.6e3 * t / FS)  # slow "video"
    f += audio_khz * 1e3 * np.sin(2 * np.pi * 6.5e6 * t / FS)
    ph = np.cumsum(2 * np.pi * f / FS)
    r = np.full(n, radius_levels[0])
    r[steps_at[0]:steps_at[1]] = radius_levels[1]
    r[steps_at[1]:] = radius_levels[2]
    i = r * np.cos(ph) + 0.2 * rng.standard_normal(n)
    q = r * np.sin(ph) + 0.2 * rng.standard_normal(n)
    ic = np.clip(np.floor(i), -8, 7).astype(int) & 15
    qc = np.clip(np.floor(q), -8, 7).astype(int) & 15
    return ((ic << 4) | qc).astype(np.uint8)


def self_test():
    w = synth()
    lines = ["Q4WIN begin n=4092 ch=A1"] + [
        f"Q4WIN {o:04d} " + w[o:o + 64].tobytes().hex() for o in range(0, 4092, 64)
    ] + ["Q4WIN end"]
    parsed = parse(lines)
    assert len(parsed) == 1 and len(parsed[0]) == 4092 and (parsed[0] == w).all()
    rep = analyse(parsed)
    assert rep["audio_6_5_db"] is not None and rep["audio_6_5_db"] > 10, rep["audio_6_5_db"]
    steps = agc_steps(w)
    assert len(steps) == 2 and abs(steps[0] - 1000) < 12 and abs(steps[1] - 2500) < 12, steps
    quiet = analyse([synth(audio_khz=0.0, steps_at=(4092, 4092))])
    assert quiet["per_window"][0]["agc_steps"] == 0
    assert quiet["audio_6_5_db"] < 10, quiet["audio_6_5_db"]
    print("analyze_q4_window: parser, audio-tone and AGC-step detection passed")


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("logs", nargs="*")
    ap.add_argument("--self-test", action="store_true")
    a = ap.parse_args()
    if a.self_test:
        self_test()
        sys.exit(0)
    lines = []
    for path in a.logs:
        with open(path, encoding="utf-8", errors="replace") as fh:
            lines.extend(fh.readlines())
    print_report(analyse(parse(lines)))
