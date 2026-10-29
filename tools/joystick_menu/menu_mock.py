#!/usr/bin/env python3
"""C5VRX-5 by Twotoz and the C5VRX contributors: joystick menu mock.

The C5VRX goggle menu on the desktop, driven by an SKRHABE010 5-way switch
read by an ESP32-S3 (firmware/), or by the keyboard. The picture is the real
one: main/menu_ui.c is compiled with menu_shim.c into build/menu_ui.dll (.so)
and shown through tools/menu_preview.py as a goggle would. Receiver state
(signal, scan, calibration, filter, ...) is simulated here; nothing talks to
a receiver. See README.md.

Controls (the firmware has one button: press = next row, hold = change it):
  menu closed     MID opens the menu, LEFT/RIGHT channel, UP/DOWN band
  menu open       UP/DOWN move, RIGHT or MID change forward, LEFT back,
                  hold MID to close, EXIT closes
  CALIBRATE       MID opens QUICK / FULL / BACK; LEFT leaves it
  keyboard        arrows, Enter or space for MID
30 s without a press closes the menu, like the firmware.

    python tools/joystick_menu/menu_mock.py [COMx | /dev/ttyACMx | none]
"""

import argparse
import ctypes
import importlib.util
import queue
import random
import re
import shutil
import subprocess
import sys
import threading
import time
import tkinter as tk
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFont, ImageTk

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent

# main/menu_ui.h order.
ITEMS = ["BAND", "CHANNEL", "SCAN", "VIDEO", "FILTER", "PHY FILT", "AFC", "CALIBRATE", "OUTPUT", "GAIN", "DAC",
         "EXIT"]
(M_BAND, M_CHANNEL, M_SCAN, M_VIDEO, M_FILTER, M_DFILT, M_AFC, M_CALIBRATE, M_OUTPUT, M_GAIN, M_DAC,
 M_EXIT) = range(len(ITEMS))
CAL_QUICK, CAL_FULL, CAL_BACK = range(3)

# main/rf.c
CHANNELS = [
    ("R", [5658, 5695, 5732, 5769, 5806, 5843, 5880, 5917]),
    ("A", [5865, 5845, 5825, 5805, 5785, 5765, 5745, 5725]),
    ("B", [5733, 5752, 5771, 5790, 5809, 5828, 5847, 5866]),
    ("E", [5705, 5685, 5665, 5645, 5885, 5905, 5925, 5945]),
    ("F", [5740, 5760, 5780, 5800, 5820, 5840, 5860, 5880]),
    ("L", [5362, 5399, 5436, 5473, 5510, 5547, 5584, 5621]),
]
BAND_NAMES = ["RACEBAND R", "BOSCAM A", "BOSCAM B", "BOSCAM E", "FATSHARK F", "LOWBAND L"]
# main/transport.c, main/receiver.c (LEVEL, KEEL and RSSI are automatic modes)
OUTPUTS = ["1.0x", "1.5x", "2.0x", "2.5x", "3.0x", "3.5x", "4.0x", "4.5x", "5.0x", "0.5x"]
GAIN_MODES = ["LEVEL", "KEEL", "RSSI", "MAX", "G78", "G72", "G66", "G60", "G54", "G48", "G42", "G36", "G30", "G24"]
GAIN_AUTO = {"LEVEL", "KEEL", "RSSI"}
GAIN_MAX = 78
# profile_logic.h profile_select_next order; widths are made up for the mock.
FILTERS = ["AUTO", "XNARROW", "NARROW", "NORMAL", "WIDE"]
FILTER_WIDTH_KHZ = {"XNARROW": 9500, "NARROW": 12000, "NORMAL": 14375, "WIDE": 18000}
DFILT_MODES = [12, 0, 4, 8]         # phy_dfilt_next from the default 12

# Pretend VTXs for the signal and the scan: (band, channel) -> signal.
VTX_CHANNELS = {(0, 0): 82, (0, 3): 55, (1, 1): 40}

