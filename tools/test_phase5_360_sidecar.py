#!/usr/bin/env python3
"""Check a Golden-DAC correction overlay and its delayed-pair alignment.

Source-domain model only: no clock-domain crossing, phase lock or board timing.
"""

from prove_golden360_capacity import GOLDEN, interval_winding, winding


PASS, FORCE_LOW, FORCE_HIGH = range(3)


def action(previous, middle, current):
    k = interval_winding(previous, middle, current)
    return FORCE_HIGH if k > 0 else FORCE_LOW if k < 0 else PASS


def overlay(golden, correction):
    return golden if correction == PASS else 0 if correction == FORCE_LOW else 63


def exact(previous, middle, current):
    golden = GOLDEN[(previous << 5) | current] & 63
    k = winding(previous, middle, current)
    return golden if k == 0 else 63 if k > 0 else 0


def find_unique_golden_lag(source_golden, tapped_golden, candidates, end,
                           window=256):
    """Illustrative fail-closed lock gate for source-synchronous captures."""
    observed = tapped_golden[end - window:end]
    if len(observed) != window or sum(a != b for a, b in zip(observed, observed[1:])) < 32:
        return None
    scores = []
    for lag in candidates:
        predicted = source_golden[end - window - lag:end - lag]
        if len(predicted) == window:
            scores.append((sum(a != b for a, b in zip(predicted, observed)), lag))
    scores.sort()
    if len(scores) < 2 or scores[0][0] != 0 or scores[1][0] < 32:
        return None
    return scores[0][1]


def main():
    for previous in range(32):
        for middle in range(32):
            for current in range(32):
                golden = GOLDEN[(previous << 5) | current] & 63
                assert overlay(golden, action(previous, middle, current)) == exact(
                    previous, middle, current)
    print("PASS/LOW/HIGH overlay: 32768/32768 exact Phase5 triplets")

    # 16,384 raw bytes of nominal half-ring RX/TX separation correspond to
    # 8,192 output pairs. Startup/driver timing introduces unknown extra lag.
    pairs = 20000
    nominal_lag = 8192
    actual_lag = nominal_lag + 11
    state = 0x72A5C3
    phase_triplets = []
    previous = 0
    for _ in range(pairs):
        state = (1664525 * state + 1013904223) & 0xffffffff
        middle = (state >> 16) & 31
        state = (1664525 * state + 1013904223) & 0xffffffff
        current = (state >> 16) & 31
        phase_triplets.append((previous, middle, current))
        previous = current

    matching = differing = false_even_with_single_code_guard = 0
    for tx_pair in range(actual_lag, pairs):
        source = tx_pair - actual_lag
        p, m, c = phase_triplets[source]
        golden = GOLDEN[(p << 5) | c] & 63
        expected = exact(p, m, c)
        assert overlay(golden, action(*phase_triplets[source])) == expected
        matching += 1
        # One-pair misalignment is unsafe even though each independent
        # correction is mathematically exact for its own source triplet.
        wrong = overlay(golden, action(*phase_triplets[source + 1]))
        differing += wrong != expected
        other_p, _other_m, other_c = phase_triplets[source + 1]
        other_golden = GOLDEN[(other_p << 5) | other_c] & 63
        # A same-code coincidence can pass a one-pair guard, hence the need
        # for an independently earned multi-sample alignment lock.
        guarded_wrong = wrong if other_golden == golden else golden
        false_even_with_single_code_guard += guarded_wrong != expected
    assert matching == pairs - actual_lag and differing > 1000
    assert false_even_with_single_code_guard > 0
    print(f"Delayed Golden alignment: {matching} exact pairs at lag {actual_lag};"
          f" lag+1 corrupts {differing} pairs ({false_even_with_single_code_guard}"
          " even after same-code gating)")

    source_golden = [GOLDEN[(p << 5) | c] & 63
                     for p, _m, c in phase_triplets]
    observed_golden = ([20] * actual_lag +
                       source_golden[:pairs - actual_lag])
    candidates = range(nominal_lag - 32, nominal_lag + 33)
    end = actual_lag + 700
    assert find_unique_golden_lag(source_golden, observed_golden,
                                  candidates, end) == actual_lag
    assert find_unique_golden_lag(source_golden, [20] * pairs,
                                  candidates, end) is None
    slipped = observed_golden[:end - 100] + [20] + observed_golden[end - 100:-1]
    assert find_unique_golden_lag(source_golden, slipped,
                                  candidates, end) is None
    print("Illustrative Golden correlation: correct lag locks;"
          " flat output and one-pair slip fail closed")


if __name__ == "__main__":
    main()
