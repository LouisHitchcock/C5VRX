#!/usr/bin/env python3
"""Capture full AGC words and calibrate transition errors on an unmodulated tone.

Examples:
  python tools/agc_transition.py capture --port COM10 --log tone.log
  python tools/agc_transition.py calibrate tone.log --tone-khz 0 --output tone.json
  python tools/agc_transition.py evaluate other_tone.log --model tone.json

The calibration is an offline research model. It is not uploaded to firmware:
aligned live gain metadata and realtime compensation remain hardware work.
Never calibrate on an unknown video waveform: wanted FM would be learned as error.
"""
import argparse
import hashlib
import json
import math
import re
import time
from pathlib import Path

import numpy as np

from analyze_agc_words import fields, events, FS_HZ


def wrap(x):
    return (x + np.pi) % (2 * np.pi) - np.pi


def windows(text):
    """Accept only ended windows with exact sizes and unique ordered chunks.

    Window numbers can repeat in later sessions; never merge by window ID.
    Retain the capture hash and keep every window separate in all processing.
    """
    active = None
    result = []
    for line in text.splitlines():
        begin = re.fullmatch(
            r"AGC_WORDS BEGIN window=(\d+) words=(\d+) stop_ptr=(\d+)", line.strip())
        if begin:
            active = [tuple(map(int, begin.groups())), [], False]
            continue
        chunk = re.fullmatch(
            r"AGC_WORDS_HEX window=(\d+) chunk=(\d+) hex=([0-9a-fA-F]+)", line.strip())
        if chunk and active:
            number, index, data = chunk.groups()
            if (int(number) != active[0][0] or int(index) != len(active[1]) or
                    len(data) != 256 * 8):
                active[2] = True
            active[1].append(data)
            continue
        end = re.fullmatch(r"AGC_WORDS END window=(\d+)", line.strip())
        if end and active:
            number, count, ptr = active[0]
            raw = "".join(active[1])
            if (not active[2] and int(end[1]) == number and count == 8192 and
                    0 <= ptr < count and len(raw) == count * 8):
                w = np.array([int(raw[i:i + 8], 16) for i in range(0, len(raw), 8)],
                             dtype=np.uint32)
                result.append(np.roll(w, -ptr))
            active = None
    return result


def digest(w):
    return hashlib.sha256(w.astype("<u4").tobytes()).hexdigest()


def decode(w, min_radius):
    i, q, gain, state = fields(w)
    z = i.astype(float) + 1j * q.astype(float)
    phase_delta = np.angle(z[1:] * np.conj(z[:-1]))
    # True ADC endpoint rail risk, distinct from the +/-256 fine-lane limit.
    good = (np.maximum(np.abs(i), np.abs(q)) < 511) & (np.abs(z) >= min_radius)
    valid_pairs = good[1:] & good[:-1]
    return phase_delta, valid_pairs, gain, state


def calibrate(captures, tone_khz, min_radius, minimum, max_mad):
    expected = 2 * np.pi * tone_khz * 1000 / FS_HZ
    buckets = {}
    for w in captures:
        delta, valid, gain, _ = decode(w, min_radius)
        change = np.flatnonzero(np.diff(gain) != 0) + 1
        error = wrap(delta - expected)
        # Learn the local response through 0.4 us after each switch, with
        # 50 ns of lead-in for diagnostic metadata/filter alignment.
        for j, n in enumerate(change):
            old, new = int(gain[n - 1]), int(gain[n])
            previous = int(change[j - 1]) if j else 0
            following = int(change[j + 1]) if j + 1 < len(change) else len(w)
            for offset in range(-4, 33):
                sample = int(n + offset)
                # Exclude windows crossing another switch; no overlapping
                # transition templates and no deltas across capture gaps.
                if sample <= previous or sample >= following or sample <= 0:
                    continue
                if valid[sample - 1]:
                    buckets.setdefault((old, new, offset), []).append(float(error[sample - 1]))
    records = []
    for (old, new, offset), values in sorted(buckets.items()):
        if len(values) < minimum:
            continue
        mean = float(np.angle(np.mean(np.exp(1j * np.asarray(values)))))
        mad = float(np.median(np.abs(wrap(np.asarray(values) - mean))))
        if mad <= max_mad:
            records.append(dict(old=old, new=new, offset=offset,
                                error_rad=mean, mad_rad=mad, observations=len(values)))
    return dict(schema=1, stimulus="unmodulated_tone", tone_khz=tone_khz,
                sample_rate_hz=FS_HZ, min_radius=min_radius,
                minimum_observations=minimum, maximum_mad_rad=max_mad,
                training_hashes=[digest(w) for w in captures], records=records,
                live_compatible=False)


