"""hp2 container and LZ4 codec round trips; optionally writes a fixture pack for the C test."""
import os
import random
import sys
from pathlib import Path

sys.path[:0] = [str(Path(__file__).resolve().parents[1])]
from halo2ps2 import hp2  # noqa: E402


def fixture_sections() -> list[tuple[str, bytes]]:
    rnd = random.Random(7)
    text = (b"halo-ps2 " * 700) + bytes(rnd.randrange(256) for _ in range(300)) + b"tail-bytes-here!"
    return [("RES", text), ("BSP0", bytes(5000)), ("TEX", bytes(rnd.randrange(256) for _ in range(777))),
            ("SND", b"")]


def main() -> None:
    rnd = random.Random(1)
    for n in (0, 1, 4, 11, 12, 13, 100, 4096):
        for src in (bytes(n), bytes(rnd.randrange(256) for _ in range(n)),
                    bytes(rnd.choice(b"abc") for _ in range(n))):
            assert hp2.lz4_decompress(hp2.lz4_compress(src), len(src)) == src, n
    long = b"x" * 70000 + b"yz" * 40000          # offsets > 255, long match/literal run lengths
    assert hp2.lz4_decompress(hp2.lz4_compress(long), len(long)) == long

    secs = fixture_sections()
    blob = hp2.build("test", secs)
    pk = hp2.parse(blob)
    assert pk.name == "test" and [s.tag for s in pk.sections] == [t for t, _ in secs]
    assert all(s.offset % hp2.SECTOR == 0 for s in pk.sections) and len(blob) % hp2.SECTOR == 0
    for tag, raw in secs:
        assert pk.read(tag) == raw, tag
    assert pk.find("RES").codec == hp2.CODEC_LZ4 and pk.find("TEX").codec == hp2.CODEC_STORED
    assert hp2.parse(hp2.build("raw", secs, compress=False)).read("BSP0") == secs[1][1]

    bad = bytearray(blob)
    bad[pk.find("RES").offset + 20] ^= 0xFF           # flip a payload byte
    try:
        hp2.parse(bytes(bad)).read("RES")
        raise SystemExit("corruption not detected")
    except hp2.Hp2Error:
        pass
    for broken in (blob[:40], b"NOPE" + blob[4:], blob[:-1]):
        try:
            hp2.parse(broken)
            raise SystemExit("bad pack accepted")
        except hp2.Hp2Error:
            pass
    out = os.environ.get("HP2_FIXTURE")
    if out:
        Path(out).write_bytes(blob)
    print("ok")


if __name__ == "__main__":
    main()
