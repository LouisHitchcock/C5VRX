#!/usr/bin/env python3
"""C5VRX-5 by Twotoz and the C5VRX contributors: GPIO live view.

Every GPIO of the ESP32-S3 Super Mini, live, to find the pad the joystick
ladder is really on. Needs the gpioscan firmware (firmware/,
pio run -e gpioscan -t upload). On connect it
sends 'g', and the board streams one frame per pass over every pin: the
digital level with the internal pull-down and pull-up, and the mV with no
pull on ADC pins.

Each tile shows what's on the pin:
  floating   follows the internal pull, nothing attached (grey)
  tied high  reads 1 against the pull-down, e.g. an external pull-up (red)
  tied low   reads 0 against the pull-up (blue)
ADC pins that aren't floating also show mV, a history trace and the ladder
button that voltage decodes to. A pin that changes lights up yellow for a
couple of seconds and goes in the log, so press the switch and watch.

    python tools/joystick_menu/gpio_gui.py [PORT]
"""

import queue
import sys
import threading
import time
import tkinter as tk
from collections import deque
from tkinter import ttk

import serial
import serial.tools.list_ports

COLUMNS = 6
TILE_W, TILE_H = 170, 110
HISTORY = 120  # frames of mV kept per pin
CHANGE_MV = 250
HIGHLIGHT_S = 2.0

COLOURS = {
    "floating": "#3a3a3a",
    "tied high": "#7a2a2a",
    "tied low": "#2a3f7a",
    "?": "#6a4a1a",
}

# Midpoint bounds from src/main.cpp, upper mV for each level.
LADDER = [("UP", 1195), ("DOWN", 1660), ("LEFT", 2125), ("RIGHT", 2495), ("MID", 2900)]


def decode_ladder(mv):
    for name, upper in LADDER:
        if mv < upper:
            return name
    return "none"


def describe(down, up):
    if not down and up:
        return "floating"
    if down and up:
        return "tied high"
    if not down and not up:
        return "tied low"
    return "?"


def parse_frame(line):
    pins = {}
    for entry in line.split()[1:]:
        try:
            pin, down, up, mv = (int(x) for x in entry.split(":"))
        except ValueError:
            return None
        pins[pin] = (down, up, mv)
    return pins


class Reader(threading.Thread):
    def __init__(self, port, frames, log):
        super().__init__(daemon=True)
        self.port = port
        self.frames = frames
        self.log = log
        self.stop = threading.Event()

    def run(self):
        try:
            ser = serial.Serial(self.port, 115200, timeout=0.5)
        except serial.SerialException as e:
            self.log.put(f"Can't open {self.port}: {e}")
            return
        ser.dtr = True
        time.sleep(0.2)
        ser.reset_input_buffer()
        ser.write(b"g")
        self.log.put(f"Connected to {self.port}")
        buf = b""
        last_frame = time.time()
        while not self.stop.is_set():
            try:
                buf += ser.read(ser.in_waiting or 1)
            except serial.SerialException as e:
                self.log.put(f"Serial error: {e}")
                break
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                line = raw.decode(errors="replace").strip()
                if line.startswith("F "):
                    pins = parse_frame(line)
                    if pins:
                        self.frames.put(pins)
                        last_frame = time.time()
            # The board may have rebooted out of stream mode; ask again.
            if time.time() - last_frame > 2:
                ser.write(b"g")
                last_frame = time.time()
        try:
            ser.write(b"x")
            ser.close()
        except serial.SerialException:
            pass
        self.log.put("Disconnected")


class Tile:
    def __init__(self, parent, pin, analog):
        self.pin = pin
        self.analog = analog
        self.canvas = tk.Canvas(parent, width=TILE_W, height=TILE_H, highlightthickness=3,
                                highlightbackground="#202020", bg=COLOURS["floating"])
        self.history = deque(maxlen=HISTORY)
        self.state = None
        self.settled_mv = None
        self.lit_until = 0.0

    def update(self, down, up, mv, now):
        state = describe(down, up)
        changed = None
        if self.state is not None and state != self.state:
            changed = f"{self.state} -> {state}"
        self.state = state

        if self.analog and state != "floating":
            self.history.append(mv)
            if self.settled_mv is None:
                self.settled_mv = mv
            elif abs(mv - self.settled_mv) > CHANGE_MV:
                changed = changed or f"{self.settled_mv} -> {mv} mV ({decode_ladder(mv)})"
                self.settled_mv = mv
        else:
            self.history.clear()
            self.settled_mv = None

        if changed:
            self.lit_until = now + HIGHLIGHT_S
        self.draw(mv, now)
        return changed

    def draw(self, mv, now):
        c = self.canvas
        c.delete("all")
        c.configure(bg=COLOURS[self.state],
                    highlightbackground="#ffd200" if now < self.lit_until else "#202020")
        c.create_text(8, 6, anchor="nw", text=f"GPIO{self.pin}", fill="white",
                      font=("Segoe UI", 12, "bold"))
        c.create_text(TILE_W - 8, 8, anchor="ne", text=self.state, fill="#dddddd",
                      font=("Segoe UI", 9))
        if not self.analog:
            c.create_text(8, 32, anchor="nw", text="digital only", fill="#aaaaaa",
                          font=("Segoe UI", 9))
            return
        if self.state == "floating":
            return
        label = f"{mv} mV" if mv <= 3150 else f"{mv} mV (max)"
        c.create_text(8, 30, anchor="nw", text=label, fill="white", font=("Consolas", 13, "bold"))
        c.create_text(TILE_W - 8, 32, anchor="ne", text=decode_ladder(mv), fill="#ffd200",
                      font=("Segoe UI", 10, "bold"))
        # History trace, 0-3300 mV.
        top, bottom = 56, TILE_H - 6
        if len(self.history) > 1:
            step = (TILE_W - 12) / (HISTORY - 1)
            points = []
            for i, v in enumerate(self.history):
                y = bottom - (min(v, 3300) / 3300) * (bottom - top)
                points += [6 + i * step, y]
            c.create_line(*points, fill="#7fffd4", width=2)
        c.create_line(6, bottom, TILE_W - 6, bottom, fill="#555555")