def evaluate(captures, model):
    if (model.get("schema") != 1 or model.get("stimulus") != "unmodulated_tone" or
            model.get("sample_rate_hz") != FS_HZ):
        raise ValueError("unsupported model or sample rate")
    hashes = set(model["training_hashes"])
    if any(digest(w) in hashes for w in captures):
        raise ValueError("evaluation overlaps training; use independent tone captures")
    expected = 2 * np.pi * model["tone_khz"] * 1000 / FS_HZ
    records = model["records"]
    table = {(int(r["old"]), int(r["new"]), int(r["offset"])): float(r["error_rad"])
             for r in records}
    before, after = [], []
    touched = total = 0
    for w in captures:
        delta, valid, gain, _ = decode(w, model["min_radius"])
        error = wrap(delta - expected)
        correction = np.zeros_like(error)
        change = np.flatnonzero(np.diff(gain) != 0) + 1
        for j, n in enumerate(change):
            old, new = int(gain[n - 1]), int(gain[n])
            previous = int(change[j - 1]) if j else 0
            following = int(change[j + 1]) if j + 1 < len(change) else len(w)
            for offset in range(-4, 33):
                sample = int(n + offset)
                value = table.get((old, new, offset))
                if (value is not None and previous < sample < following and sample > 0
                        and valid[sample - 1]):
                    correction[sample - 1] = value
                    touched += 1
        before.extend(error[valid].tolist())
        after.extend(wrap(error - correction)[valid].tolist())
        total += len(error)
    if not before:
        raise ValueError("no trustworthy tone sample pairs")
    return dict(windows=len(captures), valid_pairs=len(before), corrected_pairs=touched,
                masked_pairs=total - len(before),
                before_rms_rad=float(np.sqrt(np.mean(np.square(before)))),
                after_rms_rad=float(np.sqrt(np.mean(np.square(after)))),
                claim="independent constant-tone evaluation only; not video quality")


def acquisition_report(captures):
    """Timing within each capture only; gap-grouping is an event proxy.

    Never infer the step dwell from time between separate acquisitions or
    unrelated windows. Report total occupied time as well as shorter steps.
    """
    steps, durations, intervals = [], [], []
    count = occupied = total = adc_rails = outside_fine = 0
    for w in captures:
        i, q, gain, _ = fields(w)
        changes = np.flatnonzero(np.diff(gain) != 0) + 1
        clusters = events(gain, int(4e-6 * FS_HZ))
        mask = np.zeros(len(w), dtype=bool)
        starts = []
        for a, b in clusters:
            # Exclude clusters touching a boundary from duration statistics.
            mask[max(0, a - 8):b + 1] = True
            starts.append(a)
            count += 1
            if a > 8 and b < len(w) - 8:
                durations.append((b - a) / FS_HZ * 1e6)
                subset = changes[(changes >= a) & (changes <= b)]
                steps.extend((np.diff(subset) / FS_HZ * 1e6).tolist())
        intervals.extend((np.diff(starts) / FS_HZ * 1e6).tolist())
        occupied += int(mask.sum())
        total += len(w)
        amp = np.maximum(np.abs(i), np.abs(q))
        adc_rails += int((amp >= 511).sum())
        outside_fine += int((amp >= 256).sum())
    def median(values):
        return float(np.median(values)) if values else None
    return dict(windows=len(captures), samples=total, gain_change_clusters=count,
                cluster_rate_per_ms=count / (total / FS_HZ * 1000),
                gain_step_median_us=median(steps), gain_step_observations=len(steps),
                change_cluster_duration_median_us=median(durations),
                cluster_interval_median_us=median(intervals),
                change_cluster_share_pm=1000 * occupied / total,
                adc_endpoint_rail_pm=1000 * adc_rails / total,
                fine_window_overflow_pm=1000 * outside_fine / total,
                note="4 us gap grouping; cluster duration excludes final settling; no cross-window intervals")


