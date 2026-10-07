#!/usr/bin/env python3
"""Trained-map detector study (host model). Builds on designs.py.

Map lookup address = span50 delta (8 bits) + context (2 bits); its 16-bit word
holds the output. A trained table stores, per (delta, context), the mean CLEAN
delta (MMSE) learned on an independent training realization (other video,
other noise seed, CNR mix 1..16 dB). Contexts:
  none   : 1-D MMSE map (learned clamp/shrink)
  prev   : quartile of the previous OUTPUT (recursive, 2 bits retained)
  conf   : low-radius flag of the current and previous endpoint
  hcprev : HC decoder (previous quadrant) + prev context
"""
import sys
import numpy as np
from scipy import signal as sg
import designs as D

CNRS = (2, 4, 6, 8, 10, 14)
TRAIN_CNRS = (1, 2, 3, 4, 5, 6, 8, 10, 12, 16)


def make_stream(seed):
    rng = np.random.default_rng(seed)
    N = D.N
    t = np.arange(N) / D.F80
    luma = sg.lfilter(sg.firwin(801, 4.5e6, fs=D.F80), 1, rng.standard_normal(N))
    luma = np.clip(50 + 30 * luma / luma.std(), 0, 100)
    ire = np.where((t % 64e-6) < 4.7e-6, -40.0, luma)
    ire = sg.lfilter(sg.firwin(201, 6e6, fs=D.F80), 1, ire)
    dev = D.CFO + (ire - 30.0) / 140.0 * D.DEV_PP
    s80 = np.exp(2j * np.pi * np.cumsum(dev) / D.F80)
    white = (rng.standard_normal(N) + 1j * rng.standard_normal(N)) / np.sqrt(2)
    sig = D.chan(s80)[::2]
    sig /= np.sqrt(np.mean(np.abs(sig) ** 2))
    noise = D.chan(white)[::2]
    noise /= np.sqrt(np.mean(np.abs(noise) ** 2))
    # clean span50 delta (unquantized phase) as the training target
    ph = np.angle(sig) * 128 / np.pi
    d = D.wrap(np.diff(ph, prepend=ph[0]))
    cum = np.cumsum(d)
    clean_e = cum[2::2] - cum[0:-2:2]
    return sig, noise, clean_e, ire


def noisy_raw(sig, noise, cnr):
    z = (sig * np.sqrt(10 ** (cnr / 10)) + noise) * D.SIG_CELL * np.sqrt(2) / np.sqrt(1 + 0 * cnr)
    # noise power fixed at SIG_CELL^2 per axis
    z = sig * D.SIG_CELL * np.sqrt(2) * np.sqrt(10 ** (cnr / 10)) + noise * D.SIG_CELL * np.sqrt(2)
    return D.raw_bytes(z)


def deltas(raw, hc):
    if hc:
        ph, low = D.dec_hc(raw, 0, 0)
    else:
        ph = D.dec_static(raw)
        i, q = D.cells(raw)
        low = ((i == 0) | (i == -1)) & ((q == 0) | (q == -1))
    d = D.wrap(np.diff(ph, prepend=ph[0]))
    cum = np.cumsum(d)
    e = np.round(cum[2::2] - cum[0:-2:2]).astype(int)
    e = ((e + 128) % 256)          # 8-bit wrapped delta as map address
    conf = (low[2::2].astype(int) << 1) | low[0:-2:2].astype(int)
    return e, conf


LO, HI = D.LO_BIN, D.HI_BIN


def level_q(v):
    return np.clip(((v - LO) / (HI - LO) * 4).astype(int), 0, 3)


def run_map(e, ctx_mode, conf, table):
    out = np.empty(len(e))
    if ctx_mode == 'prev':
        prev = 1
        for k in range(len(e)):
            out[k] = table[prev, e[k]]
            prev = min(3, max(0, int((out[k] - LO) / (HI - LO) * 4)))
        return out
    ctx = conf if ctx_mode == 'conf' else np.zeros(len(e), int)
    return table[ctx, e]


