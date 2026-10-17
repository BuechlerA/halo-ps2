#!/usr/bin/env python3
"""Per-map bitmap / sound / script statistics read from the Xbox tag region.

Layouts (engine version 5, verified on all retail campaign maps; offsets are
inside the tag's own data block, every pointer is an absolute tag-space address):

bitmap (``bitm``)
    +0x60 reflexive {u32 count, u32 address, u32 unused} -> bitmap_data[48]
    bitmap_data: +0x04 u16 width, +0x06 u16 height, +0x08 u16 depth, +0x0A u16 type,
                 +0x0C u16 format, +0x0E u16 flags, +0x14 u16 mipmap count,
                 +0x18 u32 pixel offset (map file), +0x1C u32 pixel bytes

sound (``snd!``)
    +0x06 u16 sample rate (0 = 22050, 1 = 44100), +0x6C u16 channels (0 mono, 1 stereo),
    +0x6E u16 format (0 PCM16, 1 Xbox ADPCM, 2 IMA ADPCM, 3 Ogg)
    +0x98 reflexive -> pitch_range[72]; pitch_range +0x3C reflexive -> permutation[124];
    permutation: +0x28 u16 compression, +0x40 u32 sample bytes, +0x48 u32 sample offset (map file)

scenario (``scnr``)
    +0x474 u32 syntax data size, +0x480 u32 syntax data address
    +0x488 u32 string data size, +0x494 u32 string data address
    syntax data = 0x38-byte datum-array header ("script node" or "static script node"), then 20-byte nodes:
    u16 salt, u16 index/function, u16 type, u16 flags, u32 next, u32 string offset, u32 data.
    A node of type 2 (function_name) names a built-in; its ``index`` is the built-in id and
    its string is the script token.

Usage:
    python3 -I tools/halo2ps2/inventory.py assets/decompressed/a10.map [--scripts]
"""
from __future__ import annotations

import argparse
import struct
import sys
from collections import Counter
from dataclasses import dataclass, field
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from halo2ps2 import mapinfo, tagindex  # noqa: E402

BITMAP_TYPES = {0: "2d", 1: "3d", 2: "cube", 3: "white"}
BITMAP_FORMATS = {0: "a8", 1: "y8", 2: "ay8", 3: "a8y8", 6: "r5g6b5", 8: "a1r5g5b5",
                  9: "a4r4g4b4", 10: "x8r8g8b8", 11: "a8r8g8b8", 14: "dxt1", 15: "dxt3",
                  16: "dxt5", 17: "p8-bump", 18: "p8"}
BITMAP_FLAGS = ((0, "pow2"), (1, "compressed"), (2, "palettized"), (3, "swizzled"),
                (4, "linear"), (5, "v16u16"))
SOUND_RATES = {0: 22050, 1: 44100}
SOUND_FORMATS = {0: "pcm16", 1: "xbox-adpcm", 2: "ima-adpcm", 3: "ogg"}

BITMAP_DATA_SIZE = 48
PITCH_RANGE_SIZE = 72
PERMUTATION_SIZE = 124
SCRIPT_NODE_SIZE = 20
SCRIPT_HEADER_SIZE = 0x38
SCRIPT_HEADER_NAMES = (b"script node", b"static script node")
FUNCTION_NAME_TYPE = 2


@dataclass
class Region:
    """Tag region bytes plus the address mapping; every read is bounds-checked."""
    data: bytes
    base: int = mapinfo.XBOX_TAG_BASE
    problems: list[str] = field(default_factory=list)

    def at(self, addr: int, size: int, what: str) -> bytes | None:
        off = addr - self.base
        if off < 0 or size < 0 or off + size > len(self.data):
            self.problems.append(f"{what}: {size} bytes at {addr:#x} outside tag region")
            return None
        return self.data[off:off + size]

    def u32(self, addr: int, what: str) -> int | None:
        raw = self.at(addr, 4, what)
        return None if raw is None else struct.unpack("<I", raw)[0]

    def block(self, addr: int, count: int, stride: int, what: str) -> bytes | None:
        """Elements of a reflexive; empty blocks have a null address."""
        if count == 0:
            return b""
        return self.at(addr, count * stride, what)


def _reflexive(reg: Region, tag_addr: int, off: int, what: str) -> tuple[int, int] | None:
    raw = reg.at(tag_addr + off, 8, what)
    return None if raw is None else struct.unpack("<II", raw)


def bitmap_stats(reg: Region, idx: tagindex.TagIndex) -> dict:
    fmt, typ, flags, res = Counter(), Counter(), Counter(), Counter()
    tags = images = pixel_bytes = 0
    for e in idx.entries:
        if e.group != "bitm":
            continue
        tags += 1
        ref = _reflexive(reg, e.data_address, 0x60, f"{e.path}: bitmaps")
        if ref is None:
            continue
        blk = reg.block(ref[1], ref[0], BITMAP_DATA_SIZE, f"{e.path}: bitmap_data")
        for i in range(0, len(blk or b""), BITMAP_DATA_SIZE):
            w, h, d, t, f, fl = struct.unpack_from("<6H", blk, i + 4)
            (size,) = struct.unpack_from("<I", blk, i + 0x1C)
            images += 1
            pixel_bytes += size
            fmt[BITMAP_FORMATS.get(f, f"?{f}")] += 1
            typ[BITMAP_TYPES.get(t, f"?{t}")] += 1
            for bit, name in BITMAP_FLAGS:
                if fl & (1 << bit):
                    flags[name] += 1
            res[f"{w}x{h}"] += 1
    return {"tags": tags, "images": images, "pixel_bytes": pixel_bytes, "formats": dict(fmt),
            "types": dict(typ), "flags": dict(flags), "resolutions": dict(res)}


