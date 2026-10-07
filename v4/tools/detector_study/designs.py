#!/usr/bin/env python3
"""Detector design study for C5VRX-4 (host model, not a hardware result).

Honest metric: every detector output is compared with the TRUE video (CVBS
through the goggle's 5 MHz low-pass) after a least-squares gain/offset fit, so
noise, clamp distortion and lost detail all count. Clicks = output samples
whose error exceeds 40 IRE (on the 140-IRE sync-to-white scale).

Video: bounded CVBS, -40..100 IRE, 4.5 MHz detail, a 4.7 us sync pulse every
64 us, mapped to FM deviation like a real VTX (sync to white = DEV_PP Hz) plus
a carrier offset CFO. Channel: 5th-order Butterworth at +-BW, 80 -> 40 MS/s
unfiltered decimation, Q4/I4 floor quantization, noise scaled per CNR.

All detectors except adj40* fit the TX BitScrambler at 20 MS/s unique output:
two bundles per output pair, one LUT lookup per bundle, a 1024 x 16 LUT whose
decode address is raw IQ (8 bits) + 2 spare bits and whose map address is the
delta (8 bits) + 2 spare bits.
"""
import sys
from pathlib import Path
import numpy as np
from scipy import signal as sg

sys.path.insert(0, str(Path(__file__).resolve().parent))
import generate_phase8 as gen  # noqa: E402

F80, N = 80e6, 1 << 18
SIG_CELL = 1.04
DEV_PP = 6.7e6          # sync tip to peak white (CVBS150: 1 V at 0.15 V/MHz)
CFO = 1.0e6
BW = 10e6
rng = np.random.default_rng(7)

# ---- bounded CVBS truth at 80 MS/s, IRE -40..100 ----
t = np.arange(N) / F80
b = sg.firwin(801, 4.5e6, fs=F80)
luma = sg.lfilter(b, 1, rng.standard_normal(N))
luma = 50 + 30 * luma / luma.std()
luma = np.clip(luma, 0, 100)
line = (t % 64e-6) < 4.7e-6
ire = np.where(line, -40.0, luma)
ire = sg.lfilter(sg.firwin(201, 6e6, fs=F80), 1, ire)          # band-limited edges
dev = CFO + (ire - 30.0) / 140.0 * DEV_PP                         # -40..100 -> +-DEV_PP/2
s80 = np.exp(2j * np.pi * np.cumsum(dev) / F80)
white = (rng.standard_normal(N) + 1j * rng.standard_normal(N)) / np.sqrt(2)
BIN50 = 256 * 50e-9          # bins per Hz over 50 ns
LO_BIN = (CFO - DEV_PP / 2) * BIN50   # sync tip
HI_BIN = (CFO + DEV_PP / 2) * BIN50   # peak white


def chan(x):
    bb, aa = sg.butter(5, BW, fs=F80)
    return sg.lfilter(bb, aa, x)


GOG = sg.butter(4, 5e6, fs=40e6)
goggle = lambda x: sg.lfilter(*GOG, x)  # noqa: E731
truth = goggle(ire[::2])

PHASE = np.array(gen.decoder(False)[0]).astype(float)


def cells(raw):
    i = (raw >> 4) & 15
    q = raw & 15
    return np.where(i > 7, i - 16, i), np.where(q > 7, q - 16, q)


def raw_bytes(z):
    i = np.clip(np.floor(z.real), -8, 7).astype(int)
    q = np.clip(np.floor(z.imag), -8, 7).astype(int)
    return ((i & 15) << 4) | (q & 15)


def wrap(d):
    return ((d + 128) % 256) - 128


# ---------------- decoders: raw (+ previous quadrant) -> phase8 -------------
def dec_static(raw):
    return PHASE[raw]


def dec_hc(raw, ring, bias):
    """Near-origin cells take the angle nearest the previous decoded phase
    inside their own quadrant (only the previous QUADRANT is known: 2 bits).
    ring 0: the 4 origin cells; ring 1: the 16 cells with |i|,|q| <= 1.
    bias: expected rotation per sample (bins) added to the previous quadrant
    edge choice (carrier offset prior)."""
    i, q = cells(raw)
    if ring == 0:
        low = ((i == 0) | (i == -1)) & ((q == 0) | (q == -1))
    else:
        low = (i >= -2) & (i <= 1) & (q >= -2) & (q <= 1)
    st = PHASE[raw]
    out = np.empty(len(raw))
    prev = 0.0
    for k in range(len(raw)):
        if low[k]:
            cq = int(st[k]) // 64
            pq = int((prev + bias) % 256) // 64
            dq = (pq - cq) % 4
            out[k] = cq * 64 + 60 if dq == 1 else cq * 64 + 4 if dq == 3 else st[k]
        else:
            out[k] = st[k]
        prev = out[k]
    return out, low