def outer_only(table, margin):
    """Identity inside the sync-to-white window (+margin), trained outside."""
    t = table.copy()
    e = np.arange(256) - 128
    inside = (e >= LO - margin) & (e <= HI + margin)
    t[:, inside] = e[inside][None, :].astype(float)
    return t


def run_hold(e_raw_signed, margin):
    """Upper bound: out-of-window deltas repeat the previous output exactly."""
    out = np.empty(len(e_raw_signed)); prev = (LO + HI) / 2
    for k, v in enumerate(e_raw_signed):
        prev = v if LO - margin <= v <= HI + margin else prev
        out[k] = prev
    return out


def train(ctx_mode, hc, seed=101):
    sig, noise, clean, _ = make_stream(seed)
    sums = np.zeros((4, 256)); cnts = np.zeros((4, 256))
    table = None
    for it in range(2 if ctx_mode == 'prev' else 1):
        sums[:] = 0; cnts[:] = 0
        for cnr in TRAIN_CNRS:
            e, conf = deltas(noisy_raw(sig, noise, cnr), hc)
            m = min(len(e), len(clean))
            e, conf, c = e[:m], conf[:m], clean[:m]
            if ctx_mode == 'prev':
                if table is None:   # oracle context from the clean previous delta
                    ctx = np.concatenate([[1], level_q(c[:-1])])
                else:
                    out = run_map(e, 'prev', None, table)
                    ctx = np.concatenate([[1], level_q(out[:-1])])
            elif ctx_mode == 'conf':
                ctx = conf
            else:
                ctx = np.zeros(m, int)
            np.add.at(sums, (ctx, e), c)
            np.add.at(cnts, (ctx, e), 1)
        # unseen cells: fall back to the clamped identity
        ident = np.clip(np.arange(256) - 128, LO - 8, HI + 8).astype(float)
        table = np.where(cnts > 20, sums / np.maximum(cnts, 1), ident[None, :])
    return table


def output(raw, ctx_mode, hc, table, hold_margin=None):
    e, conf = deltas(raw, hc)
    if hold_margin is not None:
        v = run_hold(e - 128, hold_margin)
    else:
        v = run_map(e, ctx_mode, conf, table)
    v = np.round((v - LO) / (HI - LO + 16) * 63)   # 64 DAC codes over the window
    y = np.repeat(v, 2)
    return D.goggle(np.pad(y, (0, len(raw) - len(y)))[:len(raw)])


def main():
    base = [('prev', 'prev', False), ('hcprev', 'prev', True), ('hcnone', 'none', True)]
    trained = {name: train(mode, hc) for name, mode, hc in base}
    designs = []
    tables = {}
    for m in (4, 8, 16):
        for name, mode, hc in base:
            key = f'{name}-outer{m}'
            designs.append((key, mode, hc)); tables[key] = outer_only(trained[name], m)
    holds = [('hold-exact8', 8, False), ('hc+hold-exact8', 8, True)]
    print('trained on seed 101; testing on the designs.py realization (seed 7)')
    noise = D.chan(D.white)[::2]
    noise /= np.sqrt(np.mean(np.abs(noise) ** 2))
    sig = D.chan(D.s80)[::2]
    sig /= np.sqrt(np.mean(np.abs(sig) ** 2))
    print('design               CNR   SINAD   0-1   1-2   2-3   3-4   4-5   clicks/1000')
    for cnr in CNRS:
        raw = noisy_raw(sig, noise, cnr)
        rows = [('span50', D.DESIGNS['span50'](raw)), ('hc0+clamp', D.DESIGNS['hc0+clamp'](raw)),
                ('adj40', D.DESIGNS['adj40'](raw))]
        rows += [(name, output(raw, mode, hc, tables[name])) for name, mode, hc in designs]
        rows += [(name, output(raw, None, hc, None, m)) for name, m, hc in holds]
        for name, y in rows:
            tot, bands, clicks = D.score(y)
            print(f'{name:20s} {cnr:3d}  {tot:6.1f}  ' + ' '.join(f'{x:5.1f}' for x in bands) +
                  f'   {clicks:6.1f}')
        print()
        sys.stdout.flush()


if __name__ == '__main__':
    main()
