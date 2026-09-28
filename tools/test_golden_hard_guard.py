#!/usr/bin/env python3
"""Exercise the generated C5 BSASM against an independent phase-state oracle."""

from pathlib import Path
import importlib.util
import random

from gen_golden_hard_guard import BASE, TARGET, REJECT_BINS, build, wrap32


MODEL_PATH = Path(__file__).resolve().parents[1] / "legacy/c5vrx2/tools/bs_model.py"
spec = importlib.util.spec_from_file_location("bs_model", MODEL_PATH)
model = importlib.util.module_from_spec(spec)
spec.loader.exec_module(model)


def expand(token):
    if ".." not in token:
        return [token]
    first, last = token.split("..")
    prefix = first[0] if first[0].isalpha() else ""
    start = int(first[len(prefix):])
    end = int(last[len(prefix):])
    return [prefix + str(bit) for bit in range(start, end + 1)]


def run_bsasm(source, raw, writes):
    _, lut, blocks, labels = model.parse(source)
    assert len(blocks) == 5
    out = look = position = pc = 0
    result = []
    bundles = 0
    while len(result) < writes:
        previous_out, previous_look = out, look
        next_out = 0
        read = write = 0
        opcode = ["nop"]
        used = set()
        for line in blocks[pc]:
            parts = line.split()
            if parts[0] == "set":
                targets, sources = expand(parts[1]), expand(parts[2])
                if len(sources) == 1:
                    sources *= len(targets)
                assert len(targets) == len(sources)
                for target, source in zip(targets, sources):
                    bit = int(target)
                    assert bit not in used
                    used.add(bit)
                    if source == "h":
                        value = 1
                    elif source == "l":
                        value = 0
                    elif source[0] == "o":
                        value = (previous_out >> int(source[1:])) & 1
                    elif source[0] == "l":
                        value = (previous_look >> int(source[1:])) & 1
                    else:
                        offset = position + int(source) // 8
                        value = (raw[offset] >> (int(source) % 8)) & 1
                    next_out |= value << bit
            elif parts[0] == "read":
                read = int(parts[1])
            elif parts[0] == "write":
                write = int(parts[1])
            else:
                opcode = parts
        pc += 1
        if opcode[0] == "jmp":
            pc = labels[opcode[1]]
        elif opcode[0] == "if":
            source = opcode[1]
            assert source.startswith("l")
            if (previous_look >> int(source[1:])) & 1:
                pc = labels[opcode[2]]
        else:
            assert opcode[0] == "nop"
        out = next_out
        look = lut[(out >> 16) & 1023]
        position += read // 8
        bundles += 1
        if write:
            assert write == 16 and bundles == 2 * (len(result) + 1)
            code = out & 255
            assert code < 64 and ((out >> 8) & 255) == code
            result.append(code)
    return result


def de_bruijn(alphabet_size, order):
    a = [0] * (alphabet_size * order)
    sequence = []

    def visit(t, p):
        if t > order:
            if order % p == 0:
                sequence.extend(a[1:p + 1])
        else:
            a[t] = a[t - p]
            visit(t + 1, p)
            for value in range(a[t - p] + 1, alphabet_size):
                a[t] = value
                visit(t + 1, t)

    visit(1, 1)
    return sequence + sequence[:order]


def oracle(phases, golden):
    outputs = []
    trusted = phases[0]
    reseed = False
    for current in phases[1:]:
        if reseed:
            outputs.append(20)
            trusted = current
            reseed = False
        elif abs(wrap32(current - trusted)) >= REJECT_BINS:
            outputs.append(20)
            reseed = True
        else:
            outputs.append(golden[(trusted << 5) | current] & 63)
            trusted = current
    return outputs


def main():
    source = build()
    assert TARGET.read_text(encoding="utf-8") == source
    _, golden, _, _ = model.parse(BASE.read_text(encoding="utf-8"))
    _, guarded, _, _ = model.parse(source)
    representatives = [next(raw for raw in range(256)
                            if ((golden[raw] >> 8) & 31) == phase)
                       for phase in range(32)]
    assert len(set(representatives)) == 32
    for previous in range(32):
        for current in range(32):
            address = (previous << 5) | current
            reject = abs(wrap32(current - previous)) >= REJECT_BINS
            assert (guarded[address] & 63) == (golden[address] & 63)
            assert bool(guarded[address] & 64) == reject
    assert all((golden[(p << 5) | c] & 63) == 20
               for p in range(32) for c in range(32)
               if abs(wrap32(c - p)) >= REJECT_BINS)

    rng = random.Random(0xC5)
    sequences = [de_bruijn(32, 3)]
    sequences.extend([rng.randrange(32) for _ in range(5000)]
                     for _ in range(3))
    for phases in sequences:
        raw = bytearray()
        for phase in phases:
            raw.extend((0, representatives[phase]))
        raw.extend(b"\0\0\0\0")
        observed = run_bsasm(source, raw, len(phases) + 1)
        expected = oracle(phases, golden)
        assert observed[2:2 + len(expected)] == expected
    print("golden hard guard: 32^3 phase triples, 15k random phases, exact DAC and two bundles PASS")


if __name__ == "__main__":
    main()
