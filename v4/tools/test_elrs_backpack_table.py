"""ELRS backpack channel map: every ELRS index lands on the same frequency
and channel name in the C5VRX rf.c table. Source-level check, host only."""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]


def main():
    rf = (ROOT / "main/rf.c").read_text()
    table = rf.split("s_fpv_channels[FPV_BAND_COUNT][8] = {", 1)[1].split("};", 1)[0]
    bands = {}
    for band, body in re.findall(r"\[FPV_BAND_(\w)\]\s*=\s*\{[^{]*?((?:\{\s*\"\w+\",\s*\d+\s*\},?\s*){8})", table):
        bands[band] = re.findall(r"\"(\w+)\",\s*(\d+)", body)
    order = {"R": 0, "A": 1, "B": 2, "E": 3, "F": 4, "L": 5}
    assert set(bands) == set(order), bands.keys()
    c5 = [None] * 48
    for band, chans in bands.items():
        for i, (name, mhz) in enumerate(chans):
            c5[order[band] * 8 + i] = (name, int(mhz))

    src = (ROOT / "firmware/elrs_backpack.c").read_text()
    elrs = [int(x) for x in re.findall(r"\d{4}", src.split("s_elrs_mhz[ELRS_CHANNEL_COUNT] = {", 1)[1].split("};", 1)[0])]
    c5_band = [int(x) if x.strip().isdigit() else None for x in re.search(r"s_c5_band\[6\] = \{([^}]*)\}", src).group(1).split(",")]
    assert len(elrs) == 48 and c5_band[5] is None
    assert [mhz for _, mhz in c5[40:48]] != elrs[40:48]  # L plans really differ
    assert "NO_BAND" in src and src.count("0xFFu") == 1
    for index in range(40):  # ELRS L (40..47) is refused: different plan
        name, mhz = c5[c5_band[index // 8] * 8 + index % 8]
        assert mhz == elrs[index], (index, name, mhz, elrs[index])
        assert name == "ABEFRL"[index // 8] + str(index % 8 + 1), (index, name)
    print("PASS ELRS backpack table: 40 ELRS indices (R,A,B,E,F) match rf.c names and frequencies")


if __name__ == "__main__":
    main()
