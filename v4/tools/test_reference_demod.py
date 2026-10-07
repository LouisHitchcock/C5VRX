"""C5VRX by Twotoz and contributors: isolated donor dataflow regressions.

All raw endpoint pairs, duplicated six-bit DAC and two-bundle cadence.
Host model only: not FIFO throughput, analog quality or physical continuity.
"""
from pathlib import Path
import hashlib
import bs_model

ROOT = Path(__file__).resolve().parents[1]
HASHES = {
    "phase8_hr": "bd2819fe59ecb027cef530a496d27811c73378bdb951388720fcf81de07e8a9f",
    "golden": "18605f513984862d94fcdb91c073e76172eedE17e992d42ec1405330724da102".lower(),
}

def main():
    raw = bytearray()
    for previous in range(256):
        for current in range(256):
            raw.extend((0, previous, 0, current))
    raw.extend(bytes(8))
    for name in ("phase8_hr", "golden"):
        path = ROOT / f"firmware/programs/c5vrx4_reference_{name}.bsasm"
        text = path.read_text()
        # Normalize checkout line endings, while pinning every donor instruction/LUT.
        assert hashlib.sha256(text.encode()).hexdigest() == HASHES[name]
        cfg, lut, blocks, _ = bs_model.parse(text)
        assert cfg["trailing_bytes"] == "0" and cfg["eof_on"] == "downstream"
        assert len(blocks) == (8 if name == "phase8_hr" else 2)
        stats = {}
        result = bs_model.simulate(text, raw, len(raw), stats=stats, wrap_rom=name == "phase8_hr")
        assert stats["bundles"] == len(result)
        assert all(a == b and a < 64 for a,b in zip(result[::2], result[1::2]))
        for pair in range(65536):
            previous, current = pair >> 8, pair & 255
            if name == "phase8_hr":
                p, c = lut[previous] >> 8, lut[current] >> 8
                expected = ((128 + c - p) & 255) >> 2
            else:
                p, c = (lut[previous] >> 8) & 31, (lut[current] >> 8) & 31
                expected = lut[(p << 5) | c] & 63
            # Initial DAC pairs are untrusted; pipeline delay is one pair.
            assert result[4*pair+4] == expected, (name, pair, result[4*pair+4], expected)
        print(f"PASS reference {name}: 65536 raw pairs, two bundles, [D,D], six-bit DAC")
    pipeline = (ROOT / "firmware/pipeline.c").read_text()
    for name in ("level", "dc_recenter", "idle_raster", "sync_flywheel", "history"):
        body = pipeline.split(f"bool c5vrx4_{name}_enabled(void)", 1)[1].split("}", 1)[0]
        assert "if (c5vrx4_reference_demod()) return false;" in body
    mask = pipeline.split("bool c5vrx4_agc_mask_active(void)", 1)[1].split("}", 1)[0]
    assert "if (c5vrx4_reference_demod()) return false;" in mask
    assert '"ref_demod"' in pipeline and "value <= C5VRX4_DEMOD_GOLDEN" in pipeline
    measure = (ROOT / "main/video_measure.c").read_text()
    assert "memset(stats, 0, sizeof(*stats)); /* No false stride-3" in measure

if __name__ == "__main__":
    main()