LONG_PRESS_S = 0.6
REPEAT_DELAY_S = 0.4
REPEAT_S = 0.15
MENU_IDLE_S = 30.0

# The ladder (firmware/src/joystick.cpp).
LADDER = [("UP", 1055), ("DOWN", 1336), ("LEFT", 1985), ("RIGHT", 2269), ("MID", 2721)]
BOUNDS = [1195, 1660, 2125, 2495, 2900]


# ---------------------------------------------------------------- renderer --

class ShimView(ctypes.Structure):
    """menu_shim.c shim_view_t."""
    _fields_ = [(name, ctypes.c_char_p if kind == "s" else ctypes.c_int) for name, kind in (
        ("cursor", "i"), ("cal_sub", "i"), ("channel", "s"), ("freq_mhz", "i"), ("band", "i"),
        ("channel_in_band", "i"), ("band_name", "s"), ("signal", "i"), ("video", "i"), ("afc_locked", "i"),
        ("afc_off", "i"), ("level_valid", "i"), ("level_dbm", "i"), ("scan_active", "i"), ("scan_progress", "i"),
        ("std_mode", "i"), ("std_out_pal", "i"), ("detected_valid", "i"), ("detected_pal", "i"), ("output", "i"),
        ("output_name", "s"), ("gain_mode", "s"), ("gain_auto", "i"), ("gain", "i"), ("gain_max", "i"),
        ("filter_mode", "s"), ("filter_active", "s"), ("filter_recommended", "s"), ("filter_width_khz", "i"),
        ("narrow_ok", "i"), ("wide_ok", "i"), ("dfilt_mode", "i"), ("dfilt_default", "i"),
        ("deviation_x100", "i"), ("offset_khz", "i"), ("cn_x10", "i"), ("firmware", "s"))]


class Renderer:
    """main/menu_ui.c, rebuilt when it or its headers change."""

    def __init__(self):
        main = REPO / "main"
        sources = [HERE / "menu_shim.c", main / "menu_ui.c"]
        headers = [main / n for n in ("menu_ui.h", "menu_font.h", "menu_logo.h", "menu_raster.h", "dac_check.h")]
        lib = HERE / "build" / ("menu_ui.dll" if sys.platform == "win32" else "menu_ui.so")
        newest = max(p.stat().st_mtime for p in sources + headers)
        if not lib.exists() or lib.stat().st_mtime < newest:
            cc = shutil.which("gcc") or shutil.which("cc") or shutil.which("clang")
            if not cc:
                raise SystemExit("No C compiler (gcc, cc or clang) found to build the menu renderer.")
            lib.parent.mkdir(exist_ok=True)
            subprocess.run([cc, "-O2", "-shared", "-fPIC", "-std=c11", f"-I{main}", *map(str, sources), "-o",
                            str(lib)], check=True)
        self.lib = ctypes.CDLL(str(lib))
        if self.lib.shim_items() != len(ITEMS):
            raise SystemExit(f"main/menu_ui.h has {self.lib.shim_items()} menu items, this mock knows "
                             f"{len(ITEMS)}: update ITEMS in menu_mock.py.")
        self.lines, self.bytes = self.lib.shim_lines(), self.lib.shim_bytes()
        self.buf = (ctypes.c_uint8 * (self.lines * self.bytes))()
        self.lib.shim_render.argtypes = [ctypes.c_void_p, ctypes.POINTER(ShimView)]
        s, i = ctypes.c_char_p, ctypes.c_int
        self.lib.shim_render_status.argtypes = [ctypes.c_void_p, s, s, s, i, s, ctypes.c_uint]
        self.lib.shim_render_splash.argtypes = [ctypes.c_void_p, i]

        spec = importlib.util.spec_from_file_location("menu_preview", REPO / "tools" / "menu_preview.py")
        self.preview = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(self.preview)

    def _rows(self):
        return np.frombuffer(self.buf, dtype=np.uint8).reshape(self.lines, self.bytes).copy()

    def menu(self, rx):
        enc = lambda text: text.encode() if text is not None else None
        filt = rx.filter_active
        v = ShimView(
            cursor=rx.cursor, cal_sub=rx.cal_sub, channel=enc(rx.channel_name), freq_mhz=rx.freq, band=rx.band,
            channel_in_band=rx.chan, band_name=enc(BAND_NAMES[rx.band]), signal=rx.signal, video=rx.video,
            afc_locked=rx.afc and rx.video, afc_off=not rx.afc, level_valid=1, level_dbm=rx.level_dbm,
            scan_active=rx.scan_active, scan_progress=rx.scan_progress, std_mode=rx.std_mode,
            std_out_pal=rx.std_mode != 1, detected_valid=rx.video, detected_pal=1, output=rx.output,
            output_name=enc(OUTPUTS[rx.output]), gain_mode=enc(GAIN_MODES[rx.gain_mode]),
            gain_auto=GAIN_MODES[rx.gain_mode] in GAIN_AUTO, gain=rx.gain, gain_max=GAIN_MAX,
            filter_mode=enc(FILTERS[rx.filter]), filter_active=enc(filt),
            filter_recommended=enc("NORMAL" if rx.video else None), filter_width_khz=FILTER_WIDTH_KHZ[filt],
            narrow_ok=1, wide_ok=1, dfilt_mode=DFILT_MODES[rx.dfilt], dfilt_default=rx.dfilt == 0,
            deviation_x100=73 if rx.video else -1, offset_khz=81 if rx.afc and rx.video else 0,
            cn_x10=rx.signal * 3 - 60 if rx.video else -990, firmware=b"5.0.0-mock")
        self.lib.shim_render(self.buf, ctypes.byref(v))
        return self._rows()

    def status(self, title, line1, line2, percent, rx):
        self.lib.shim_render_status(self.buf, title.encode(), line1.encode(), line2.encode(), percent,
                                    rx.channel_name.encode(), rx.freq)
        return self._rows()

    def splash(self, percent):
        self.lib.shim_render_splash(self.buf, percent)
        return self._rows()

    def picture(self, rows, height):
        return self.preview.to_image(self.preview.frame(rows, 4.2), False, height)


