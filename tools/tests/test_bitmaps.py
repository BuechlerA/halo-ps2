"""Xbox bitmap parsing and decoding on synthetic data (no game data needed); an optional
skip-if-missing check against a real map verifies swizzle / mip addressing on every retail format."""
import math
import struct
import sys
from pathlib import Path

sys.path[:0] = [str(Path(__file__).resolve().parents[1])]
from halo2ps2 import bitmaps, tagindex  # noqa: E402

BASE = tagindex.XBOX_TAG_BASE
ROOT = Path(__file__).resolve().parents[2]


def px(rgba: bytes, i: int) -> tuple[int, int, int, int]:
    return tuple(rgba[i * 4:i * 4 + 4])


def morton(x: int, y: int, bits: int) -> int:
    """Reference interleave for square power-of-two images: x in the even bits."""
    out = 0
    for b in range(bits):
        out |= (x >> b & 1) << (2 * b) | (y >> b & 1) << (2 * b + 1)
    return out


def test_swizzle() -> None:
    for n, bits in ((4, 2), (8, 3), (32, 5)):
        order = bitmaps.swizzle_order(n, n)
        assert sorted(order) == list(range(n * n))
        assert all(order[y * n + x] == morton(x, y, bits) for y in range(n) for x in range(n))
    # non-square: x runs out of bits first (4 wide, 16 high): bits x0 y0 x1 y1 y2 y3
    order = bitmaps.swizzle_order(4, 16)
    assert sorted(order) == list(range(64))
    assert order[0 * 4 + 1] == 1 and order[1 * 4 + 0] == 2 and order[0 * 4 + 2] == 4
    assert order[1 * 4 + 1] == 3 and order[2 * 4 + 0] == 8 and order[3 * 4 + 0] == 10
    assert order[4 * 4 + 0] == 16 and order[8 * 4 + 0] == 32 and order[15 * 4 + 3] == 63
    # wide: 16 x 4
    order = bitmaps.swizzle_order(16, 4)
    assert sorted(order) == list(range(64)) and order[0 * 16 + 4] == 16 and order[3 * 16 + 3] == 15
    # round trip: write a stream where texel (x, y) lives at its swizzled slot, read it back
    w, h = 8, 4
    lin = bytes((x * 16 + y) & 255 for y in range(h) for x in range(w))
    order = bitmaps.swizzle_order(w, h)
    stream = bytearray(w * h)
    for i, o in enumerate(order):
        stream[o] = lin[i]
    assert bitmaps.unswizzle(bytes(stream), 1, w, h) == lin
    lin16 = b"".join(struct.pack("<H", y * 256 + x) for y in range(h) for x in range(w))
    stream16 = bytearray(w * h * 2)
    for i, o in enumerate(order):
        stream16[o * 2:o * 2 + 2] = lin16[i * 2:i * 2 + 2]
    assert bitmaps.unswizzle(bytes(stream16), 2, w, h) == lin16
    assert bitmaps.unswizzle(b"\x12\x34\x56\x78", 4, 1, 1) == b"\x12\x34\x56\x78"
    try:
        bitmaps.unswizzle(b"\0" * 10, 1, 8, 8)
        raise SystemExit("short data accepted")
    except bitmaps.BitmapError:
        pass