def sound_stats(reg: Region, idx: tagindex.TagIndex) -> dict:
    kinds = Counter()
    tags = perms = sample_bytes = 0
    for e in idx.entries:
        if e.group != "snd!":
            continue
        tags += 1
        head = reg.at(e.data_address, 0x70, f"{e.path}: header")
        ref = _reflexive(reg, e.data_address, 0x98, f"{e.path}: pitch_ranges")
        if head is None or ref is None:
            continue
        rate = SOUND_RATES.get(struct.unpack_from("<H", head, 6)[0], 0)
        chan = "stereo" if struct.unpack_from("<H", head, 0x6C)[0] else "mono"
        fmt = SOUND_FORMATS.get(struct.unpack_from("<H", head, 0x6E)[0], "?")
        prs = reg.block(ref[1], ref[0], PITCH_RANGE_SIZE, f"{e.path}: pitch_range")
        for i in range(0, len(prs or b""), PITCH_RANGE_SIZE):
            pc, pa = struct.unpack_from("<II", prs, i + 0x3C)
            pm = reg.block(pa, pc, PERMUTATION_SIZE, f"{e.path}: permutation")
            for j in range(0, len(pm or b""), PERMUTATION_SIZE):
                (size,) = struct.unpack_from("<I", pm, j + 0x40)
                perms += 1
                sample_bytes += size
                kinds[f"{fmt} {rate} Hz {chan}"] += 1
    return {"tags": tags, "permutations": perms, "sample_bytes": sample_bytes, "kinds": dict(kinds)}


def script_stats(reg: Region, idx: tagindex.TagIndex) -> dict:
    sc = idx.scenario.data_address
    sizes = [reg.u32(sc + o, "scenario") for o in (0x474, 0x480, 0x488, 0x494)]
    if None in sizes:
        return {"nodes": 0, "builtins": {}}
    syn_size, syn_addr, str_size, str_addr = sizes
    syn = reg.at(syn_addr, syn_size, "script syntax data") if syn_size else b""
    strs = reg.at(str_addr, str_size, "script string data") if str_size else b""
    if not syn or not syn.startswith(SCRIPT_HEADER_NAMES) or strs is None:
        if syn_size:
            reg.problems.append("script syntax data lacks a script node table header")
        return {"nodes": 0, "builtins": {}}
    nodes = (len(syn) - SCRIPT_HEADER_SIZE) // SCRIPT_NODE_SIZE
    calls, names = Counter(), {}
    live = 0
    for i in range(nodes):
        salt, fn, typ, _flags, _next, soff, _data = struct.unpack_from(
            "<HHHHIII", syn, SCRIPT_HEADER_SIZE + i * SCRIPT_NODE_SIZE)
        if salt == 0:
            continue
        live += 1
        if typ == FUNCTION_NAME_TYPE:
            end = strs.find(b"\0", soff)
            calls[fn] += 1
            names.setdefault(fn, strs[soff:end].decode("latin1") if end >= 0 else f"#{fn}")
    return {"nodes": live, "syntax_bytes": syn_size, "string_bytes": str_size,
            "builtins": {names[k]: v for k, v in sorted(calls.items(), key=lambda kv: (-kv[1], kv[0]))}}


def inventory(path: Path) -> dict:
    """Bitmap, sound and script statistics for one decompressed map."""
    hdr = mapinfo.read_header(path)
    idx = tagindex.TagIndex.read(path)
    with path.open("rb") as f:
        f.seek(hdr.tag_data_offset)
        data = f.read(hdr.tag_data_size)
    reg = Region(data)
    out = {"bitmaps": bitmap_stats(reg, idx), "sounds": sound_stats(reg, idx),
           "scripts": script_stats(reg, idx)}
    out["problems"] = reg.problems
    return out


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("maps", nargs="+", type=Path)
    ap.add_argument("--scripts", action="store_true", help="list built-in usage per map")
    args = ap.parse_args(argv)
    rc = 0
    for p in args.maps:
        inv = inventory(p)
        b, s, sc = inv["bitmaps"], inv["sounds"], inv["scripts"]
        print(f"{p.stem}: {b['images']} images in {b['tags']} bitmaps ({b['pixel_bytes'] / 2**20:.1f} MiB), "
              f"{s['permutations']} sound permutations ({s['sample_bytes'] / 2**20:.1f} MiB), "
              f"{sc['nodes']} script nodes, {len(sc['builtins'])} built-ins")
        if args.scripts:
            for name, n in sc["builtins"].items():
                print(f"  {n:6d} {name}")
        for pr in inv["problems"]:
            print("problem:", pr, file=sys.stderr)
            rc = 1
    return rc


if __name__ == "__main__":
    sys.exit(main())
