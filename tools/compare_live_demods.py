#!/usr/bin/env python3
"""Compare actual live LUTs on identical frozen Q4 captures (#118).

Accepts a binary packed Q4/I4 capture or complete console 'z' windows. Each
window is independent: do not concatenate gaps between separate captures.
Reports DAC statistics, not a picture rating, RF sensitivity or noise removal.
Different pedestal/gain means raw DAC metrics cannot rank demod quality.
"""
import argparse
import hashlib
import json
import re
from pathlib import Path

import numpy as np
from analyze_q4_window import parse
ROOT = Path(__file__).resolve().parents[1]


def _parse_asm_lut(path):
    match = re.search(r'^lut (.*)$', path.read_text(), re.MULTILINE)
    if not match:
        raise ValueError(f'no LUT in {path}')
    return [int(word) for word in match.group(1).split()]

NAMES = ('PHASE5_GOLDEN', 'PHASE8_FULL', 'PHASE8_VIDEO32')


def tables():
    golden = np.array(_parse_asm_lut(ROOT / 'main/fm.bsasm'), dtype=np.int64)
    full = np.array(_parse_asm_lut(ROOT / 'main/fm_phase8_hr_live.bsasm'), dtype=np.int64)
    video = np.array(_parse_asm_lut(ROOT / 'main/fm_phase8_video.bsasm'), dtype=np.int64)
    assert all(len(t) == 1024 for t in (golden, full, video))
    return golden, full, video


def codes(raw, parity, lut):
    """First endpoint has no predecessor in this capture and is discarded."""
    golden, full, video = lut
    endpoints = np.asarray(raw, dtype=np.uint8)[parity::2]
    p, c = endpoints[:-1], endpoints[1:]
    gstate = (golden[:256] >> 8) & 31
    vstate = video[:256] & 31
    return {
        NAMES[0]: golden[(gstate[p] << 5) | gstate[c]] & 63,
        NAMES[1]: (((full[p] & 255) + (full[c] >> 8)) & 255) >> 2,
        NAMES[2]: (video[(vstate[p] << 5) | vstate[c]] >> 8) & 63,
    }


def stats(values):
    jumps = np.abs(np.diff(values))
    return dict(pairs=len(values), mean_dac=float(np.mean(values)),
                p05_dac=float(np.percentile(values, 5)),
                p95_dac=float(np.percentile(values, 95)),
                rail_permille=float(np.mean((values == 0) | (values == 63)) * 1000),
                jump_ge16_permille=float(np.mean(jumps >= 16) * 1000) if len(jumps) else 0.0)


def compare(windows, parity):
    lut = tables()
    result = []
    for k, raw in enumerate(windows):
        if len(raw) < 6:
            raise ValueError('capture needs at least three endpoints')
        result.append(dict(window=k, samples=len(raw),
                           sha256=hashlib.sha256(bytes(raw)).hexdigest(),
                           modes={name: stats(v) for name, v in codes(raw, parity, lut).items()}))
    return dict(endpoint_parity=parity, unique_rate_hz=20000000,
                interpretation='Same IQ for every mode. Different DAC gain/pedestal: '
                               'rail/jump statistics alone do not prove cleaner video or longer range. '
                               'No concatenation or phase delta across separate windows.',
                windows=result)


def self_test():
    from gen_phase8_hr import phase8
    from gen_phase8_video import build
    from range_demod_bench import golden_code
    lut = tables()
    _, _, state, _, video_codes = build()
    # Exhaust every raw pair, including signed-byte arithmetic and both parities.
    for prev in range(256):
        raw = np.array([b for cur in range(256) for b in (0, prev, 0, cur)], dtype=np.uint8)
        out = codes(raw, 1, lut)
        for cur in range(256):
            pos = cur * 2
            assert out[NAMES[0]][pos] == golden_code(prev, cur)
            assert out[NAMES[1]][pos] == ((128 + phase8(cur) - phase8(prev)) & 255) >> 2
            assert out[NAMES[2]][pos] == video_codes[(state[prev] << 5) | state[cur]]
        swapped = raw.reshape(-1, 2)[:, ::-1].reshape(-1)
        assert all(np.array_equal(v, codes(swapped, 0, lut)[name]) for name, v in out.items())
    a = np.array([0x22] * 10, dtype=np.uint8)
    b = np.array([0xee] * 10, dtype=np.uint8)
    rep = compare([a, b], 1)
    assert len(rep['windows']) == 2
    assert all(w['modes'][NAMES[0]]['pairs'] == 4 for w in rep['windows'])
    assert all(w['modes'][NAMES[0]]['jump_ge16_permille'] == 0 for w in rep['windows'])
    print('Live demod capture comparison: 65,536 pairs, both parities, no cross-window delta PASS')


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('capture', type=Path, nargs='?')
    ap.add_argument('--binary', action='store_true', help='input is packed Q4/I4 bytes')
    ap.add_argument('--parity', type=int, choices=(0, 1), default=1,
                    help='endpoint byte parity relative to capture start (default 1; z ring nodes are aligned)')
    ap.add_argument('--self-test', action='store_true')
    args = ap.parse_args()
    if args.self_test:
        self_test()
        return
    if args.capture is None:
        ap.error('capture required')
    windows = [np.frombuffer(args.capture.read_bytes(), dtype=np.uint8)] if args.binary else \
              parse(args.capture.read_text(encoding='utf-8', errors='replace').splitlines())
    if not windows:
        ap.error('no complete 4092-sample Q4WIN capture found')
    try:
        print(json.dumps(compare(windows, args.parity), indent=2, allow_nan=False))
    except ValueError as e:
        ap.error(str(e))


if __name__ == '__main__':
    main()