# ---------------- maps: delta (+ confidence) -> output ---------------------
def out_span50(ph, low, mode, alpha=0.5, margin=8):
    d = wrap(np.diff(ph, prepend=ph[0]))
    cum = np.cumsum(d)
    e = (cum[2::2] - cum[0:-2:2]).astype(float)
    lo, hi = LO_BIN - margin, HI_BIN + margin
    mid = (LO_BIN + HI_BIN) / 2
    if 'conf' in mode and low is not None:
        lc = low[2::2] | low[0:-2:2]
        e = np.where(lc, alpha * e + (1 - alpha) * mid, e)
    if 'clamp' in mode:
        e = np.clip(e, lo, hi)
    if 'grey' in mode:
        e = np.where((e < lo) | (e > hi), mid, e)
    if 'fold' in mode:          # out-of-window: reflect back inside (2nd-hit guess)
        e = np.where(e > hi, hi - (e - hi), np.where(e < lo, lo + (lo - e), e))
        e = np.clip(e, lo, hi)
    e = np.floor(e / 2) * 2 + 1          # 7-bit DAC map resolution (64 codes over ~128 bins)
    y = np.repeat(e, 2)
    return goggle(np.pad(y, (0, len(ph) - len(y)))[:len(ph)])


def out_adj40(ph, clamp):
    d = wrap(np.diff(ph, prepend=ph[0])).astype(float)
    if clamp:
        d = np.clip(d, LO_BIN / 2 - 4, HI_BIN / 2 + 4)
    return goggle(d)


DESIGNS = {
    'span50':            lambda r: out_span50(dec_static(r), None, ''),
    'span50+clamp':      lambda r: out_span50(dec_static(r), None, 'clamp'),
    'span50+grey':       lambda r: out_span50(dec_static(r), None, 'grey'),
    'hc0+clamp':         lambda r: out_span50(*dec_hc(r, 0, 0), 'clamp'),
    'hc1+clamp':         lambda r: out_span50(*dec_hc(r, 1, 0), 'clamp'),
    'hc1b+clamp':        lambda r: out_span50(*dec_hc(r, 1, CFO * 25e-9 * 256), 'clamp'),
    'hc1+conf.5+clamp':  lambda r: out_span50(*dec_hc(r, 1, 0), 'conf clamp', 0.5),
    'hc1+conf.25+clamp': lambda r: out_span50(*dec_hc(r, 1, 0), 'conf clamp', 0.25),
    'st+conf.5+clamp':   lambda r: out_span50(dec_static(r), dec_hc(r, 1, 0)[1], 'conf clamp', 0.5),
    'hc1+conf.5+grey':   lambda r: out_span50(*dec_hc(r, 1, 0), 'conf grey', 0.5),
    'adj40':             lambda r: out_adj40(dec_static(r), False),
    'adj40+clamp':       lambda r: out_adj40(dec_static(r), True),
}

BANDS = [(0.05e6, 1e6), (1e6, 2e6), (2e6, 3e6), (3e6, 4e6), (4e6, 5e6)]
BAND_FIR = [sg.firwin(511, band, pass_zero=False, fs=40e6) for band in BANDS]


def align(y):
    """Best lag (quarter-sample resolution) and gain/offset against the truth."""
    sl = slice(6000, -6000)
    best = None
    up = sg.resample_poly(y, 4, 1)
    for lag in range(-24, 25):
        yy = np.roll(up, -lag)[::4]
        a, c = np.polyfit(yy[sl], truth[sl], 1)
        e = np.var((a * yy + c - truth)[sl])
        if best is None or e < best[0]:
            best = (e, a * yy + c)
    return best[1]


def score(y):
    sl = slice(6000, -6000)
    err = align(y) - truth
    tot = 10 * np.log10(np.var(truth[sl]) / np.var(err[sl]))
    bands = [10 * np.log10(np.var(sg.lfilter(f, 1, truth)[sl]) / np.var(sg.lfilter(f, 1, err)[sl]))
             for f in BAND_FIR]
    clicks = 1000 * np.mean(np.abs(err[sl]) > 40)
    return tot, bands, clicks


def main():
    only = sys.argv[1:] or list(DESIGNS)
    noise = chan(white)[::2]
    noise *= SIG_CELL * np.sqrt(2) / np.sqrt(np.mean(np.abs(noise) ** 2))
    sig = chan(s80)[::2]
    sig /= np.sqrt(np.mean(np.abs(sig) ** 2))
    print(f'CVBS sync..white {DEV_PP/1e6:.1f} MHz, CFO {CFO/1e6:.1f} MHz, channel +-{BW/1e6:.0f} MHz')
    print('design               CNR   SINAD   0-1   1-2   2-3   3-4   4-5   clicks/1000')
    for cnr in (2, 4, 6, 8, 10, 14):
        z = sig * SIG_CELL * np.sqrt(2) * np.sqrt(10 ** (cnr / 10)) + noise
        raw = raw_bytes(z)
        for name in only:
            tot, bands, clicks = score(DESIGNS[name](raw))
            print(f'{name:20s} {cnr:3d}  {tot:6.1f}  ' + ' '.join(f'{x:5.1f}' for x in bands) +
                  f'   {clicks:6.1f}')
        print()


if __name__ == '__main__':
    main()