def capture(port, log_path, timeout, timing=None):
    import serial
    session = serial.Serial()
    session.port, session.baudrate = port, 115200
    session.timeout, session.write_timeout = 0.2, 2
    session.dtr = session.rts = False
    try:
        session.open()
        session.reset_input_buffer()
        command = b"}" if timing == "vendor" else b"{" if timing == "next" else b"@"
        session.write(command)  # Explicit native lab request; never force gain.
        deadline = time.monotonic() + timeout
        data = bytearray()
        with Path(log_path).open("wb") as log:
            while time.monotonic() < deadline:
                try:
                    if not session.is_open:
                        session.open()
                    chunk = session.read(65536)
                except (serial.SerialException, OSError):
                    session.close()
                    time.sleep(0.2)
                    continue
                if chunk:
                    data.extend(chunk)
                    log.write(chunk)
                    log.flush()
                if b"AGC_CAPTURE END restored=1" in data:
                    decoded = windows(data.decode(errors="replace"))
                    if len(decoded) != 8:
                        raise ValueError(f"incomplete capture: {len(decoded)}/8 windows")
                    print(f"Captured eight separate full-word windows to {log_path}")
                    return
        raise ValueError("capture timed out; inspect log and firmware version")
    finally:
        session.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    cap = sub.add_parser("capture")
    cap.add_argument("--port", default="COM10")
    cap.add_argument("--log", required=True)
    cap.add_argument("--timeout", type=float, default=90)
    cap.add_argument("--timing", choices=("vendor", "next"),
                     help="restore vendor or advance one native acquisition field profile")
    cal = sub.add_parser("calibrate")
    cal.add_argument("logs", nargs="+")
    cal.add_argument("--tone-khz", type=float, required=True,
                     help="known constant baseband frequency of an UNMODULATED carrier")
    cal.add_argument("--output", required=True)
    cal.add_argument("--min-radius", type=float, default=16)
    cal.add_argument("--minimum", type=int, default=8)
    cal.add_argument("--max-mad", type=float, default=0.12)
    ev = sub.add_parser("evaluate")
    ev.add_argument("logs", nargs="+")
    ev.add_argument("--model", required=True)
    report = sub.add_parser("report")
    report.add_argument("logs", nargs="+")
    args = parser.parse_args()
    try:
        if args.command == "capture":
            if args.timeout <= 0:
                raise ValueError("timeout must be positive")
            capture(args.port, args.log, args.timeout, args.timing)
            return
        if args.command == "report":
            for p in args.logs:
                text = Path(p).read_text(errors="replace")
                captures = windows(text)
                if not captures:
                    raise ValueError(f"{p}: no complete full-word AGC windows")
                rows = re.findall(r"(?m)^AGC_ACQ (.+)$", text)
                profiles = [dict(re.findall(r"(\w+)=(\S+)", row)) for row in rows]
                if len({r.get("idx") for r in profiles}) > 1:
                    raise ValueError(f"{p}: mixed profiles; keep each capture in a separate file")
                print(json.dumps(dict(log=p, profile=profiles[-1] if profiles else None,
                                      **acquisition_report(captures)), indent=2))
            return
        captures = [w for p in args.logs for w in windows(Path(p).read_text(errors="replace"))]
        if not captures:
            raise ValueError("no complete full-word AGC windows")
        if args.command == "calibrate":
            if (not math.isfinite(args.tone_khz) or abs(args.tone_khz) >= FS_HZ / 2000 or
                    not math.isfinite(args.min_radius) or args.min_radius <= 0 or
                    args.minimum < 8 or not 0 < args.max_mad < np.pi):
                raise ValueError("invalid calibration parameters (minimum >=8)")
            unique = {digest(w): w for w in captures}
            model = calibrate(list(unique.values()), args.tone_khz, args.min_radius,
                              args.minimum, args.max_mad)
            if not model["records"]:
                raise ValueError("no repeatable transitions; collect more independent tone windows")
            Path(args.output).write_text(json.dumps(model, indent=2) + "\n", encoding="utf-8")
            print(f"Saved {len(model['records'])} offline correction entries to {args.output}")
        else:
            model = json.loads(Path(args.model).read_text())
            print(json.dumps(evaluate(captures, model), indent=2))
    except (ValueError, OSError) as exc:
        parser.exit(1, f"agc_transition: {exc}\n")


if __name__ == "__main__":
    main()