def test_uncompressed() -> None:
    f = bitmaps.decode_pixels
    # r5g6b5: red, green, blue, white
    raw = struct.pack("<4H", 0xF800, 0x07E0, 0x001F, 0xFFFF)
    out = f(bitmaps.FMT_R5G6B5, False, raw, 2, 2)
    assert [px(out, i) for i in range(4)] == [(255, 0, 0, 255), (0, 255, 0, 255), (0, 0, 255, 255),
                                              (255, 255, 255, 255)]
    # a1r5g5b5
    out = f(bitmaps.FMT_A1R5G5B5, False, struct.pack("<2H", 0x7C00, 0xFC00), 2, 1)
    assert px(out, 0) == (255, 0, 0, 0) and px(out, 1) == (255, 0, 0, 255)
    # a4r4g4b4: alpha 0x8, r 0xF, g 0x0, b 0x4
    out = f(bitmaps.FMT_A4R4G4B4, False, struct.pack("<H", 0x8F04) + bytes(2), 2, 1)
    assert px(out, 0) == (255, 0, 68, 136) and px(out, 1) == (0, 0, 0, 0)
    # a8y8: low byte luminance, high byte alpha
    out = f(bitmaps.FMT_A8Y8, False, struct.pack("<2H", 0x4020, 0xFF80), 2, 1)
    assert px(out, 0) == (0x20, 0x20, 0x20, 0x40) and px(out, 1) == (0x80, 0x80, 0x80, 0xFF)
    # 8-bit formats
    assert px(f(bitmaps.FMT_Y8, False, b"\x80\x00", 2, 1), 0) == (128, 128, 128, 255)
    assert px(f(bitmaps.FMT_AY8, False, b"\x80\x00", 2, 1), 0) == (128, 128, 128, 128)
    assert px(f(bitmaps.FMT_A8, False, b"\x40\x00", 2, 1), 0) == (255, 255, 255, 64)
    # 32 bit: bytes are B, G, R, A / B, G, R, X
    out = f(bitmaps.FMT_A8R8G8B8, False, bytes([1, 2, 3, 4, 5, 6, 7, 8]), 2, 1)
    assert px(out, 0) == (3, 2, 1, 4) and px(out, 1) == (7, 6, 5, 8)
    out = f(bitmaps.FMT_X8R8G8B8, False, bytes([1, 2, 3, 4, 5, 6, 7, 8]), 2, 1)
    assert px(out, 0) == (3, 2, 1, 255) and px(out, 1) == (7, 6, 5, 255)
    # swizzled path goes through the Morton order: texel (1, 0) is stream slot 1, (0, 1) slot 2
    raw = struct.pack("<4H", 0xF800, 0x07E0, 0x001F, 0xFFFF)
    out = f(bitmaps.FMT_R5G6B5, True, raw, 2, 2)
    assert px(out, 1) == (0, 255, 0, 255) and px(out, 2) == (0, 0, 255, 255)
    # swizzled 4x2 (x has two bits, y one): stream order x0 y0 x1
    raw = bytes([10, 11, 12, 13, 14, 15, 16, 17])
    lin = bitmaps.unswizzle(raw, 1, 4, 2)
    assert lin == bytes([10, 12, 14, 16, 11, 13, 15, 17]) or lin == bytes([10, 11, 14, 15, 12, 13, 16, 17]), lin
    assert bitmaps.swizzle_order(4, 2) == (0, 1, 4, 5, 2, 3, 6, 7)   # x0 y0 x1: (1,0)->1, (0,1)->2


def c565(r: int, g: int, b: int) -> int:
    return (r >> 3) << 11 | (g >> 2) << 5 | b >> 3