# ------------------------------------------------------------------- model --

class Receiver:
    """Simulated receiver state and the menu actions of main/control.c."""

    def __init__(self):
        self.band, self.chan = 0, 0
        self.menu_open = False
        self.cursor, self.cal_sub = 0, -1
        self.std_mode = 0           # AUTO, NTSC, PAL
        self.filter = 0
        self.dfilt = 0
        self.afc = True
        self.output = 1
        self.gain_mode = 0
        self.gain = 42
        self.scan_active, self.scan_progress = False, 0
        self.status = None          # (title, line1, line2, percent) while calibrating
        self.cal_t0, self.cal_s = 0.0, 6.0
        self.splash_until = 0.0
        self.last_input = time.time()
        self.signal, self.level_dbm = 0, -100
        self.log = []

    channel_name = property(lambda self: f"{CHANNELS[self.band][0]}{self.chan + 1}")
    freq = property(lambda self: CHANNELS[self.band][1][self.chan])
    video = property(lambda self: self.signal > 20)
    filter_active = property(lambda self: "NORMAL" if self.filter == 0 else FILTERS[self.filter])

    def note(self, text):
        self.log.append(text)

    def tick(self, now):
        base = 0 if self.scan_active else VTX_CHANNELS.get((self.band, self.chan), 0)
        self.signal = int(max(0, min(100, base + (random.uniform(-4, 4) if base else random.uniform(0, 6)))))
        self.level_dbm = int(-100 + self.signal * 0.6 + random.uniform(-1, 1))
        mode = GAIN_MODES[self.gain_mode]
        if mode in GAIN_AUTO:
            self.gain = int(max(3, min(GAIN_MAX, 70 - self.signal * 0.4 + random.uniform(-1, 1))))
        else:
            self.gain = GAIN_MAX if mode == "MAX" else int(mode[1:])

        if self.scan_active:
            self.scan_progress = min(100, self.scan_progress + 2)
            step = self.scan_progress * 48 // 100
            if step < 48:
                self.band, self.chan = divmod(step, 8)
            else:
                self.finish_scan()
        if self.status:
            spent = now - self.cal_t0
            if self.status[0] == "CALIBRATING":
                if spent >= self.cal_s:
                    self.status = ("FILTER CALIBRATED", "NARROW FILTER STORED", "", 100)
                    self.cal_t0 = now
                else:
                    left = int(self.cal_s - spent) + 1
                    self.status = ("CALIBRATING", "KEEP THE VTX OFF", f"ABOUT {left} S LEFT, THEN LIVE VIDEO",
                                   int(spent / self.cal_s * 100))
            elif spent > 2.0:
                self.status = None
                self.note("calibration done, live video")
        if self.menu_open and now - self.last_input > MENU_IDLE_S:
            self.menu_open, self.cal_sub = False, -1
            self.note(f"menu closed after {MENU_IDLE_S:.0f} s idle")

    def start_scan(self):
        self.scan_active, self.scan_progress = True, 0
        self.note("scan started")

    def finish_scan(self):
        self.scan_active = False
        self.band, self.chan = max(VTX_CHANNELS.items(), key=lambda kv: kv[1])[0]
        self.note(f"scan found {self.channel_name} {self.freq} MHz")

    def calibrate(self, full):
        self.menu_open, self.cal_sub = False, -1
        self.cal_s = 12.0 if full else 6.0
        self.cal_t0 = time.time()
        self.status = ("CALIBRATING", "KEEP THE VTX OFF", "", 0)
        self.note(f"calibrate {'FULL' if full else 'QUICK'}: menu closed, filter calibration")

    def step(self, item, d):
        """Cycle an item's value by d. False when the item has no value."""
        if item == M_BAND:
            self.band = (self.band + d) % len(CHANNELS)
        elif item == M_CHANNEL:
            self.chan = (self.chan + d) % 8
        elif item == M_VIDEO:
            self.std_mode = (self.std_mode + d) % 3
        elif item == M_FILTER:
            self.filter = (self.filter + d) % len(FILTERS)
        elif item == M_DFILT:
            self.dfilt = (self.dfilt + d) % len(DFILT_MODES)
        elif item == M_AFC:
            self.afc = not self.afc
        elif item == M_OUTPUT:
            self.output = (self.output + d) % len(OUTPUTS)
        elif item == M_GAIN:
            self.gain_mode = (self.gain_mode + d) % len(GAIN_MODES)
        else:
            return False
        return True

    def action(self, forward):
        if self.cal_sub >= 0:
            if not forward or self.cal_sub == CAL_BACK:
                self.cal_sub = -1
            else:
                self.calibrate(self.cal_sub == CAL_FULL)
            return
        c = self.cursor
        if self.step(c, 1 if forward else -1) or not forward:
            return
        if c == M_SCAN:
            self.start_scan()
        elif c == M_CALIBRATE:
            self.cal_sub = CAL_QUICK
        elif c == M_DAC:
            self.note("DAC TEST: the bench meter steps are not simulated")
        elif c == M_EXIT:
            self.menu_open = False
            self.note("menu closed")

    def press(self, button, long=False):
        """A press, a long MID hold, or an auto-repeat."""
        self.last_input = time.time()
        if self.splash_until > time.time() or self.status or self.scan_active:
            return
        if not self.menu_open:
            if button == "MID" and not long:
                self.menu_open, self.cursor, self.cal_sub = True, 0, -1
                self.note("menu opened")
            elif button in ("LEFT", "RIGHT"):
                self.step(M_CHANNEL, 1 if button == "RIGHT" else -1)
            elif button in ("UP", "DOWN"):
                self.step(M_BAND, 1 if button == "DOWN" else -1)
            return
        if button == "MID" and long:
            self.menu_open, self.cal_sub = False, -1
            self.note("menu closed (held MID)")
        elif button in ("UP", "DOWN"):
            d = 1 if button == "DOWN" else -1
            if self.cal_sub >= 0:
                self.cal_sub = (self.cal_sub + d) % 3
            else:
                self.cursor = (self.cursor + d) % len(ITEMS)
        elif button in ("RIGHT", "MID"):
            self.action(True)
        elif button == "LEFT":
            self.action(False)


