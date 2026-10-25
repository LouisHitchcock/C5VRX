"""Compare the compiled MSP channel map with the actual V5 RF table.
Usage: python test/test_elrs_backpack_table.py /path/to/test_elrs_backpack
"""
from pathlib import Path
import re
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
rf = (root / "main/rf.c").read_text()
bands = {}
for band, body in re.findall(r"\[FPV_BAND_(\w)\]\s*=\s*\{(.*?)\},?\s*(?=\[FPV_BAND_|\n\};)", rf, re.S):
    bands[band] = re.findall(r'\{"(\w+)",\s*(\d+)\}', body)
assert set(bands) == set("RABEFL"), bands.keys()
rows = subprocess.check_output([sys.argv[1], "--map"], text=True).splitlines()
assert len(rows) == 256
for row in rows:
    elrs, c5, mhz = map(int, row.split())
    if elrs >= 40:
        assert c5 == -1
        continue
    name, frequency = bands["RABEFL"[c5 // 8]][c5 % 8]
    assert name == "ABEFR"[elrs // 8] + str(elrs % 8 + 1), row
    assert int(frequency) == mhz, (row, frequency)
print("PASS: all 40 ELRS channel names/frequencies match V5; all unsupported indexes refused")
