#!/usr/bin/env python3
"""C5VRX-5 by Twotoz and the C5VRX contributors: joystick ladder check.

Guided press-each-button check for the SKRHABE010 resistor ladder. Needs the
gpioscan firmware (firmware/). Close gpio_gui.py first, only one program can
hold the port.

Prompts for each direction, waits for the press to show up on the pin, records
a second of raw ADC counts and the firmware's mV conversion, then waits for
release. A direction that doesn't register within the timeout is recorded as
no response and the routine moves on. The report compares raw against mV so a
bad mV conversion can be told apart from a wiring fault, and suggests raw
thresholds for the firmware.

    python tools/joystick_menu/ladder_check.py PORT [gpio]      gpio defaults to 11
"""

import statistics
import sys
import time
from pathlib import Path

import serial

if len(sys.argv) < 2:
    sys.exit(__doc__)
PORT = sys.argv[1]
PIN = int(sys.argv[2]) if len(sys.argv) > 2 else 11

BUTTONS = [("UP", 1.055), ("DOWN", 1.336), ("LEFT", 1.985), ("RIGHT", 2.269), ("MID", 2.721)]
PRESS_DELTA = 120  # raw counts away from idle that count as pressed
SETTLE_S = 0.15
RECORD_S = 1.0
TIMEOUT_S = 20.0
RESULT_FILE = Path(__file__).resolve().parent / "build" / "ladder_check_result.txt"


class Stream:
    def __init__(self, port, pin):
        self.ser = serial.Serial(port, 115200, timeout=0.5)
        self.ser.dtr = True
        time.sleep(0.3)
        self.ser.reset_input_buffer()
        self.ser.write(f"a{pin}\n".encode())

    def read(self):
        """Next (raw, mV) sample, or None on timeout."""
        line = self.ser.readline().decode(errors="replace").split()
        if len(line) == 3 and line[0] == "A":
            return int(line[1]), int(line[2])
        return None

    def close(self):
        self.ser.write(b"x")
        self.ser.close()


def collect(stream, seconds):
    samples = []
    end = time.time() + seconds
    while time.time() < end:
        s = stream.read()
        if s:
            samples.append(s)
    return samples


def summarise(samples):
    raws = [r for r, _ in samples]
    mvs = [m for _, m in samples]
    return {
        "raw": statistics.mean(raws), "raw_min": min(raws), "raw_max": max(raws),
        "mv": statistics.mean(mvs), "mv_min": min(mvs), "mv_max": max(mvs), "n": len(samples),
    }


def wait_for(stream, pressed, idle_raw, hold_s):
    """Wait until the pin has been pressed (or released) for hold_s. False on timeout."""
    end = time.time() + TIMEOUT_S
    since = None
    while time.time() < end:
        s = stream.read()
        if not s:
            continue
        away = abs(s[0] - idle_raw) > PRESS_DELTA
        if away == pressed:
            since = since or time.time()
            if time.time() - since >= hold_s:
                return True
        else:
            since = None
    return False


def main():
    print(f"Ladder check on GPIO{PIN} via {PORT}. Close gpio_gui.py if it's open.\n")
    try:
        stream = Stream(PORT, PIN)
    except serial.SerialException as e:
        sys.exit(f"Can't open {PORT}: {e}")

    lines = []

    def out(text=""):
        print(text)
        lines.append(text)

    try:
        print("Hands off the switch, measuring idle...")
        idle_samples = collect(stream, 1.5)
        if not idle_samples:
            sys.exit("No samples. Is the gpioscan firmware flashed?")
        idle = summarise(idle_samples)
        print(f"  idle raw {idle['raw']:.0f}  mV {idle['mv']:.0f}\n")

        results = {}
        for name, volts in BUTTONS:
            print(f">>> Press and HOLD {name}")
            if not wait_for(stream, True, idle["raw"], SETTLE_S):
                print(f"    no response from {name} in {TIMEOUT_S:.0f} s, skipping\n")
                results[name] = None
                continue
            samples = collect(stream, RECORD_S)
            results[name] = summarise(samples) if samples else None
            r = results[name]
            print(f"    got it: raw {r['raw']:.0f}  mV {r['mv']:.0f}. Release.")
            if not wait_for(stream, False, idle["raw"], 0.3):
                print("    still not back at idle, carrying on")
            print()
    finally:
        stream.close()

    out(f"=== Ladder check, GPIO{PIN}, {time.strftime('%Y-%m-%d %H:%M:%S')} ===")
    out(f"idle: raw {idle['raw']:.0f} ({idle['raw_min']}-{idle['raw_max']}), "
        f"mV {idle['mv']:.0f} ({idle['mv_min']}-{idle['mv_max']})")
    out()
    out("button  expected V  raw mean  raw range    mV mean  mV range     mV/expected")
    for name, volts in BUTTONS:
        r = results[name]
        if r is None:
            out(f"{name:<6}  {volts:10.2f}  NO RESPONSE")
            continue
        out(f"{name:<6}  {volts:10.2f}  {r['raw']:8.0f}  {r['raw_min']:4}-{r['raw_max']:<4}  "
            f"{r['mv']:8.0f}  {r['mv_min']:4}-{r['mv_max']:<4}  {r['mv'] / (volts * 1000):10.2f}")

    # Raw thresholds: midpoints between neighbouring measured levels, idle last.
    seen = [(name, results[name]["raw"]) for name, _ in BUTTONS if results[name]]
    out()
    if len(seen) == len(BUTTONS):
        levels = [raw for _, raw in seen] + [idle["raw"]]
        if levels != sorted(levels):
            out("Raw levels are out of order, check the resistor values.")
        bounds = [round((a + b) / 2) for a, b in zip(levels, levels[1:])]
        gaps = [b - a for a, b in zip(levels, levels[1:])]
        out("Suggested raw upper bounds (UP, DOWN, LEFT, RIGHT, MID): " +
            ", ".join(str(b) for b in bounds))
        out(f"Smallest gap between levels: {min(gaps):.0f} counts")
    else:
        missing = [name for name, _ in BUTTONS if not results[name]]
        out("No response from: " + ", ".join(missing))
        out("Raw didn't move either, so it's wiring rather than the mV conversion: check those "
            "resistors and their switch pins.")

    RESULT_FILE.parent.mkdir(exist_ok=True)
    RESULT_FILE.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"\nSaved to {RESULT_FILE}")
    if sys.stdin.isatty():
        input("Press Enter to close.")


if __name__ == "__main__":
    main()
