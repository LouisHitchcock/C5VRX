#!/usr/bin/env python3
"""Executable reference for a compact adjacent-sample Phase5 discriminator.

This models a proposed LUT *input contract*. It does not model a working
BitScrambler schedule or change the live receiver program.
"""

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def wrap32(value: int) -> int:
    return ((value + 16) & 31) - 16


def winding_dac6(travel: int) -> int:
    """Candidate P20/G2 correction for trips outside the endpoint arc."""
    return min(63, max(0, 20 + 2 * travel))


def address(first_delta: int, endpoint_delta: int) -> int:
    """Two independently formed modulo-32 differences, ten address bits."""
    return ((first_delta & 31) << 5) | (endpoint_delta & 31)


def travel_from_address(key: int) -> int:
    first = wrap32(key >> 5)
    endpoint = wrap32(key & 31)
    return first + wrap32(endpoint - first)


def golden_lut() -> tuple[int, ...]:
    words = next(
        line.split()[1:]
        for line in (ROOT / "main/fm.bsasm").read_text(encoding="utf-8").splitlines()
        if line.startswith("lut ")
    )
    assert len(words) == 1024
    return tuple(int(word) & 63 for word in words)


def build_lut(golden: tuple[int, ...]) -> tuple[int, ...]:
    # A 10-bit (a,g) address has no room for absolute P. Use the median of
    # Golden's 32 calibrated endpoint-pair codes when winding is absent.
    median = tuple(
        sorted(golden[(p << 5) | ((p + delta) & 31)] for p in range(32))[15]
        for delta in range(32)
    )
    return tuple(
        median[key & 31] if travel_from_address(key) == wrap32(key & 31)
        else winding_dac6(travel_from_address(key))
        for key in range(1024)
    )


def verify() -> dict[str, int]:
    golden = golden_lut()
    lut = build_lut(golden)
    assert len(lut) == 1024 and all(0 <= code <= 63 for code in lut)

    seen = set()
    windings = 0
    non_windings = 0
    for previous in range(32):
        for middle in range(32):
            for current in range(32):
                first = wrap32(middle - previous)
                endpoint = wrap32(current - previous)
                key = address(first, endpoint)
                seen.add(key)
                expected = first + wrap32(current - middle)
                assert travel_from_address(key) == expected
                assert lut[key] == (median_golden(golden, endpoint)
                                    if expected == endpoint else winding_dac6(expected))
                if expected != endpoint:
                    windings += 1
                else:
                    non_windings += 1

    assert len(seen) == 1024  # Every word is reachable; no spare LUT addresses.
    assert (non_windings, windings) == (24576, 8192)
    # The endpoints alone cannot select the correct output.
    assert travel_from_address(address(wrap32(10), wrap32(20))) == 20
    assert travel_from_address(address(wrap32(20), wrap32(20))) == -12

    golden_errors = [
        abs(lut[address(wrap32(m - p), wrap32(c - p))] - golden[(p << 5) | c])
        for p in range(32) for m in range(32) for c in range(32)
        if wrap32(m - p) + wrap32(c - m) == wrap32(c - p)
    ]
    return {
        "triplets": 32768,
        "reachable_lut_words": len(seen),
        "non_winding": non_windings,
        "winding": windings,
        "non_winding_dac_differences_from_golden": sum(error != 0 for error in golden_errors),
        "max_non_winding_dac_error": max(golden_errors),
    }


def median_golden(golden: tuple[int, ...], delta: int) -> int:
    values = sorted(golden[(p << 5) | ((p + delta) & 31)] for p in range(32))
    return values[15]


if __name__ == "__main__":
    for name, value in verify().items():
        print(f"{name}: {value}")
