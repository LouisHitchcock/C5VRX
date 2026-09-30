#!/usr/bin/env python3
"""Blind A/B of the native AGC policy profiles (main/agc_policy.h).

capture  Cycles the profiles (serial '1'..'4' = NATIVE/INIT/TUNED/HOLD) in a
         shuffled order, several rounds, lets each settle, records P8ENV rows
         ('E') and asks the operator to rate the picture WITHOUT showing which
         profile is active (g=good, n=noisy, b=bad, x=no picture). Keep the
         VTX, antenna and distance fixed for the whole run. The log keeps the
         profile names, so nothing depends on the operator's memory.

           python tools/agc_ab.py capture --port COM10 --log agc_ab.log
           python tools/agc_ab.py capture --port COM10 --no-rate   # unattended

analyze  Per profile: picture votes and medians of the Q4 envelope, Phase8
         hard tail, native state switching/restart proxies, pinned gain and
         transport faults.

           python tools/agc_ab.py analyze agc_ab.log

The first row after a switch is discarded: it can straddle the transition.
Metrics are evidence, the picture votes decide: sync_q/std_valid proved
unreliable as picture indicators on hardware (docs/phase8-range-envelope.md).
"""

import argparse
import random
import re
import statistics
import sys
import time

PROFILES = {"1": "NATIVE", "2": "INIT", "3": "TUNED", "4": "HOLD"}
ROW_RE = re.compile(r"\bP8ENV\s+(.*)$")
TAG_RE = re.compile(r"\bAGCAB_BLOCK\s+profile=(\w+)\s+round=(\d+)(?:\s+picture=(\w+))?")
KV_RE = re.compile(r"(\w+)=(\S+)")
PICTURE_KEYS = {"g": "good", "n": "noisy", "b": "bad", "x": "none"}
PICTURE_SCORE = {"good": 3, "noisy": 2, "bad": 1, "none": 0}
METRICS = ("p50", "p95", "origin_pm", "central_pm", "clip_pm", "coh_pm", "hard_pm",
           "agc_state", "agc_state_sw_per_ms_x10", "agc_start_reentry_per_ms_x10",
           "agc_hold", "agc_init", "strength", "sync_q")
FAULTS = ("rx_ovf", "tx_empty", "gdma_in", "gdma_out", "bs_empty", "bs_eof")


def parse_row(text):
    row = {}
    for key, value in KV_RE.findall(text):
        try:
            row[key] = int(value, 16) if value.startswith("0x") else int(value)
        except ValueError:
            row[key] = value
    return row


def parse_log(lines):
    blocks = []
    current = None
    for line in lines:
        tag = TAG_RE.search(line)
        if tag:
            current = {"profile": tag.group(1), "round": int(tag.group(2)),
                       "picture": tag.group(3), "rows": []}
            blocks.append(current)
            continue
        row = ROW_RE.search(line)
        if row and current is not None:
            current["rows"].append(parse_row(row.group(1)))
    return blocks


def summarize(blocks):
    out = {}
    for block in blocks:
        rows = block["rows"][1:]  # first row may straddle the switch
        s = out.setdefault(block["profile"], {"rows": [], "pictures": [], "mismatch": 0,
                                              "fault_delta": 0})
        for row in rows:
            if row.get("agc_prof") not in (None, block["profile"]):
                s["mismatch"] += 1
                continue
            s["rows"].append(row)
        if block["picture"]:
            s["pictures"].append(block["picture"])
        if len(block["rows"]) >= 2:
            first, last = block["rows"][0], block["rows"][-1]
            s["fault_delta"] += sum(max(0, last.get(k, 0) - first.get(k, 0)) for k in FAULTS)
    table = {}
    for profile, s in out.items():
        entry = {"n": len(s["rows"]), "mismatch": s["mismatch"],
                 "faults": s["fault_delta"], "pictures": s["pictures"]}
        entry["picture_score"] = (statistics.mean(PICTURE_SCORE[p] for p in s["pictures"])
                                  if s["pictures"] else None)
        for key in METRICS:
            values = [r[key] for r in s["rows"] if isinstance(r.get(key), int)]
            entry[key] = statistics.median(values) if values else None
        table[profile] = entry
    return table


def rank(table):
    def key(item):
        e = item[1]
        score = e["picture_score"] if e["picture_score"] is not None else -1
        hard = e["hard_pm"] if e["hard_pm"] is not None else 10**6
        return (-score, hard)
    return [p for p, _ in sorted(table.items(), key=key)]


def print_table(table):
    cols = ("n", "picture_score", "p50", "p95", "origin_pm", "clip_pm", "coh_pm",
            "hard_pm", "agc_state_sw_per_ms_x10", "agc_start_reentry_per_ms_x10",
            "agc_hold", "agc_init", "strength", "faults")
    short = ("n", "pic", "p50", "p95", "orig", "clip", "coh", "hard", "sw/ms10",
             "restart", "hold", "init", "str", "fault")
    print("profile  " + " ".join(f"{c:>7}" for c in short))
    for profile in rank(table):
        e = table[profile]
        cells = []
        for c in cols:
            v = e[c]
            cells.append("      -" if v is None else
                         f"{v:7.2f}" if isinstance(v, float) and not v.is_integer() else f"{int(v):7d}")
        print(f"{profile:<8} " + " ".join(cells))
        if e["mismatch"]:
            print(f"  warning: {e['mismatch']} rows reported another profile (switch not applied?)")
        if e["pictures"]:
            print(f"  pictures: {' '.join(e['pictures'])}")
    carrierless = [p for p, e in table.items() if e["coh_pm"] is not None and e["coh_pm"] < 50]
    if carrierless:
        print("warning: low coherence (no carrier?) for " + ", ".join(carrierless) +
              "; check ch=/mhz= and the VTX before trusting this run")


