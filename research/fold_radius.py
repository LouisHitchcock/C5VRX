"""C5VRX by Twotoz and the C5VRX contributors: the carrier radius of a folded
4-bit window, for the LEVEL AGC.

The ultrafine lane keeps ADC bits {9,6,5,4}: the sign of each component is
the ADC's own and its magnitude folds modulo 8 cells (iq_lane_fusion.py
component_code). A folded window reads small, so its power says little about
how far over the window the carrier is. An FM carrier is a circle, though:
for a radius R and a uniform phase, every pair of component magnitudes
(|I| token, |Q| token: 8 x 8 classes, the signs carry nothing for a centred
circle) has a known probability. The radius of a window is the R whose
classes explain its samples best (maximum likelihood over a table
log P(class | R, noise)), on the exact hardware model.

    python research/fold_radius.py eval            # accuracy on simulated windows
    python research/fold_radius.py header           # write main/fold_radius_table.h
    python research/fold_radius.py header --check   # CI: the header is current
"""
import argparse
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import iq_lane_fusion as F  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / 'main/fold_radius_table.h'
R_STEP = 0.25                      # cells
R_GRID = np.arange(1.0, 30.0 + 1e-9, R_STEP)
SIGMAS = (0.35, 0.7, 1.4)          # per-axis noise classes in cells (table per class)
PHASES = 1 << 15
FLOOR = 1e-4                       # the probability no class falls below (DC, interference)
DC_SPREAD = 0.3                    # cells per axis: the residual DC the live servo leaves (board walk 288: 0.1-0.6)


def tokens(x_cells):
    """4-bit tokens (-8..7) of one component, exact hardware model."""
    t = F.component_code(F.adc10(np.asarray(x_cells) * 16.0), F.ULTRA.i).astype(int)
    return np.where(t > 7, t - 16, t)


def mag_class(t):
    """|component| class 0..7 of a token: cells t + 0.5, mirrored for negatives."""
    return np.where(t >= 0, t, -t - 1)


def window_classes(i_tok, q_tok):
    return mag_class(i_tok) * 8 + mag_class(q_tok)


