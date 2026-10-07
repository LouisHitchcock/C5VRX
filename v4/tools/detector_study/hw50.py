#!/usr/bin/env python3
"""Exact-hardware span50 variants that fit the 2-bundle TX BitScrambler loop.

The live span50 program decodes only the pair ENDPOINTS: per output pair the
counter computes A.high = BIAS + MULT*(p_cur - p_prev) mod 256 from one LUT
word (minus | plus << 8) and the DAC is A.high's top six bits, held [D,D].
There is one decode lookup per pair, addressed by the next raw IQ byte plus
two bank bits that are the low two bits of the retained minus term.

  hw50        BIAS 128, MULT 1 (the field-tested program)
  hw50x2      MULT 2, BIAS centred on the video (twice the DAC levels across
              sync..white; wraps beyond +-64 bins = +-5 MHz around centre)
  hc50p6      bank bits = previous endpoint's quadrant: the minus/plus terms
              are rounded to multiples of 4 (Phase6 endpoints) and the
              minus term's low two bits carry the quadrant tag. Origin cells
              decode near the quadrant edge facing the previous quadrant.
  hc50p6x2    both
Scored like designs.py (true video, delay/gain fitted, clicks > 40 IRE).
"""
import numpy as np
import designs as D

CENTRE = (D.LO_BIN + D.HI_BIN) / 2          # video centre in 50-ns bins


def endpoint_phases(raw, hc, p6):
    ends = raw[0::2]                            # one decoded endpoint per pair
    st = D.PHASE[ends].astype(int)
    if p6:
        st = (np.round(st / 4).astype(int) * 4) % 256
    if not hc:
        return st
    i, q = D.cells(ends)
    low = ((i == 0) | (i == -1)) & ((q == 0) | (q == -1))
    out = np.empty(len(ends), int)
    prev = 0
    for k in range(len(ends)):
        if low[k]:
            cq = st[k] // 64
            dq = (prev // 64 - cq) % 4
            out[k] = cq * 64 + 60 if dq == 1 else cq * 64 + 4 if dq == 3 else st[k]
        else:
            out[k] = st[k]
        prev = out[k]
    return out


def hw(raw, mult, hc=False, p6=False):
    ph = endpoint_phases(raw, hc, p6)
    bias = 128 if mult == 1 else int(round(128 - mult * CENTRE)) % 256
    a = (bias + mult * (ph[1:] - ph[:-1])) % 256
    dac = a >> 2                                  # top six bits
    y = np.repeat(dac.astype(float), 2)
    return D.goggle(np.pad(y, (0, len(raw) - len(y)))[:len(raw)])


DESIGNS = {
    'hw50':      lambda r: hw(r, 1),
    'hw50x2':    lambda r: hw(r, 2),
    'hc50p6':    lambda r: hw(r, 1, True, True),
    'hc50p6x2':  lambda r: hw(r, 2, True, True),
    'p6only':    lambda r: hw(r, 1, False, True),
    'adj40':     D.DESIGNS['adj40'],
    'span75fw':  None,
}


def main():
    noise = D.chan(D.white)[::2]
    noise *= D.SIG_CELL * np.sqrt(2) / np.sqrt(np.mean(np.abs(noise) ** 2))
    sig = D.chan(D.s80)[::2]
    sig /= np.sqrt(np.mean(np.abs(sig) ** 2))
    names = [n for n, f in DESIGNS.items() if f]
    print('design      ' + '  '.join(f'C/N{c:>3}' for c in (2, 4, 6, 8, 10, 14)) + '   (SINAD dB / clicks per 1000)')
    for name in names:
        cells = []
        for cnr in (2, 4, 6, 8, 10, 14):
            raw = D.raw_bytes(sig * D.SIG_CELL * np.sqrt(2) * np.sqrt(10 ** (cnr / 10)) + noise)
            tot, _, clicks = D.score(DESIGNS[name](raw))
            cells.append(f'{tot:4.1f}/{clicks:4.0f}')
        print(f'{name:10s}  ' + '  '.join(cells), flush=True)


if __name__ == '__main__':
    main()
