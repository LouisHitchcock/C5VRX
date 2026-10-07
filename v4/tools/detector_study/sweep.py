import numpy as np
import designs as D

def dec_hc_edge(raw, inner):
    i, q = D.cells(raw)
    low = ((i == 0) | (i == -1)) & ((q == 0) | (q == -1))
    st = D.PHASE[raw]; out = np.empty(len(raw)); prev = 0.0
    for k in range(len(raw)):
        if low[k]:
            cq = int(st[k]) // 64; dq = (int(prev) // 64 - cq) % 4
            out[k] = cq * 64 + 64 - inner if dq == 1 else cq * 64 + inner if dq == 3 else st[k]
        else:
            out[k] = st[k]
        prev = out[k]
    return out, low

noise = D.chan(D.white)[::2]; noise *= D.SIG_CELL * np.sqrt(2) / np.sqrt(np.mean(np.abs(noise) ** 2))
sig = D.chan(D.s80)[::2]; sig /= np.sqrt(np.mean(np.abs(sig) ** 2))
cfgs = [('static', None, m) for m in (4, 8, 12, 16, 24)] + [('hc', inner, m) for inner in (1, 4, 8, 16) for m in (8, 12, 16)]
print('cfg                 ' + '  '.join(f'C/N{c:>3}' for c in (2, 4, 6, 8, 10, 14)) + '   (SINAD dB / clicks)')
for name, inner, m in cfgs:
    cells = []
    for cnr in (2, 4, 6, 8, 10, 14):
        raw = D.raw_bytes(sig * D.SIG_CELL * np.sqrt(2) * np.sqrt(10 ** (cnr / 10)) + noise)
        ph, low = (D.dec_static(raw), None) if name == 'static' else dec_hc_edge(raw, inner)
        tot, bands, clicks = D.score(D.out_span50(ph, low, 'clamp', margin=m))
        cells.append(f'{tot:4.1f}/{clicks:4.0f}')
    print(f'{name}{"" if inner is None else inner:>3} m{m:<3}       ' + '  '.join(cells), flush=True)