def capture(args):
    import serial  # pyserial; only needed on the bench
    keys = [k for k in args.profiles.split(",") if k in PROFILES]
    order = []
    rng = random.Random(args.seed)
    for rnd in range(args.rounds):
        block = keys[:]
        rng.shuffle(block)
        order += [(k, rnd) for k in block]
    port = serial.Serial(args.port, 115200, timeout=0.2)
    time.sleep(0.3)
    port.reset_input_buffer()
    log = open(args.log, "a", encoding="utf-8")

    def read_for(seconds):
        end = time.time() + seconds
        buf = b""
        while time.time() < end:
            buf += port.read(4096)
        text = buf.decode(errors="replace")
        log.write(text)
        log.flush()
        return text

    print(f"{len(order)} blocks of ~{args.settle + args.rows * 3:.0f} s. Keep VTX and distance fixed.")
    for n, (key, rnd) in enumerate(order, 1):
        port.write(key.encode())
        read_for(args.settle)
        picture = None
        if not args.no_rate:
            while picture is None:
                answer = input(f"[{n}/{len(order)}] picture? g=good n=noisy b=bad x=none: ").strip().lower()
                picture = PICTURE_KEYS.get(answer[:1]) if answer else None
        tag = f"AGCAB_BLOCK profile={PROFILES[key]} round={rnd}" + (f" picture={picture}" if picture else "")
        log.write(tag + "\n")
        for _ in range(args.rows):
            port.write(b"E")
            read_for(3.0)
        if args.no_rate:
            print(f"[{n}/{len(order)}] done")
    port.write(b"1")  # back to production NATIVE
    read_for(1.0)
    log.close()
    with open(args.log, encoding="utf-8", errors="replace") as f:
        print_table(summarize(parse_log(f)))


def analyze(args):
    lines = []
    for path in args.logs:
        with open(path, encoding="utf-8", errors="replace") as f:
            lines += f.readlines()
    print_table(summarize(parse_log(lines)))


def self_test():
    def row(prof, p50, hard, sw, tx=0):
        return (f"P8ENV ch=A1 mhz=5865 agc_prof={prof} p50={p50} p95={p50*2} origin_pm=10 "
                f"central_pm=5 clip_pm=0 coh_pm=900 hard_pm={hard} agc_state=50 "
                f"agc_state_sw_per_ms_x10={sw} agc_start_reentry_per_ms_x10=8 agc_hold=-1 "
                f"agc_init=82 strength=60 sync_q=80 tx_empty={tx}\n")
    log = ["AGCAB_BLOCK profile=NATIVE round=0 picture=noisy\n",
           row("NATIVE", 6, 99, 150), row("NATIVE", 6, 30, 150), row("NATIVE", 7, 32, 140),
           "AGCAB_BLOCK profile=HOLD round=0 picture=good\n",
           row("NATIVE", 6, 30, 150), row("HOLD", 18, 5, 0), row("HOLD", 19, 6, 0, tx=2),
           "AGCAB_BLOCK profile=INIT round=0\n",
           row("INIT", 6, 20, 90), row("INIT", 6, 21, 90), row("TUNED", 6, 21, 90)]
    table = summarize(parse_log(log))
    assert table["NATIVE"]["n"] == 2 and table["NATIVE"]["hard_pm"] == 31
    assert table["HOLD"]["n"] == 2 and table["HOLD"]["p50"] == 18.5
    assert table["HOLD"]["faults"] == 2
    assert table["INIT"]["mismatch"] == 1 and table["INIT"]["picture_score"] is None
    assert rank(table)[0] == "HOLD" and rank(table)[1] == "NATIVE"
    print("agc_ab self-test passed")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--self-test", action="store_true")
    sub = ap.add_subparsers(dest="cmd")
    c = sub.add_parser("capture")
    c.add_argument("--port", required=True)
    c.add_argument("--log", default="agc_ab.log")
    c.add_argument("--profiles", default="1,2,3,4", help="serial keys 1..4")
    c.add_argument("--rounds", type=int, default=2)
    c.add_argument("--rows", type=int, default=4, help="P8ENV rows per block")
    c.add_argument("--settle", type=float, default=6.0, help="seconds after a switch")
    c.add_argument("--seed", type=int, default=None)
    c.add_argument("--no-rate", action="store_true", help="unattended, metrics only")
    a = sub.add_parser("analyze")
    a.add_argument("logs", nargs="+")
    args = ap.parse_args()
    if args.self_test:
        self_test()
    elif args.cmd == "capture":
        capture(args)
    elif args.cmd == "analyze":
        analyze(args)
    else:
        ap.print_help()
        sys.exit(2)


if __name__ == "__main__":
    main()
