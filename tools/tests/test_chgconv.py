"""CCHG section (tools/halo2ps2/chgconv.py): record layout, round trip, the captain's grey-only rule."""
import struct
import sys
from pathlib import Path

sys.path[:0] = [str(Path(__file__).resolve().parents[1])]
from halo2ps2 import chgconv  # noqa: E402


def test_round_trip() -> None:
    a = [(0.5, (255, 0, 0), (200, 0, 0)), (1.0, (0, 0, 255), (0, 0, 200))]
    b = [(1.0, (10, 20, 30), (40, 50, 60))]
    rec = chgconv.pack_record(0xE1234567, [a, b])
    assert len(rec) == chgconv.REC_SIZE == 276
    blob = struct.pack("<4sHHI", b"CCHG", 1, 0, 1) + rec
    got = chgconv.parse_section(blob)[0xE1234567]
    assert [len(c) for c in got] == [2, 1, 0, 0]
    assert got[0][0][1:] == ((255, 0, 0), (200, 0, 0)) and abs(got[0][1][0] - 1.0) < 1e-6 and abs(got[0][0][0] - 0.5) < 0.01
    assert got[1][0][1:] == ((10, 20, 30), (40, 50, 60))
    try:
        chgconv.parse_section(blob[:-1])
    except ValueError:
        pass
    else:
        raise AssertionError("truncated section accepted")


def test_saturation() -> None:
    assert chgconv._sat((100, 100, 100), (90, 90, 90)) == 0 and chgconv._sat((255, 0, 0), (0, 0, 0)) == 255


if __name__ == "__main__":
    test_round_trip()
    test_saturation()
    print("ok")