# ------------------------------------------------------------------- input --

class SerialInput(threading.Thread):
    """firmware/src/joystick.cpp: 'press X' / 'release X' and the raw mV stream."""

    EVENT = re.compile(r"^(press|release)\s+(\w+)")
    RAW = re.compile(r"^\s*(\d+) mV\s+(\w+)")

    def __init__(self, port, out):
        super().__init__(daemon=True)
        self.port, self.out = port, out
        self.stop = threading.Event()

    def run(self):
        try:
            import serial
            ser = serial.Serial(self.port, 115200, timeout=0.2)
        except ImportError:
            self.out.put(("log", "pyserial not installed, keyboard only"))
            return
        except Exception as e:
            self.out.put(("log", f"Can't open {self.port}: {e}. Keyboard only."))
            return
        ser.dtr = True
        self.out.put(("log", f"Joystick on {self.port}"))
        last_raw, asked, buf = 0.0, time.time(), b""
        while not self.stop.is_set():
            try:
                buf += ser.read(ser.in_waiting or 1)
            except Exception as e:
                self.out.put(("log", f"Serial error: {e}"))
                return
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                line = raw.decode(errors="replace").strip()
                if m := self.EVENT.match(line):
                    self.out.put((m.group(1), m.group(2)))
                elif m := self.RAW.match(line):
                    self.out.put(("mv", int(m.group(1))))
                    last_raw = time.time()
            # 'r' toggles the firmware's raw stream: ask again whenever it is off.
            now = time.time()
            if now - last_raw > 1.0 and now - asked > 1.5:
                ser.write(b"r")
                asked = now
        try:
            if time.time() - last_raw < 1.0:
                ser.write(b"r")
            ser.close()
        except Exception:
            pass


