#!/usr/bin/env python3
"""Parse and decode Xbox ``bitmap`` tags (engine version 5) to RGBA8. Pure stdlib.

Layouts (verified on all 11 retail maps; offsets are inside the tag's own data block, every
pointer is an absolute tag-space address, see inventory.py for the shared reflexive layout):

bitmap (``bitm``) tag data
    +0x60 reflexive {u32 count, u32 address, u32 unused} -> bitmap_data[48 bytes]

bitmap_data (48 bytes)
    +0x00 char[4] signature ``bitm`` (stored byte-reversed: b"mtib")
    +0x04 u16 width, +0x06 u16 height, +0x08 u16 depth, +0x0A u16 type, +0x0C u16 format,
    +0x0E u16 flags, +0x10 s16 registration x, +0x12 s16 registration y,
    +0x14 u16 mipmap count (number of mips *below* the base level; 0 = base only),
    +0x16 u16 unknown (varies between 0 and ~30000, not used),
    +0x18 u32 pixel offset (absolute offset in the decompressed map file),
    +0x1C u32 pixel bytes (whole mip chain, padded up to a multiple of 128),
    +0x20 u32 owning tag id, +0x24 u32 0xFFFFFFFF, +0x28 u32 0, +0x2C u32 runtime pointer
types   0 2d, 1 3d, 2 cube map, 3 white.  Only type 0 is decoded here; the rest are flagged.
formats 0 a8, 1 y8, 2 ay8, 3 a8y8, 6 r5g6b5, 8 a1r5g5b5, 9 a4r4g4b4, 10 x8r8g8b8,
        11 a8r8g8b8, 14 dxt1, 15 dxt3, 16 dxt5, 17 p8-bump, 18 p8
flags   bit0 pow2, bit1 compressed (DXT), bit2 palettized (p8 only), bit3 swizzled (Morton),
        bit4 linear, bit5 v16u16; bit7 is set on every retail image (meaning unknown).

Pixel data (verified by comparing the stored sizes with the model below on every 2D image of
a10 and by checking that mip 1 decodes to the box-filtered mip 0 in test/verify scripts)
    * the mip chain is stored largest first, back to back; level 0 occupies exactly
      ``level_size(format, w, h)`` bytes and level 1 follows it immediately.  Smaller levels are
      padded in a way that is NOT modelled (DXT chains and tiny swizzled levels disagree with the
      naive sum by a few bytes), so only levels 0 and 1 are addressable here;
    * DXT1/3/5 are plain 4x4 block rasters (8 or 16 byte blocks), never swizzled;
    * uncompressed formats are Morton ("swizzled", flag bit3) with x in the even bits and y in
      the odd bits; once one dimension runs out of bits the other continues linearly (so 4x256 and
      256x4 work); images without the flag (a4r4g4b4 ``linear``) are row-major;
    * a8y8 is little-endian 16 bit with alpha in the high byte; 32 bit formats are B,G,R,A bytes;
      r5g6b5 / a1r5g5b5 / a4r4g4b4 are little-endian 16 bit words, channels in the usual order;
    * dxt1 blocks with color0 <= color1 use index 3 as transparent black.
Unverified: the colour convention of a8 (decoded as white with the given alpha), p8/p8-bump
(index used as grey; retail only has p8-bump, which are bump maps and never base maps).
Cube maps carry six faces back to back; they and 3D maps are reported, not decoded.

Usage:
    python3 -I tools/halo2ps2/bitmaps.py assets/decompressed/a10.map [--limit 20]
"""
from __future__ import annotations

import argparse
import struct
import sys
from array import array
from dataclasses import dataclass
from functools import lru_cache
from operator import itemgetter
from pathlib import Path
from typing import Callable

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

BITMAP_DATA_SIZE = 48
BITMAPS_REFLEXIVE = 0x60
SIGNATURE = b"mtib"

TYPE_2D, TYPE_3D, TYPE_CUBE, TYPE_WHITE = 0, 1, 2, 3
(FMT_A8, FMT_Y8, FMT_AY8, FMT_A8Y8, FMT_R5G6B5, FMT_A1R5G5B5, FMT_A4R4G4B4, FMT_X8R8G8B8,
 FMT_A8R8G8B8, FMT_DXT1, FMT_DXT3, FMT_DXT5, FMT_P8_BUMP, FMT_P8) = (0, 1, 2, 3, 6, 8, 9, 10, 11, 14, 15, 16, 17, 18)
