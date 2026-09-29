#!/usr/bin/env python3
"""Exhaustive source-driven oracle, including state at stream boundaries."""
from gen_phase8_video import build, TARGET, HEADER, dac
from test_phase8_hr import de_bruijn_2, model, expand

def simulate(source, raw):
    cfg, lut, blocks, _ = model.parse(source)
    assert len(blocks) == 8 and len(lut) == 1024
    assert cfg['trailing_bytes'] == '0' and cfg['eof_on'] == 'downstream'
    out = look = pos = 0
    result = []
    for pc in range(len(raw)):
        new = 0
        used = set()
        for line in blocks[pc & 7]:
            p = line.split()
            if p[0] == 'set':
                ds, ss = expand(p[1]), expand(p[2])
                if len(ss) == 1: ss *= len(ds)
                for d, s in zip(ds, ss):
                    d = int(d)
                    assert d not in used
                    used.add(d)
                    v = ((out if s[0] == 'o' else look) >> int(s[1:])) & 1 if s[0] in 'ol' else (raw[pos + int(s)//8] >> (int(s)%8)) & 1
                    new |= v << d
            else:
                assert p[0] in ('read', 'write') and p[1] == '16'
        out = new
        look = lut[(out >> 16) & 1023]
        if pc & 1:
            assert out & 255 == (out >> 8) & 255
            result.append(out & 255)
            pos += 2
    return result

def main():
    asm, header, state, reps, codes = build()
    assert TARGET.read_text() == asm and HEADER.read_text() == header
    seq = de_bruijn_2(256)
    raw = bytearray(b for r in seq for b in (0, r)) + b'\0\0\0\0'
    observed = simulate(asm, raw)
    for k in range(1, len(seq)):
        assert observed[k+1] == codes[(state[seq[k-1]] << 5) | state[seq[k]]]
        assert 0 <= observed[k+1] <= 63
    assert dac(0) == 20 and dac(-32) == 0 and dac(64) == 63
    assert dac(-128) == dac(127) == 20
    assert all(0 <= dac(d) <= 63 for d in range(-128, 128))
    # Monotone throughout the calibrated useful interval (outside tail taper).
    assert all(dac(d) <= dac(d+1) for d in range(-96, 96))
    print('Phase8 VIDEO32: 65,536 raw pairs, two bundles, duplicated DAC, bounded gain PASS')

if __name__ == '__main__': main()
