#!/usr/bin/env python3
"""Reader and writer for the ``.hp2`` pack container (docs/07-hp2-format.md) plus a
dependency-free LZ4 block codec.

Usage:
    python3 -I tools/halo2ps2/hp2.py info pack.hp2
    python3 -I tools/halo2ps2/hp2.py pack out.hp2 --name a10 RES=res.bin BSP0=bsp0.bin [--store]
"""
from __future__ import annotations

import argparse
import struct
import sys
import zlib
from dataclasses import dataclass
from pathlib import Path

MAGIC = b"HP2P"
VERSION = 1
HEADER_SIZE = 64
ENTRY_SIZE = 32
SECTOR = 2048
MAX_SECTIONS = 64
CODEC_STORED, CODEC_LZ4 = 0, 1
HEADER_FMT = "<4sHHII16s32x"
ENTRY_FMT = "<4sIIIIIII"
assert struct.calcsize(HEADER_FMT) == HEADER_SIZE and struct.calcsize(ENTRY_FMT) == ENTRY_SIZE


class Hp2Error(ValueError):
    pass


# ---- LZ4 block codec ------------------------------------------------------------
MIN_MATCH = 4
LAST_LITERALS = 5      # the format requires the final 5 bytes to be literals
MF_LIMIT = 12          # no match may start within the last 12 bytes


def lz4_compress(src: bytes) -> bytes:
    """Greedy single-probe hash matcher. Valid LZ4 block output; ratio is modest."""
    n = len(src)
    out = bytearray()
    table: dict[bytes, int] = {}
    anchor = i = 0

    def emit(lit_end: int, match_len: int, offset: int) -> None:
        lit = lit_end - anchor
        token = min(lit, 15) << 4
        ml = match_len - MIN_MATCH if match_len else 0
        if match_len:
            token |= min(ml, 15)
        out.append(token)
        if lit >= 15:
            r = lit - 15
            while r >= 255:
                out.append(255)
                r -= 255
            out.append(r)
        out.extend(src[anchor:lit_end])
        if match_len:
            out.extend(struct.pack("<H", offset))
            if ml >= 15:
                r = ml - 15
                while r >= 255:
                    out.append(255)
                    r -= 255
                out.append(r)

    limit = n - MF_LIMIT
    while i < limit:
        key = src[i:i + 4]
        cand = table.get(key, -1)
        table[key] = i
        if cand >= 0 and i - cand <= 0xFFFF:
            length = 4
            max_len = n - LAST_LITERALS - i
            while length < max_len and src[cand + length] == src[i + length]:
                length += 1
            emit(i, length, i - cand)
            i += length
            anchor = i
        else:
            i += 1
    emit(n, 0, 0)                     # trailing literals, no match part
    return bytes(out)


def lz4_decompress(src: bytes, raw_size: int) -> bytes:
    try:
        return _lz4_decompress(src, raw_size)
    except IndexError:
        raise Hp2Error("lz4: truncated stream") from None


def _lz4_decompress(src: bytes, raw_size: int) -> bytes:
    out = bytearray()
    i, n = 0, len(src)
    while i < n:
        token = src[i]
        i += 1
        lit = token >> 4
        if lit == 15:
            while True:
                b = src[i]
                i += 1
                lit += b
                if b != 255:
                    break
        out += src[i:i + lit]
        i += lit
        if i >= n:
            break
        offset = src[i] | (src[i + 1] << 8)
        i += 2
        ml = token & 15
        if ml == 15:
            while True:
                b = src[i]
                i += 1
                ml += b
                if b != 255:
                    break
        ml += MIN_MATCH
        if offset == 0 or offset > len(out):
            raise Hp2Error("lz4: bad match offset")
        for _ in range(ml):           # byte-wise: matches may overlap their own output
            out.append(out[-offset])
        if len(out) > raw_size:
            raise Hp2Error("lz4: output exceeds raw size")
    if len(out) != raw_size:
        raise Hp2Error(f"lz4: decoded {len(out)} bytes, expected {raw_size}")
    return bytes(out)


# ---- container -------------------------------------------------------------------
@dataclass
class Section:
    tag: str
    offset: int
    stored_size: int
    raw_size: int
    codec: int
    crc32: int
    align: int