def test_dxt() -> None:
    red, blue = c565(255, 0, 0), c565(0, 0, 255)
    # DXT1, c0 > c1 (red > blue as 565 words): indices row0 = 0 1 2 3, rest 0
    block = struct.pack("<HHI", red, blue, 0b11_10_01_00)
    out = bitmaps.decode_dxt(bitmaps.FMT_DXT1, block, 4, 4)
    row = [px(out, i) for i in range(4)]
    assert row[0] == (255, 0, 0, 255) and row[1] == (0, 0, 255, 255)
    assert row[2] == (170, 0, 85, 255) and row[3] == (85, 0, 170, 255)
    assert all(px(out, i) == (255, 0, 0, 255) for i in range(4, 16))
    # DXT1, c0 <= c1: index 2 is the midpoint, index 3 transparent black
    block = struct.pack("<HHI", blue, red, 0b11_10_01_00)
    out = bitmaps.decode_dxt(bitmaps.FMT_DXT1, block, 4, 4)
    assert [px(out, i) for i in range(4)] == [(0, 0, 255, 255), (255, 0, 0, 255), (127, 0, 127, 255), (0, 0, 0, 0)]
    # DXT3: explicit 4-bit alpha, row 0 = 0, 5, 10, 15
    block = struct.pack("<QHHI", 0xFA50, red, blue, 0)
    out = bitmaps.decode_dxt(bitmaps.FMT_DXT3, block, 4, 4)
    assert [px(out, i)[3] for i in range(4)] == [0, 85, 170, 255] and px(out, 8)[3] == 0
    assert px(out, 1)[:3] == (255, 0, 0)
    # DXT5, a0 > a1: eight interpolated values; row 0 uses indices 0, 1, 2, 7
    idx = 0 | 1 << 3 | 2 << 6 | 7 << 9
    block = struct.pack("<BBHHHHHI", 255, 0, idx & 0xFFFF, 0, 0, red, blue, 0)
    out = bitmaps.decode_dxt(bitmaps.FMT_DXT5, block, 4, 4)
    assert [px(out, i)[3] for i in range(4)] == [255, 0, (6 * 255) // 7, (255 * 1) // 7]
    # DXT5, a0 <= a1: six values plus 0 and 255 (indices 6 and 7)
    idx = 6 | 7 << 3 | 2 << 6
    block = struct.pack("<BBHHHHHI", 0, 100, idx, 0, 0, red, blue, 0)
    out = bitmaps.decode_dxt(bitmaps.FMT_DXT5, block, 4, 4)
    assert [px(out, i)[3] for i in range(3)] == [0, 255, (4 * 0 + 1 * 100) // 5]
    # two blocks side by side land in the right columns; images smaller than a block are cropped
    two = struct.pack("<HHI", red, red, 0) + struct.pack("<HHI", blue, blue, 0)
    out = bitmaps.decode_dxt(bitmaps.FMT_DXT1, two, 8, 4)
    assert px(out, 3)[:3] == (255, 0, 0) and px(out, 4)[:3] == (0, 0, 255) and len(out) == 8 * 4 * 4
    out = bitmaps.decode_dxt(bitmaps.FMT_DXT1, two[:8], 2, 2)
    assert len(out) == 16 and px(out, 3) == (255, 0, 0, 255)
    try:
        bitmaps.decode_dxt(bitmaps.FMT_DXT1, two[:7], 4, 4)
        raise SystemExit("short DXT data accepted")
    except bitmaps.BitmapError:
        pass


def synthetic_region() -> tuple[bytes, int]:
    """Region holding one bitmap tag at BASE+0x100 with two images (dxt1 mipped, cube)."""
    region = bytearray(0x400)
    tag = 0x100
    blk = 0x200

    def bdata(w, h, typ, fmt, flags, mips, off, size):
        b = bytearray(48)
        b[0:4] = b"mtib"
        struct.pack_into("<6H", b, 4, w, h, 1, typ, fmt, flags)
        struct.pack_into("<H", b, 0x14, mips)
        struct.pack_into("<II", b, 0x18, off, size)
        return b
    region[blk:blk + 48] = bdata(64, 64, 0, 14, 0x83, 6, 0x5000, 2816)
    region[blk + 48:blk + 96] = bdata(32, 32, 2, 11, 0x89, 0, 0x6000, 24576)
    struct.pack_into("<II", region, tag + 0x60, 2, BASE + blk)
    return bytes(region), BASE + tag


def test_parse() -> None:
    region, addr = synthetic_region()
    a, b = bitmaps.parse_images(region, BASE, addr)
    assert (a.width, a.height, a.format, a.mip_count, a.pixel_offset, a.pixel_size) == (64, 64, 14, 6, 0x5000, 2816)
    assert a.decodable and a.format_name == "dxt1" and not a.swizzled and a.has_alpha_channel
    assert b.type == bitmaps.TYPE_CUBE and not b.decodable and b.swizzled and b.index == 1
    assert bitmaps.level_size(14, 64, 64) == 2048 and bitmaps.level_size(16, 8, 8) == 64
    assert bitmaps.level_size(11, 4, 4) == 64 and bitmaps.level_size(6, 3, 5) == 30
    assert bitmaps.level_offset(a, 1) == 2048 and bitmaps.level_dims(a, 1) == (32, 32)
    for bad in (2, 5):
        try:
            bitmaps.level_offset(a, bad)
            raise SystemExit("unverified mip level accepted")
        except bitmaps.BitmapError:
            pass
    assert bitmaps.parse_images(region, BASE, addr + 0x80) == []      # null reflexive
    broken = bytearray(region)
    broken[0x200] ^= 1
    for bad_region, bad_addr in ((bytes(broken), addr), (region[:0x180], addr), (region, BASE + 0x3FF0)):
        try:
            bitmaps.parse_images(bad_region, BASE, bad_addr)
            raise SystemExit("bad bitmap tag accepted")
        except bitmaps.BitmapError:
            pass
    # decode_level: level 1 of a dxt1 image comes straight after level 0
    red, blue = c565(255, 0, 0), c565(0, 0, 255)
    level0 = struct.pack("<HHI", red, red, 0) * 256
    level1 = struct.pack("<HHI", blue, blue, 0) * 64
    w, h, out = bitmaps.decode_level(a, level0 + level1 + bytes(768))
    assert (w, h) == (64, 64) and px(out, 0)[:3] == (255, 0, 0)
    w, h, out = bitmaps.decode_level(a, level0 + level1 + bytes(768), 1)
    assert (w, h) == (32, 32) and px(out, 0)[:3] == (0, 0, 255) and len(out) == 32 * 32 * 4
    try:
        bitmaps.decode_level(b, bytes(24576))
        raise SystemExit("cube map decoded")
    except bitmaps.BitmapError:
        pass


def test_real_map() -> None:
    path = ROOT / "assets" / "decompressed" / "a10.map"
    if not path.exists():
        print("skip: no a10.map for the real-data check")
        return
    from halo2ps2 import mapinfo
    hdr, idx = mapinfo.read_header(path), tagindex.TagIndex.read(path)
    with path.open("rb") as f:
        f.seek(hdr.tag_data_offset)
        region = f.read(hdr.tag_data_size)
    read = bitmaps.file_reader(path)
    seen = {}
    for e in idx.entries:
        if e.group != "bitm":
            continue
        for img in bitmaps.parse_images(region, idx.base, e.data_address):
            key = (img.format, img.swizzled)
            if (img.decodable and img.mip_count >= 1 and 8 <= img.width <= 64 and 8 <= img.height <= 64
                    and "particles" not in e.path and seen.get(key, 0) < 3):
                seen[key] = seen.get(key, 0) + 1
                pixels = bitmaps.read_pixels(img, read)
                w, h, l0 = bitmaps.decode_level(img, pixels, 0)
                w1, h1, l1 = bitmaps.decode_level(img, pixels, 1)
                ref = bytearray()
                for y in range(0, h - 1, 2):
                    for x in range(0, w - 1, 2):
                        for c in range(4):
                            ref.append(sum(l0[((y + dy) * w + x + dx) * 4 + c] for dy in (0, 1) for dx in (0, 1)) >> 2)
                rms = math.sqrt(sum((a - b) ** 2 for a, b in zip(ref, l1)) / len(ref))
                # level 1 is the box-filtered level 0 up to compression / bump-map noise: a wrong swizzle
                # or offset gives an RMS of 40+
                assert rms < 25.0, (e.path, img.format_name, rms)
    assert len(seen) >= 5, seen


def main() -> None:
    test_swizzle()
    test_uncompressed()
    test_dxt()
    test_parse()
    test_real_map()
    print("ok")


if __name__ == "__main__":
    main()