def table(sigma, seed=1):
    rng = np.random.default_rng(seed)
    logp = np.zeros((len(R_GRID), 64))
    for k, r in enumerate(R_GRID):
        phi = rng.uniform(0, 2 * np.pi, PHASES)
        # one residual DC per window of 256 samples, as the servo leaves it
        dc = np.repeat(rng.normal(0, DC_SPREAD, (2, PHASES // 256)), 256, axis=1)
        i = r * np.cos(phi) + dc[0] + sigma * rng.normal(size=PHASES)
        q = r * np.sin(phi) + dc[1] + sigma * rng.normal(size=PHASES)
        c = np.bincount(window_classes(tokens(i), tokens(q)), minlength=64) / PHASES
        logp[k] = np.log(np.maximum(c, FLOOR))
    return logp


def estimate(classes, logp):
    """(radius, mean log-likelihood per sample) of one window."""
    h = np.bincount(classes, minlength=64)
    score = logp @ h
    k = int(np.argmax(score))
    return R_GRID[k], score[k] / max(1, h.sum())


def fm_window(rng, n, r, sigma, dc=(0.0, 0.0)):
    """An FM carrier: -1 MHz offset plus a random video deviation at 40 MS/s."""
    f = -1.0e6 + rng.uniform(-3e6, 3e6) + 1.5e6 * np.sin(2 * np.pi * rng.uniform(0.2, 2.0) * np.arange(n) / n)
    phi = rng.uniform(0, 2 * np.pi) + np.cumsum(2 * np.pi * f / 40e6)
    i = r * np.cos(phi) + dc[0] + sigma * rng.normal(size=n)
    q = r * np.sin(phi) + dc[1] + sigma * rng.normal(size=n)
    return tokens(i), tokens(q)


def noise_window(rng, n, sigma, dc):
    return tokens(dc[0] + sigma * rng.normal(size=n)), tokens(dc[1] + sigma * rng.normal(size=n))


def evaluate():
    rng = np.random.default_rng(7)
    tabs = {s: table(s) for s in SIGMAS}
    print('carrier windows: radius estimate (median / p10..p90) and wrong-fold share (|error| > 2 cells)')
    for n in (64, 256):
        for sigma in (0.35, 0.7, 1.4):
            row = []
            for r in (4, 6, 8, 10, 12, 14, 17, 20, 24):
                est = []
                ll = []
                for _ in range(200):
                    i, q = fm_window(rng, n, r, sigma, dc=rng.normal(0, 0.2, 2))
                    e, l = estimate(window_classes(i, q), tabs[sigma])
                    est.append(e); ll.append(l)
                est = np.array(est)
                bad = np.mean(np.abs(est - r) > 2)
                row.append(f'{r:>2}:{np.median(est):5.2f} {np.percentile(est,10):5.2f}..{np.percentile(est,90):5.2f} bad {100*bad:3.0f}%')
            print(f'  n={n:3d} sigma={sigma:4.2f}  ' + ' | '.join(row[:5]))
            print(f'  {"":20s}' + ' | '.join(row[5:]))
    print('\nmean log-likelihood per sample: carrier vs noise/DC clouds (sigma table 0.7, n=256)')
    for label, gen in (
            ('carrier R=8', lambda: fm_window(rng, 256, 8, 0.7)),
            ('carrier R=14', lambda: fm_window(rng, 256, 14, 0.7)),
            ('carrier R=20', lambda: fm_window(rng, 256, 20, 0.7)),
            ('noise 2.1, DC 0', lambda: noise_window(rng, 256, 2.1, (0, 0))),
            ('noise 2.1, DC +4.1/-3.7', lambda: noise_window(rng, 256, 2.1, (4.1, -3.7))),
            ('noise 4.0, DC 0', lambda: noise_window(rng, 256, 4.0, (0, 0))),
            ('noise 8.0, DC 0', lambda: noise_window(rng, 256, 8.0, (0, 0)))):
        v = [estimate(window_classes(*gen()), tabs[0.7]) for _ in range(300)]
        ll = np.array([x[1] for x in v]); rr = np.array([x[0] for x in v])
        print(f'  {label:26s} ll p10 {np.percentile(ll,10):6.2f} median {np.median(ll):6.2f} p90 {np.percentile(ll,90):6.2f}'
              f'   radius median {np.median(rr):5.2f}')


SCALE = 12        # log-probability units per nat in the header (int8: FLOOR -> -111)
HEADER_SIGMAS = SIGMAS[:2]   # the firmware estimates only where noise is under ~1 cell per axis


def render():
    out = ['/* C5VRX-5 by Twotoz and the C5VRX contributors: log P(magnitude class | carrier radius)',
           ' * of the ultrafine lane, for agc_level_fold_radius() (agc.c). Generated by',
           ' * research/fold_radius.py from the exact hardware fold model (sign kept,',
           ' * magnitude modulo 8 cells, 10-bit saturation); never edit by hand. */',
           '#pragma once', '#include <stdint.h>', '',
           f'#define FOLD_R_N {len(R_GRID)}',
           f'#define FOLD_R0_X16 {int(round(R_GRID[0] * 16))}        /* first radius, cells x16 */',
           f'#define FOLD_R_STEP_X16 {int(round(R_STEP * 16))}       /* radius step, cells x16 */',
           f'#define FOLD_SIGMAS {len(HEADER_SIGMAS)}',
           f'#define FOLD_SCALE {SCALE}            /* units per nat */',
           '/* per-axis receiver noise of each table, cells x16 */',
           'static const uint8_t fold_sigma_x16[FOLD_SIGMAS] = {'
           + ', '.join(str(int(round(s * 16))) for s in HEADER_SIGMAS) + '};',
           '/* expected mean log P per sample of a true circle at each radius (x16 FOLD_SCALE units) */',
           'static const int16_t fold_expect[FOLD_SIGMAS][FOLD_R_N] = {']
    tabs = [table(s) for s in HEADER_SIGMAS]
    for t in tabs:
        h = (np.exp(t) * t).sum(1)
        out.append('    {' + ', '.join(str(int(round(v * SCALE * 16))) for v in h) + '},')
    out += ['};', 'static const int8_t fold_logp[FOLD_SIGMAS][FOLD_R_N][64] = {']
    for t in tabs:
        out.append('    {')
        for row in t:
            out.append('        {' + ','.join(str(max(-128, int(round(v * SCALE)))) for v in row) + '},')
        out.append('    },')
    out.append('};')
    return '\n'.join(out) + '\n'


if __name__ == '__main__':
    ap = argparse.ArgumentParser()
    ap.add_argument('mode', choices=('eval', 'header'))
    ap.add_argument('--check', action='store_true')
    a = ap.parse_args()
    if a.mode == 'eval':
        evaluate()
    elif a.check:
        if not HEADER.exists() or HEADER.read_text() != render():
            sys.exit(f'{HEADER.relative_to(ROOT)} is not current: python research/fold_radius.py header')
        print(f'{HEADER.relative_to(ROOT)} current')
    else:
        HEADER.write_text(render())
        print(f'wrote {HEADER.relative_to(ROOT)}')