FORMAT_NAMES = {FMT_A8: "a8", FMT_Y8: "y8", FMT_AY8: "ay8", FMT_A8Y8: "a8y8", FMT_R5G6B5: "r5g6b5",
                FMT_A1R5G5B5: "a1r5g5b5", FMT_A4R4G4B4: "a4r4g4b4", FMT_X8R8G8B8: "x8r8g8b8",
                FMT_A8R8G8B8: "a8r8g8b8", FMT_DXT1: "dxt1", FMT_DXT3: "dxt3", FMT_DXT5: "dxt5",
                FMT_P8_BUMP: "p8-bump", FMT_P8: "p8"}
FLAG_POW2, FLAG_COMPRESSED, FLAG_PALETTIZED, FLAG_SWIZZLED, FLAG_LINEAR = 1, 2, 4, 8, 16
BITS_PER_PIXEL = {FMT_A8: 8, FMT_Y8: 8, FMT_AY8: 8, FMT_A8Y8: 16, FMT_R5G6B5: 16, FMT_A1R5G5B5: 16,
                  FMT_A4R4G4B4: 16, FMT_X8R8G8B8: 32, FMT_A8R8G8B8: 32, FMT_P8_BUMP: 8, FMT_P8: 8}
DXT_BLOCK_BYTES = {FMT_DXT1: 8, FMT_DXT3: 16, FMT_DXT5: 16}
HAS_ALPHA = {FMT_A8, FMT_AY8, FMT_A8Y8, FMT_A1R5G5B5, FMT_A4R4G4B4, FMT_A8R8G8B8, FMT_DXT1, FMT_DXT3, FMT_DXT5}

_LE = sys.byteorder == "little"


class BitmapError(ValueError):
    pass


@dataclass
class BitmapImage:
    width: int
    height: int
    depth: int
    type: int
    format: int
    flags: int
    mip_count: int
    pixel_offset: int          # absolute file offset
    pixel_size: int
    index: int = 0             # position inside the bitmap tag

    @property
    def format_name(self) -> str:
        return FORMAT_NAMES.get(self.format, f"?{self.format}")

    @property
    def swizzled(self) -> bool:
        return bool(self.flags & FLAG_SWIZZLED)

    @property
    def is_2d(self) -> bool:
        return self.type == TYPE_2D and self.depth <= 1

    @property
    def decodable(self) -> bool:
        return self.is_2d and self.format in FORMAT_NAMES and self.width > 0 and self.height > 0

    @property
    def has_alpha_channel(self) -> bool:
        return self.format in HAS_ALPHA


# ---- parsing -------------------------------------------------------------------------------
def parse_images(region: bytes, base: int, tag_addr: int) -> list[BitmapImage]:
    """The ``bitmap_data`` blocks of the bitmap tag whose data block is at ``tag_addr``."""
    off = tag_addr - base
    if off < 0 or off + 0x6C > len(region):
        raise BitmapError(f"bitmap tag at {tag_addr:#x} outside the tag region")
    count, addr = struct.unpack_from("<II", region, off + BITMAPS_REFLEXIVE)
    if count == 0:
        return []
    bo = addr - base
    if count > 4096 or bo < 0 or bo + count * BITMAP_DATA_SIZE > len(region):
        raise BitmapError(f"bitmap_data block ({count} at {addr:#x}) outside the tag region")
    out = []
    for i in range(count):
        sig, w, h, d, t, f, fl, _rx, _ry, mips, _u, poff, psize = struct.unpack_from(
            "<4sHHHHHHhhHHII", region, bo + i * BITMAP_DATA_SIZE)
        if sig != SIGNATURE:
            raise BitmapError(f"bitmap_data[{i}] has signature {sig!r}")
        out.append(BitmapImage(w, h, d, t, f, fl, mips, poff, psize, i))
    return out


# ---- sizes ---------------------------------------------------------------------------------
def level_dims(img: BitmapImage, level: int) -> tuple[int, int]:
    return max(1, img.width >> level), max(1, img.height >> level)


