"""Hand-written parsers for the Xbox (engine 5) cache tag index and the few tag
structs the Phase 0 inventory needs (ADR-011). Layouts: docs/05-halo-ce-data.md.

Operates on a *decompressed* map (see mapinfo.py). All pointers in tag data are
absolute addresses relative to XBOX_TAG_BASE; file offset = index_offset + (addr - base).
"""
from __future__ import annotations

import struct
from dataclasses import dataclass
from pathlib import Path

from .mapinfo import HEADER_SIZE, MAGIC_HEAD, XBOX_TAG_BASE

INDEX_HEADER_SIZE = 36
TAG_ENTRY_SIZE = 32
BITMAP_ELEMENT_SIZE = 48

BITMAP_FORMATS = {
    0: "a8", 1: "y8", 2: "ay8", 3: "a8y8", 6: "r5g6b5", 8: "a1r5g5b5", 9: "a4r4g4b4",
    10: "x8r8g8b8", 11: "a8r8g8b8", 14: "dxt1", 15: "dxt3", 16: "dxt5", 17: "p8",
}
BITMAP_TYPES = {0: "2d", 1: "3d", 2: "cube"}
SOUND_RATES = {0: "22050 Hz", 1: "44100 Hz"}
SOUND_ENCODINGS = {0: "mono", 1: "stereo"}
SOUND_COMPRESSIONS = {0: "16-bit PCM", 1: "Xbox ADPCM", 2: "IMA ADPCM", 3: "Ogg Vorbis"}


@dataclass
class Tag:
    group: str        # primary group, e.g. "bitm"
    group2: str
    tag_id: int
    path: str
    data_addr: int
    indexed: int


class CacheMap:
    """Read-only view over a decompressed Xbox cache file."""

    def __init__(self, buf: bytes):
        head, engine, size, _pad, index_off, _tsize = struct.unpack_from("<6I", buf, 0)
        if head != MAGIC_HEAD or engine != 5:
            raise ValueError("not a decompressed Xbox cache file")
        if len(buf) != size:
            raise ValueError(f"size mismatch: file {len(buf)}, header {size}")
        self.buf = buf
        self.index_off = index_off
        self.name = buf[0x20:0x40].split(b"\0", 1)[0].decode("ascii", "replace")
        (self.tag_array_addr, self.scenario_id, _map_id, self.tag_count,
         self.vertex_count, _voff, self.index_count, _ioff, _magic) = \
            struct.unpack_from("<9I", buf, index_off)
        self.tags = [self._tag(i) for i in range(self.tag_count)]

    def off(self, addr: int) -> int:
        o = self.index_off + (addr - XBOX_TAG_BASE)
        if not 0 <= o < len(self.buf):
            raise ValueError(f"address {addr:#x} outside file")
        return o

    def cstr(self, addr: int) -> str:
        o = self.off(addr)
        return self.buf[o:self.buf.index(b"\0", o)].decode("ascii", "replace")

    def _tag(self, i: int) -> Tag:
        g1, g2, _g3, tid, path, data, indexed, _ = struct.unpack_from(
            "<4s4s4sIIIII", self.buf, self.index_off + INDEX_HEADER_SIZE + TAG_ENTRY_SIZE * i)
        return Tag(g1[::-1].decode("latin1"), g2[::-1].decode("latin1"), tid,
                   self.cstr(path), data, indexed)

    def reflexive(self, tag_off: int, field: int) -> tuple[int, int]:
        """(count, file offset of first element) for a reflexive field."""
        count, addr = struct.unpack_from("<II", self.buf, tag_off + field)
        return count, (self.off(addr) if count else 0)

    def bitmaps(self):
        """Yield (tag, width, height, depth, type, format, mips, pixel_bytes) per bitmap element."""
        for t in self.tags:
            if t.group != "bitm" or t.indexed:
                continue
            count, first = self.reflexive(self.off(t.data_addr), 0x60)
            for k in range(count):
                (sig, w, h, d, ty, fmt, _fl, _rx, _ry, mips, _p, _poff, psize) = \
                    struct.unpack_from("<4sHHHHHHhhHHII", self.buf, first + BITMAP_ELEMENT_SIZE * k)
                if sig != b"mtib":
                    raise ValueError(f"{t.path}: bad bitmap signature {sig!r}")
                yield t, w, h, d, ty, fmt, mips, psize

    def sounds(self):
        """Yield (tag, sample_rate, encoding, compression, perm_count, sample_bytes) (raw enums)."""
        for t in self.tags:
            if t.group != "snd!" or t.indexed:
                continue
            d = self.off(t.data_addr)
            rate = struct.unpack_from("<H", self.buf, d + 6)[0]
            enc, comp = struct.unpack_from("<HH", self.buf, d + 0x6C)   # encoding precedes compression
            nranges, rfirst = self.reflexive(d, 0x98)
            perms = size = 0
            for r in range(nranges):
                pc, pfirst = self.reflexive(rfirst + 72 * r, 0x3C)
                for p in range(pc):
                    perms += 1
                    size += struct.unpack_from("<I", self.buf, pfirst + 124 * p + 0x40)[0]
            yield t, rate, enc, comp, perms, size

    def script_function_usage(self) -> tuple[int, dict[int, int]]:
        """(script node count, {built-in function index: function_name node count}).

        Names are not stored in the map; see hsnames.load_names for the index->name table.
        """
        hdr = self.buf.find(b"script node", self.index_off)
        if hdr < 0:
            return 0, {}
        while self.buf[hdr - 1:hdr].isalpha() or self.buf[hdr - 1:hdr] == b" ":
            hdr -= 1   # name may carry a prefix ("static script node"); header fields sit 32 bytes after its start
        count = struct.unpack_from("<H", self.buf, hdr + 0x2E)[0]
        first = hdr + 0x38
        usage: dict[int, int] = {}
        for k in range(count):
            salt, ix, ntype, _fl = struct.unpack_from("<HHHH", self.buf, first + 20 * k)
            if salt and ntype == 2:   # 2 = function_name
                usage[ix] = usage.get(ix, 0) + 1
        return count, usage


def load(path: Path) -> CacheMap:
    return CacheMap(path.read_bytes())