# --------------------------------------------------------------------- GUI --

class App:
    PIC_H = 480

    def __init__(self, root, renderer, port):
        self.root, self.r = root, renderer
        self.rx = Receiver()
        self.rx.splash_until = time.time() + 1.5
        self.events = queue.Queue()
        self.held = {}              # button -> [press time, next repeat, long fired]
        self.mv = None
        self.serial = None
        root.title("C5VRX menu mock + SKRHABE010 joystick")
        root.configure(bg="#161616")

        left = tk.Frame(root, bg="#161616")
        left.pack(side="left", fill="y", padx=10, pady=10)
        self.stick = tk.Canvas(left, width=300, height=300, bg="#161616", highlightthickness=0)
        self.stick.pack()
        self.meter = tk.Canvas(left, width=300, height=110, bg="#161616", highlightthickness=0)
        self.meter.pack(pady=(6, 0))
        tk.Label(left, justify="left", bg="#161616", fg="#9a9a9a", font=("Segoe UI", 9),
                 text="Closed: MID open, L/R channel, U/D band\n"
                      "Open: U/D move, R/MID change, L back,\n"
                      "hold MID close.  Keys: arrows, Enter").pack(anchor="w", pady=6)
        self.log = tk.Listbox(left, height=9, width=44, bg="#0e0e0e", fg="#cfcfcf", font=("Consolas", 9),
                              highlightthickness=0, borderwidth=0)
        self.log.pack(fill="x")
        self.screen = tk.Label(root, bg="black")
        self.screen.pack(side="left", padx=(0, 10), pady=10)

        for key, button in (("Up", "UP"), ("Down", "DOWN"), ("Left", "LEFT"), ("Right", "RIGHT"),
                            ("Return", "MID"), ("space", "MID")):
            root.bind(f"<KeyPress-{key}>", lambda e, b=button: self.on_press(b))
            root.bind(f"<KeyRelease-{key}>", lambda e, b=button: self.on_release(b))
        if port:
            self.serial = SerialInput(port, self.events)
            self.serial.start()
        else:
            self.add_log("Keyboard only")
        root.protocol("WM_DELETE_WINDOW", self.close)
        self.last_frame = self.last_tick = 0.0
        self.draw_stick()
        self.draw_meter()
        self.loop()

    def close(self):
        if self.serial:
            self.serial.stop.set()
            self.serial.join(timeout=1)
        self.root.destroy()

    def add_log(self, text):
        self.log.insert(0, f"{time.strftime('%H:%M:%S')}  {text}")
        self.log.delete(200, "end")

    # Input ------------------------------------------------------------

    def on_press(self, button):
        if button in self.held:
            return              # keyboard auto-repeat
        now = time.time()
        self.held[button] = [now, now + REPEAT_DELAY_S, False]
        if button != "MID":     # MID acts on release, so a hold can mean close
            self.rx.press(button)
        self.draw_stick()

    def on_release(self, button):
        h = self.held.pop(button, None)
        if h and button == "MID" and not h[2]:
            self.rx.press("MID")
        self.draw_stick()

    def handle_held(self, now):
        for button, h in self.held.items():
            if button == "MID":
                if not h[2] and now - h[0] >= LONG_PRESS_S:
                    h[2] = True
                    self.rx.press("MID", long=True)
                    self.draw_stick()
            elif now >= h[1]:
                h[1] = now + REPEAT_S
                self.rx.press(button)

    # Drawing ----------------------------------------------------------

    def draw_stick(self):
        c = self.stick
        c.delete("all")
        cx = cy = 150
        c.create_oval(cx - 140, cy - 140, cx + 140, cy + 140, fill="#262626", outline="#3a3a3a", width=2)
        on, off = "#ffd200", "#4a4a4a"
        arrows = {
            "UP": [(cx, cy - 125), (cx - 38, cy - 70), (cx + 38, cy - 70)],
            "DOWN": [(cx, cy + 125), (cx - 38, cy + 70), (cx + 38, cy + 70)],
            "LEFT": [(cx - 125, cy), (cx - 70, cy - 38), (cx - 70, cy + 38)],
            "RIGHT": [(cx + 125, cy), (cx + 70, cy - 38), (cx + 70, cy + 38)],
        }
        for name, pts in arrows.items():
            c.create_polygon(*[v for p in pts for v in p], fill=on if name in self.held else off,
                             outline="#202020", width=2)
        mid = "MID" in self.held
        r = 46 if mid else 50
        c.create_oval(cx - r, cy - r, cx + r, cy + r, fill=on if mid else "#5a5a5a", outline="#202020", width=3)
        c.create_text(cx, cy, text="MID", fill="#111111" if mid else "#d0d0d0", font=("Segoe UI", 12, "bold"))
        if mid and self.held["MID"][2]:
            c.create_text(cx, cy + 22, text="held", fill="#111111", font=("Segoe UI", 8))

    def draw_meter(self):
        c = self.meter
        c.delete("all")
        x0, x1, y = 14, 286, 46
        scale = lambda mv: x0 + (x1 - x0) * min(mv, 3300) / 3300
        c.create_text(x0, 10, anchor="w", fill="#cfcfcf", font=("Segoe UI", 10, "bold"),
                      text="Ladder" + (f"   {self.mv} mV" if self.mv is not None else "   (no board)"))
        c.create_rectangle(x0, y - 10, x1, y + 10, fill="#262626", outline="#3a3a3a")
        lo = 0
        for (name, mv), hi in zip(LADDER, BOUNDS):
            lit = name in self.held
            c.create_rectangle(scale(lo) + 1, y - 9, scale(hi) - 1, y + 9, fill="#3d3a1a" if lit else "#2e2e2e",
                               outline="")
            c.create_line(scale(mv), y - 10, scale(mv), y + 10, fill="#777777")
            c.create_text(scale(mv), y + 22 + (12 if name in ("DOWN", "RIGHT") else 0), text=name,
                          fill="#ffd200" if lit else "#8a8a8a", font=("Segoe UI", 8))
            lo = hi
        c.create_text(x1, y + 22, anchor="e", text="none", fill="#8a8a8a", font=("Segoe UI", 8))
        if self.mv is not None:
            x = scale(self.mv)
            c.create_polygon(x, y - 12, x - 7, y - 24, x + 7, y - 24, fill="#7fffd4")
        c.create_text(x0, 100, anchor="w", fill="#7a7a7a", font=("Segoe UI", 8), text="0 V")
        c.create_text(x1, 100, anchor="e", fill="#7a7a7a", font=("Segoe UI", 8), text="3.3 V")

    def picture(self, now):
        rx = self.rx
        if now < rx.splash_until:
            return self.r.picture(self.r.splash(int(100 - (rx.splash_until - now) / 1.5 * 100)), self.PIC_H)
        if rx.status:
            return self.r.picture(self.r.status(*rx.status, rx), self.PIC_H)
        if rx.menu_open or rx.scan_active:
            if rx.scan_active:
                rx.cursor = M_SCAN
            return self.r.picture(self.r.menu(rx), self.PIC_H)
        return self.live_view()

    def live_view(self):
        """Stand-in for camera video while the menu is closed."""
        rx = self.rx
        w, h = self.PIC_H * 4 // 3, self.PIC_H
        if rx.video:
            grad = np.linspace(70, 120, w // 4)[None, :] + np.linspace(0, 30, h // 4)[:, None]
            img = Image.fromarray(np.clip(grad + np.random.normal(0, 4, grad.shape), 0, 255).astype(np.uint8),
                                  "L").resize((w, h))
        else:
            img = Image.fromarray(np.random.randint(0, 200, (h // 3, w // 3), dtype=np.uint8), "L").resize(
                (w, h), Image.NEAREST)
        d = ImageDraw.Draw(img)
        try:
            font, small = ImageFont.truetype("consola.ttf", 26), ImageFont.truetype("consola.ttf", 16)
        except OSError:
            try:
                font, small = ImageFont.load_default(26), ImageFont.load_default(16)
            except TypeError:
                font = small = ImageFont.load_default()
        label = f"LIVE  {rx.channel_name} {rx.freq} MHz" + ("" if rx.video else "  NO VIDEO")
        d.rectangle((16, 16, 30 + d.textlength(label, font=font), 56), fill=0)
        d.text((24, 20), label, fill=230, font=font)
        d.rectangle((16, h - 44, w - 16, h - 16), fill=0)
        d.text((24, h - 40), "MID: menu   LEFT/RIGHT: channel   UP/DOWN: band", fill=200, font=small)
        return img

    def loop(self):
        now = time.time()
        while not self.events.empty():
            kind, value = self.events.get()
            if kind == "log":
                self.add_log(value)
            elif kind == "mv":
                self.mv = value
            elif kind == "press":
                self.on_press(value)
            elif kind == "release":
                self.on_release(value)
        self.handle_held(now)
        if now - self.last_tick >= 0.1:
            self.last_tick = now
            self.rx.tick(now)
            self.draw_meter()
        for text in self.rx.log:
            self.add_log(text)
        self.rx.log.clear()
        if now - self.last_frame >= 0.1:
            self.last_frame = now
            self.photo = ImageTk.PhotoImage(self.picture(now))
            self.screen.configure(image=self.photo)
        self.root.after(15, self.loop)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("port", nargs="?", default="none",
                    help="the joystick board's serial port (COM21, /dev/ttyACM0), or 'none' for the keyboard")
    a = ap.parse_args()
    renderer = Renderer()
    root = tk.Tk()
    App(root, renderer, None if a.port.lower() == "none" else a.port)
    root.mainloop()


if __name__ == "__main__":
    main()
