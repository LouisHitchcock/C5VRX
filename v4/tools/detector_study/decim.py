"""Cost of the unfiltered 80 -> 40 MS/s PARLIO decimation (host model)."""
import numpy as np
from scipy import signal as sg
import designs as D
import hw50 as H

AAF = sg.firwin(255, 19e6, fs=D.F80)          # near-ideal 20 MHz low-pass before 2:1

def stream(bw, ideal):
    """Fixed C/N0: the scale factors come from the unfiltered (PARLIO) case,
    so noise folded from 20..40 MHz counts against PARLIO 2:1."""
    bb, aa = sg.butter(5, bw, fs=D.F80)
    s = sg.lfilter(bb, aa, D.s80); n = sg.lfilter(bb, aa, D.white)
    ks = 1 / np.sqrt(np.mean(np.abs(s[::2]) ** 2)); kn = 1 / np.sqrt(np.mean(np.abs(n[::2]) ** 2))
    if ideal:
        s = np.convolve(s, AAF, mode="same"); n = np.convolve(n, AAF, mode="same")
    s, n = s[::2], n[::2]
    return s * ks, n * kn

print('C/N = PARLIO-case C/N (fixed C/N0); SINAD dB / clicks per 1000')
for bw in (10e6, 20e6, 30e6):
    for ideal in (False, True):
        s, n = stream(bw, ideal)
        for name in ('hw50', 'hc50p6'):
            cells = []
            for cnr in (2, 4, 6, 8, 14):
                raw = D.raw_bytes(s * D.SIG_CELL * np.sqrt(2) * np.sqrt(10 ** (cnr / 10)) + n * D.SIG_CELL * np.sqrt(2))
                tot, _, clicks = D.score(H.DESIGNS[name](raw))
                cells.append(f'{tot:4.1f}/{clicks:4.0f}')
            print(f'+-{bw/1e6:2.0f} MHz {"ideal-dec" if ideal else "PARLIO2:1"} {name:7s} ' + '  '.join(cells), flush=True)
