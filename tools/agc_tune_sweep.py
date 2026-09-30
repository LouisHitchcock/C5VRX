#!/usr/bin/env python3
"""Step through the native AGC register candidates (rf.c s_agc_tunes) on a
probe build and measure each boot's native acquisitions per sample.

Each serial '8' persists the next candidate and reboots. The probe build
prints AGC_TUNE (which candidate is active) and three full 80 MS/s dump
windows (AGC_WORDS) ~8 s after boot. Per candidate the table reports:
acquisitions per ms, time share in acquisition, median acquisition
duration, trapped gain spread and the trapped IQ radius / out-of-window
share. The port is opened with DTR/RTS deasserted (they are the reset and
boot straps of the C5 USB-Serial/JTAG). The sweep ends back on index 0.

Keep the VTX on one channel, fixed and still, for the whole run.

  python tools/agc_tune_sweep.py --port COM10 --log agc_tune.log
  python tools/agc_tune_sweep.py --analyze agc_tune.log
"""

import argparse
import re
import sys
import time
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import analyze_agc_words as aw  # noqa: E402

TUNE_RE = re.compile(r"AGC_TUNE idx=(\d+) count=(\d+) name=(\S+)")


def open_port(port):
    import serial
    s = serial.Serial()
    s.port, s.baudrate, s.timeout, s.write_timeout = port, 115200, 0.1, 2
    s.dtr = False
    s.rts = False
    s.open()
    return s


def capture_boot(port, log, send=None, timeout=60.0):
    """Optionally send a key, then collect until the third AGC window ends."""
    buf = b""
    s = None
    end = time.time() + timeout
    sent = send is None
    while time.time() < end:
        try:
            if s is None:
                s = open_port(port)
            if not sent:
                s.write(send.encode())
                sent = True
            chunk = s.read(65536)
            if chunk:
                buf += chunk
                log.write(chunk)
                log.flush()
            if b"AGC_WORDS END window=2" in buf and b"AGC_TUNE idx=" in buf:
                break
        except Exception:
            try:
                s.close()
            except Exception:
                pass
            s = None
            time.sleep(0.3)
    if s is not None:
        s.close()
    return buf.decode(errors="replace")


def summarize(text):
    tune = TUNE_RE.findall(text)
    windows = aw.parse(text)
    if not tune or not windows:
        return None
    idx, count, name = tune[-1]
    r = aw.analyze(windows)
    gains = [int(np.bincount(aw.fields(w)[2]).argmax()) for w in windows]
    ms = r["samples"] / aw.FS_HZ * 1e3
    seg_r = [rad for *_, rad in r["segments"]]
    return {
        "idx": int(idx), "count": int(count), "name": name,
        "acq_per_ms": r["events"] / ms if ms else 0.0,
        "acq_share": r["acq_share"],
        "acq_us": float(np.median(r["duration_us"])) if len(r["duration_us"]) else 0.0,
        "gains": gains,
        "radius_p50": float(np.median(seg_r)) if seg_r else float("nan"),
        "radius_spread": float(np.ptp(seg_r)) if len(seg_r) > 1 else 0.0,
        "trap_out256": r["trap_out256"] or 0.0,
    }


def split_boots(text):
    parts = re.split(r"(?=AGC_TUNE idx=)", text)
    return [p for p in parts if p.startswith("AGC_TUNE idx=")]


def table(rows):
    print(f"{'idx':>3} {'candidate':<18} {'acq/ms':>7} {'acq%':>6} {'acq us':>7} "
          f"{'radius':>7} {'r spread':>8} {'out256':>7}  window gains")
    for r in rows:
        print(f"{r['idx']:>3} {r['name']:<18} {r['acq_per_ms']:7.1f} {100 * r['acq_share']:6.1f} "
              f"{r['acq_us']:7.2f} {r['radius_p50']:7.0f} {r['radius_spread']:8.0f} "
              f"{1000 * r['trap_out256']:7.1f}  {r['gains']}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port")
    ap.add_argument("--log", default="agc_tune.log")
    ap.add_argument("--analyze", metavar="LOG")
    args = ap.parse_args()
    if args.analyze:
        rows = [s for s in (summarize(b) for b in split_boots(Path(args.analyze).read_text(errors="replace"))) if s]
        table(rows)
        return
    if not args.port:
        ap.error("--port or --analyze required")
    rows = []
    with open(args.log, "ab") as log:
        # Current boot first (whatever candidate is active), via a restart.
        first = summarize(capture_boot(args.port, log, send="8"))
        if first is None:
            sys.exit("no AGC_TUNE/AGC_WORDS seen: is the probe build flashed?")
        rows.append(first)
        table(rows[-1:])
        for _ in range(first["count"] - 1):
            row = summarize(capture_boot(args.port, log, send="8"))
            if row is None:
                print("boot without complete capture; continuing")
                continue
            rows.append(row)
            table(rows[-1:])
            if row["idx"] == 0:
                break
    print()
    table(sorted(rows, key=lambda r: r["idx"]))


if __name__ == "__main__":
    main()