def level_size(fmt: int, w: int, h: int) -> int:
    """Bytes of one mip level of a 2D image (exact for level 0; see the module docstring)."""
    if fmt in DXT_BLOCK_BYTES:
        return ((w + 3) // 4) * ((h + 3) // 4) * DXT_BLOCK_BYTES[fmt]
    if fmt not in BITS_PER_PIXEL:
        raise BitmapError(f"unknown bitmap format {fmt}")
    return w * h * BITS_PER_PIXEL[fmt] // 8


def level_offset(img: BitmapImage, level: int) -> int:
    """Byte offset of ``level`` inside the image's pixel data. Only 0 and 1 are verified."""
    if level == 0:
        return 0
    if level == 1 and img.mip_count >= 1:
        return level_size(img.format, img.width, img.height)
    raise BitmapError("only mip levels 0 and 1 are addressable")


# ---- swizzle -------------------------------------------------------------------------------
def _deposit(value: int, mask: int) -> int:
    out, bit = 0, 1
    while mask:
        low = mask & -mask
        if value & bit:
            out |= low
        mask ^= low
        bit <<= 1
    return out


@lru_cache(maxsize=64)
def swizzle_order(w: int, h: int) -> tuple[int, ...]:
    """``order[y * w + x]`` = index of texel (x, y) in the swizzled stream (Xbox / NV2A Morton)."""
    mx = my = 0
    bit = mb = 1
    while bit < w or bit < h:
        if bit < w:
            mx |= mb
            mb <<= 1
        if bit < h:
            my |= mb
            mb <<= 1
        bit <<= 1
    xo = [_deposit(x, mx) for x in range(w)]
    yo = [_deposit(y, my) for y in range(h)]
    return tuple(a + b for b in yo for a in xo)


def unswizzle(raw: bytes, bytes_per_pixel: int, w: int, h: int) -> bytes:
    """Swizzled texel stream -> row-major bytes."""
    n = w * h
    if len(raw) < n * bytes_per_pixel:
        raise BitmapError("pixel data shorter than the image")
    if n == 1:
        return bytes(raw[:bytes_per_pixel])
    order = swizzle_order(w, h)
    if bytes_per_pixel == 1:
        return bytes(itemgetter(*order)(raw))
    code = {2: "H", 4: "I"}[bytes_per_pixel]
    src = array(code, raw[:n * bytes_per_pixel])
    if not _LE:
        src.byteswap()
    dst = array(code, itemgetter(*order)(src))
    if not _LE:
        dst.byteswap()
    return dst.tobytes()


# ---- uncompressed formats ------------------------------------------------------------------
def _pack(r: int, g: int, b: int, a: int) -> int:
    return r | g << 8 | b << 16 | a << 24


def _e5(v: int) -> int:
    return v << 3 | v >> 2


def _e6(v: int) -> int:
    return v << 2 | v >> 4


@lru_cache(maxsize=None)
def _lut(fmt: int) -> array:
    """Packed RGBA (little-endian bytes R,G,B,A) for every 8 or 16 bit texel value."""
    if fmt == FMT_R5G6B5:
        return array("I", (_pack(_e5(v >> 11), _e6(v >> 5 & 63), _e5(v & 31), 255) for v in range(65536)))
    if fmt == FMT_A1R5G5B5:
        return array("I", (_pack(_e5(v >> 10 & 31), _e5(v >> 5 & 31), _e5(v & 31), 255 if v >> 15 else 0)
                           for v in range(65536)))
    if fmt == FMT_A4R4G4B4:
        return array("I", (_pack((v >> 8 & 15) * 17, (v >> 4 & 15) * 17, (v & 15) * 17, (v >> 12) * 17)
                           for v in range(65536)))
    if fmt == FMT_A8Y8:
        return array("I", (_pack(v & 255, v & 255, v & 255, v >> 8) for v in range(65536)))
    if fmt == FMT_A8:
        return array("I", (_pack(255, 255, 255, v) for v in range(256)))
    if fmt == FMT_AY8:
        return array("I", (_pack(v, v, v, v) for v in range(256)))
    if fmt in (FMT_Y8, FMT_P8, FMT_P8_BUMP):
        return array("I", (_pack(v, v, v, 255) for v in range(256)))
    raise BitmapError(f"no lookup table for format {fmt}")


def _via_lut(fmt: int, linear: bytes) -> bytes:
    lut = _lut(fmt)
    if BITS_PER_PIXEL[fmt] == 8:
        out = array("I", map(lut.__getitem__, linear))
    else:
        px = array("H", linear)
        if not _LE:
            px.byteswap()
        out = array("I", map(lut.__getitem__, px))
    if not _LE:
        out.byteswap()
    return out.tobytes()


def _decode_32(fmt: int, linear: bytes) -> bytes:
    out = bytearray(linear)
    out[0::4] = linear[2::4]          # B,G,R,A -> R,G,B,A
    out[2::4] = linear[0::4]
    if fmt == FMT_X8R8G8B8:
        out[3::4] = b"\xff" * (len(linear) // 4)
    return bytes(out)


# ---- DXT -----------------------------------------------------------------------------------
_IDX4 = [itemgetter(b & 3, b >> 2 & 3, b >> 4 & 3, b >> 6) for b in range(256)]
_RGB565 = None


def _rgb565() -> array:
    global _RGB565
    if _RGB565 is None:
        _RGB565 = array("I", (_pack(_e5(v >> 11), _e6(v >> 5 & 63), _e5(v & 31), 0) for v in range(65536)))
    return _RGB565


def _rgb(c: int) -> tuple[int, int, int]:
    return _e5(c >> 11), _e6(c >> 5 & 63), _e5(c & 31)


def _dxt_colours(c0: int, c1: int, four: bool) -> tuple[int, int, int, int]:
    """Palette of a colour block as packed RGB (alpha bits 0). ``four`` forces the 4-colour mode
    (DXT3/5); DXT1 with c0 <= c1 gets a transparent black fourth entry (alpha bits stay 0)."""
    lut = _rgb565()
    p0, p1 = lut[c0], lut[c1]
    r0, g0, b0 = _rgb(c0)
    r1, g1, b1 = _rgb(c1)
    if four or c0 > c1:
        p2 = _pack((2 * r0 + r1) // 3, (2 * g0 + g1) // 3, (2 * b0 + b1) // 3, 0)
        p3 = _pack((r0 + 2 * r1) // 3, (g0 + 2 * g1) // 3, (b0 + 2 * b1) // 3, 0)
    else:
        p2 = _pack((r0 + r1) // 2, (g0 + g1) // 2, (b0 + b1) // 2, 0)
        p3 = 0
    return p0, p1, p2, p3


def decode_dxt(fmt: int, raw: bytes, w: int, h: int) -> bytes:
    """DXT1/3/5 block raster -> RGBA8 row-major."""
    bw, bh = (w + 3) // 4, (h + 3) // 4
    pw = bw * 4
    need = bw * bh * DXT_BLOCK_BYTES[fmt]
    if len(raw) < need:
        raise BitmapError("pixel data shorter than the image")
    out = [0] * (pw * bh * 4)
    ALPHA = 0xFF000000
    if fmt == FMT_DXT1:
        it = struct.iter_unpack("<HHI", memoryview(raw)[:need])
    elif fmt == FMT_DXT3:
        it = struct.iter_unpack("<QHHI", memoryview(raw)[:need])
    else:
        it = struct.iter_unpack("<BBHHHHHI", memoryview(raw)[:need])
    bi = 0
    for blk in it:
        by, bx = divmod(bi, bw)
        bi += 1
        base = by * 4 * pw + bx * 4
        if fmt == FMT_DXT1:
            c0, c1, bits = blk
            cols = _dxt_colours(c0, c1, False)
            if c0 > c1:
                cols = tuple(c | ALPHA for c in cols)
            else:
                cols = (cols[0] | ALPHA, cols[1] | ALPHA, cols[2] | ALPHA, 0)
            for r in range(4):
                out[base + r * pw:base + r * pw + 4] = _IDX4[bits >> (8 * r) & 255](cols)
        elif fmt == FMT_DXT3:
            abits, c0, c1, bits = blk
            cols = _dxt_colours(c0, c1, True)
            for r in range(4):
                row = _IDX4[bits >> (8 * r) & 255](cols)
                a = abits >> (16 * r)
                out[base + r * pw:base + r * pw + 4] = (
                    row[0] | (a & 15) * 17 << 24, row[1] | (a >> 4 & 15) * 17 << 24,
                    row[2] | (a >> 8 & 15) * 17 << 24, row[3] | (a >> 12 & 15) * 17 << 24)
        else:
            a0, a1, i0, i1, i2, c0, c1, bits = blk
            cols = _dxt_colours(c0, c1, True)
            if a0 > a1:
                al = (a0, a1, *((( 7 - k) * a0 + k * a1) // 7 for k in range(1, 7)))
            else:
                al = (a0, a1, *(((5 - k) * a0 + k * a1) // 5 for k in range(1, 5)), 0, 255)
            aidx = i0 | i1 << 16 | i2 << 32
            for r in range(4):
                row = _IDX4[bits >> (8 * r) & 255](cols)
                s = r * 12
                out[base + r * pw:base + r * pw + 4] = (
                    row[0] | al[aidx >> s & 7] << 24, row[1] | al[aidx >> (s + 3) & 7] << 24,
                    row[2] | al[aidx >> (s + 6) & 7] << 24, row[3] | al[aidx >> (s + 9) & 7] << 24)
    arr = array("I", out)
    if not _LE:
        arr.byteswap()
    data = arr.tobytes()
    if pw == w and bh * 4 == h:
        return data
    rows = [data[y * pw * 4:y * pw * 4 + w * 4] for y in range(h)]
    return b"".join(rows)


# ---- public decode -------------------------------------------------------------------------
def decode_pixels(fmt: int, swizzled: bool, raw: bytes, w: int, h: int) -> bytes:
    """Raw bytes of one level -> RGBA8 row-major (``w * h * 4`` bytes: R, G, B, A)."""
    if fmt in DXT_BLOCK_BYTES:
        return decode_dxt(fmt, raw, w, h)
    if fmt not in BITS_PER_PIXEL:
        raise BitmapError(f"unsupported bitmap format {fmt}")
    bpp = BITS_PER_PIXEL[fmt] // 8
    linear = unswizzle(raw, bpp, w, h) if swizzled else bytes(raw[:w * h * bpp])
    if len(linear) < w * h * bpp:
        raise BitmapError("pixel data shorter than the image")
    if bpp == 4:
        return _decode_32(fmt, linear)
    return _via_lut(fmt, linear)


def decode_level(img: BitmapImage, pixels: bytes, level: int = 0) -> tuple[int, int, bytes]:
    """Decode mip ``level`` (0 or 1) of ``pixels`` (the image's ``pixel_size`` bytes).
    Returns (width, height, rgba)."""
    if not img.decodable:
        raise BitmapError(f"image type {img.type}/format {img.format} is not decodable")
    w, h = level_dims(img, level)
    off = level_offset(img, level)
    size = level_size(img.format, w, h)
    raw = pixels[off:off + size]
    return w, h, decode_pixels(img.format, img.swizzled, raw, w, h)


PixelReader = Callable[[int, int], bytes]


def read_pixels(img: BitmapImage, reader: PixelReader) -> bytes:
    return reader(img.pixel_offset, img.pixel_size)


def file_reader(path: Path) -> PixelReader:
    """Reader of absolute file ranges of a decompressed map (opens the file per call batch)."""
    fh = path.open("rb")

    def read(off: int, size: int) -> bytes:
        fh.seek(off)
        data = fh.read(size)
        if len(data) != size:
            raise BitmapError(f"pixel range {off:#x}+{size:#x} outside the map file")
        return data
    return read


def main(argv: list[str] | None = None) -> int:
    from halo2ps2 import mapinfo, tagindex
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("map", type=Path)
    ap.add_argument("--limit", type=int, default=20)
    args = ap.parse_args(argv)
    hdr = mapinfo.read_header(args.map)
    idx = tagindex.TagIndex.read(args.map)
    with args.map.open("rb") as f:
        f.seek(hdr.tag_data_offset)
        region = f.read(hdr.tag_data_size)
    reader = file_reader(args.map)
    shown = 0
    for e in idx.entries:
        if e.group != "bitm":
            continue
        for img in parse_images(region, idx.base, e.data_address):
            if shown < args.limit:
                print(f"{e.path} [{img.index}] {img.width}x{img.height} {img.format_name} "
                      f"mips={img.mip_count} flags={img.flags:#x} type={img.type} size={img.pixel_size}")
                shown += 1
            if img.decodable:
                decode_level(img, read_pixels(img, reader))
    return 0


if __name__ == "__main__":
    sys.exit(main())