@dataclass
class Pack:
    name: str
    sections: list[Section]
    data: bytes

    def find(self, tag: str) -> Section:
        for s in self.sections:
            if s.tag == tag:
                return s
        raise KeyError(tag)

    def read(self, tag: str) -> bytes:
        s = self.find(tag)
        stored = self.data[s.offset:s.offset + s.stored_size]
        if len(stored) != s.stored_size:
            raise Hp2Error(f"{tag}: payload truncated")
        raw = stored if s.codec == CODEC_STORED else lz4_decompress(stored, s.raw_size) \
            if s.codec == CODEC_LZ4 else None
        if raw is None:
            raise Hp2Error(f"{tag}: unknown codec {s.codec}")
        if len(raw) != s.raw_size or zlib.crc32(raw) != s.crc32:
            raise Hp2Error(f"{tag}: CRC mismatch")
        return raw


def _tag_bytes(tag: str) -> bytes:
    raw = tag.encode("ascii")
    if not 1 <= len(raw) <= 4:
        raise Hp2Error(f"bad section tag {tag!r}")
    return raw.ljust(4, b"\0")


def build(name: str, sections: list[tuple[str, bytes]], compress: bool = True,
          align: int = 16) -> bytes:
    if len(sections) > MAX_SECTIONS:
        raise Hp2Error("too many sections")
    if len({t for t, _ in sections}) != len(sections):
        raise Hp2Error("duplicate section tag")
    table_end = HEADER_SIZE + ENTRY_SIZE * len(sections)
    cursor = (table_end + SECTOR - 1) // SECTOR * SECTOR
    entries, blobs = [], []
    for tag, raw in sections:
        stored, codec = raw, CODEC_STORED
        if compress:
            packed = lz4_compress(raw)
            if len(packed) < len(raw):
                stored, codec = packed, CODEC_LZ4
        entries.append(struct.pack(ENTRY_FMT, _tag_bytes(tag), cursor, len(stored), len(raw),
                                   codec, zlib.crc32(raw), align, 0))
        blobs.append((cursor, stored))
        cursor = (cursor + len(stored) + SECTOR - 1) // SECTOR * SECTOR
    out = bytearray(struct.pack(HEADER_FMT, MAGIC, VERSION, len(sections), cursor, 0,
                                name.encode("ascii")))
    for e in entries:
        out += e
    out += bytes(cursor - len(out))
    for off, stored in blobs:
        out[off:off + len(stored)] = stored
    return bytes(out)


def parse(data: bytes) -> Pack:
    if len(data) < HEADER_SIZE:
        raise Hp2Error("shorter than the header")
    magic, version, count, size, _flags, name = struct.unpack_from(HEADER_FMT, data)
    if magic != MAGIC or version != VERSION:
        raise Hp2Error(f"bad magic/version {magic!r}/{version}")
    if size != len(data) or count > MAX_SECTIONS or HEADER_SIZE + ENTRY_SIZE * count > len(data):
        raise Hp2Error("inconsistent header")
    sections = []
    for i in range(count):
        tag, off, stored, raw, codec, crc, align, _ = struct.unpack_from(
            ENTRY_FMT, data, HEADER_SIZE + i * ENTRY_SIZE)
        if off % SECTOR or off + stored > len(data):
            raise Hp2Error(f"section {i}: bad offset/size")
        sections.append(Section(tag.rstrip(b"\0").decode("ascii"), off, stored, raw, codec, crc, align))
    return Pack(name.rstrip(b"\0").decode("ascii"), sections, data)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    ai = sub.add_parser("info")
    ai.add_argument("pack", type=Path)
    ap_ = sub.add_parser("pack")
    ap_.add_argument("out", type=Path)
    ap_.add_argument("--name", default="pack")
    ap_.add_argument("--store", action="store_true", help="do not compress")
    ap_.add_argument("sections", nargs="+", help="TAG=file")
    args = ap.parse_args(argv)
    if args.cmd == "info":
        pk = parse(args.pack.read_bytes())
        print(f"{pk.name}: {len(pk.sections)} sections, {len(pk.data)} bytes")
        for s in pk.sections:
            pk.read(s.tag)   # verifies codec + CRC
            print(f"  {s.tag:<4} @{s.offset:#x} stored {s.stored_size:>9} raw {s.raw_size:>9} "
                  f"codec {s.codec} crc {s.crc32:#010x}")
        return 0
    secs = []
    for spec in args.sections:
        tag, _, path = spec.partition("=")
        secs.append((tag, Path(path).read_bytes()))
    args.out.write_bytes(build(args.name, secs, compress=not args.store))
    return 0


if __name__ == "__main__":
    sys.exit(main())
