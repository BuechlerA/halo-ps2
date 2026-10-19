"""MOUT section (lip-sync data): the blob round trips, records are sorted by (tag, variant), oversized data is refused; on the real a10 map (skipped
without assets/decompressed/a10.map) the line "that's it then" (snd! tag id 3897886433) has one loudness byte per tick: 293 bytes for 10981 ms
x 64/72 x 30 Hz, steps of 5.

Set MOUT_FIXTURE=<path> to also write the blob for the C reader test (engine/tests/test_mouth.c)."""
import os
import struct
import sys
from pathlib import Path

sys.path[:0] = [str(Path(__file__).resolve().parents[1])]
from halo2ps2 import mouthconv as mc, soundconv as sc  # noqa: E402


def test_blob():
    entries = {(0xE8F00002, 1): bytes([0, 5, 255]), (0xE8F00001, 0): bytes(range(0, 250, 5)), (0xE8F00002, 0): b"\x07"}
    blob = mc.build_blob(entries)
    assert blob[:4] == b"MOUT" and struct.unpack_from("<HHII", blob, 4) == (1, 0, 3, 16 + 36)
    assert mc.parse(blob) == entries
    keys = [struct.unpack_from("<IH", blob, 16 + 12 * i) for i in range(3)]
    assert keys == sorted(keys) == [(0xE8F00001, 0), (0xE8F00002, 0), (0xE8F00002, 1)], keys
    assert len(blob) % 4 == 0
    try:
        mc.build_blob({(1, 0): bytes(0x10000)})
    except sc.SoundError:
        pass
    else:
        raise AssertionError("65536 bytes of mouth data accepted")
    for bad in (blob[:15], b"XXXX" + blob[4:], blob[:4] + struct.pack("<H", 2) + blob[6:]):
        try:
            mc.parse(bad)
        except (ValueError, struct.error):
            pass
        else:
            raise AssertionError("bad blob accepted")
    if os.environ.get("MOUT_FIXTURE"):
        Path(os.environ["MOUT_FIXTURE"]).write_bytes(blob)


def test_real_a10():
    mp = Path(__file__).resolve().parents[2] / "assets" / "decompressed" / "a10.map"
    if not mp.exists():
        print("real a10: skipped (no map)")
        return
    d = mc.parse(mc.build_section(mp))
    assert len(d) > 1000
    line = d[(3897886433, 0)]
    assert len(line) == 293 and max(line) == 255 and all(b % 5 == 0 for b in line), (len(line), max(line))
    assert abs(len(line) / 30.0 - 10981 / 1000 * 64 / 72) < 0.1                    # 30 bytes per second of the decoded line
    print(f"real a10: {len(d)} lines with mouth data")


if __name__ == "__main__":
    test_blob()
    test_real_a10()
    print("ok")