class App:
    def __init__(self, root, port):
        self.root = root
        self.frames = queue.Queue()
        self.log_q = queue.Queue()
        self.reader = None
        self.tiles = {}
        root.title("S3 Super Mini GPIO view")
        root.configure(bg="#181818")

        bar = ttk.Frame(root, padding=6)
        bar.pack(fill="x")
        ttk.Label(bar, text="Port").pack(side="left")
        self.port_var = tk.StringVar(value=port or "")
        self.port_box = ttk.Combobox(bar, textvariable=self.port_var, width=40)
        self.port_box.pack(side="left", padx=4)
        ttk.Button(bar, text="Refresh", command=self.refresh_ports).pack(side="left")
        self.connect_btn = ttk.Button(bar, text="Connect", command=self.toggle)
        self.connect_btn.pack(side="left", padx=4)
        self.hide_floating = tk.BooleanVar(value=False)
        ttk.Checkbutton(bar, text="Hide floating pins", variable=self.hide_floating,
                        command=self.layout).pack(side="left", padx=12)
        self.rate_var = tk.StringVar(value="")
        ttk.Label(bar, textvariable=self.rate_var).pack(side="right")

        self.grid = tk.Frame(root, bg="#181818")
        self.grid.pack(padx=6, pady=6)

        self.log = tk.Listbox(root, height=8, bg="#101010", fg="#dddddd",
                              font=("Consolas", 10))
        self.log.pack(fill="both", expand=True, padx=6, pady=(0, 6))

        self.frame_times = deque(maxlen=30)
        self.refresh_ports()
        if port:
            self.toggle()
        root.after(50, self.poll)

    def refresh_ports(self):
        ports = [f"{p.device}  {p.description}  {p.serial_number or ''}"
                 for p in serial.tools.list_ports.comports()]
        self.port_box["values"] = ports
        if not self.port_var.get() and ports:
            self.port_var.set(ports[0])

    def toggle(self):
        if self.reader:
            self.reader.stop.set()
            self.reader = None
            self.connect_btn.configure(text="Connect")
            return
        port = self.port_var.get().split()[0] if self.port_var.get() else ""
        if not port:
            return
        self.reader = Reader(port, self.frames, self.log_q)
        self.reader.start()
        self.connect_btn.configure(text="Disconnect")

    def add_log(self, text):
        self.log.insert(0, f"{time.strftime('%H:%M:%S')}  {text}")
        self.log.delete(300, "end")

    def layout(self):
        for tile in self.tiles.values():
            tile.canvas.grid_forget()
        shown = [t for _, t in sorted(self.tiles.items())
                 if not (self.hide_floating.get() and t.state == "floating")]
        for i, tile in enumerate(shown):
            tile.canvas.grid(row=i // COLUMNS, column=i % COLUMNS, padx=3, pady=3)

    def poll(self):
        while not self.log_q.empty():
            self.add_log(self.log_q.get())

        pins = None
        while not self.frames.empty():
            pins = self.frames.get()
            self.frame_times.append(time.time())
        if pins:
            now = time.time()
            relayout = False
            for pin, (down, up, mv) in pins.items():
                tile = self.tiles.get(pin)
                if tile is None:
                    tile = self.tiles[pin] = Tile(self.grid, pin, analog=mv >= 0)
                    relayout = True
                was = tile.state
                changed = tile.update(down, up, mv, now)
                if changed:
                    self.add_log(f"GPIO{pin}: {changed}")
                if was != tile.state:
                    relayout = True
            if relayout:
                self.layout()
            else:
                for tile in self.tiles.values():
                    if tile.lit_until and now >= tile.lit_until:
                        tile.lit_until = 0.0
                        tile.canvas.configure(highlightbackground="#202020")
            if len(self.frame_times) > 1:
                span = self.frame_times[-1] - self.frame_times[0]
                if span > 0:
                    self.rate_var.set(f"{(len(self.frame_times) - 1) / span:.0f} frames/s")
        self.root.after(50, self.poll)


def main():
    root = tk.Tk()
    App(root, sys.argv[1] if len(sys.argv) > 1 else None)
    root.mainloop()


if __name__ == "__main__":
    main()
