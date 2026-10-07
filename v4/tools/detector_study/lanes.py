"""Quantization at the range edge: which 4-bit lane (host model, HC50 detector).
Noise sigma in quantizer steps: coarse 0.52, fine 1.04 (board), ultrafine 2.08.
Sign-preserving lanes fold outside their window (+288 reads as +32): modelled
as wrap of the magnitude bits, sign kept."""
import numpy as np
import designs as D
import hw50 as H

def raw_lane(z):
    def q(v):
        x = np.floor(v).astype(int)
        s = x < 0
        mag = np.where(s, -x - 1, x) & 7          # fold: keep 3 magnitude bits
        return np.where(s, -mag - 1, mag)
    i, qq = q(z.real), q(z.imag)
    return ((i & 15) << 4) | (qq & 15)

noise = D.chan(D.white)[::2]; noise /= np.sqrt(np.mean(np.abs(noise) ** 2))
sig = D.chan(D.s80)[::2]; sig /= np.sqrt(np.mean(np.abs(sig) ** 2))
print('lane (noise sigma)   ' + '  '.join(f'C/N{c:>3}' for c in (0, 2, 4, 6, 8, 14)) + '   SINAD/clicks')
for name, sigma in (('coarse', 0.52), ('fine', 1.04), ('ultrafine', 2.08), ('x1.5', 1.56)):
    cells = []
    for cnr in (0, 2, 4, 6, 8, 14):
        z = (sig * np.sqrt(10 ** (cnr / 10)) + noise) * sigma * np.sqrt(2) / np.sqrt(2)
        z = sig * sigma * np.sqrt(2) * np.sqrt(10 ** (cnr / 10)) / np.sqrt(2) * np.sqrt(2) + noise * sigma
        tot, _, clicks = D.score(H.DESIGNS['hc50p6'](raw_lane(z)))
        cells.append(f'{tot:4.1f}/{clicks:4.0f}')
    print(f'{name:9s} ({sigma:4.2f})    ' + '  '.join(cells), flush=True)
