#!/usr/bin/env python3
"""Lip-sync data of the dialogue sounds: the object pack's ``MOUT`` section (engine/src/world/mouth.c).

On the Xbox every permutation of a ``snd!`` tag carries *mouth data*: one byte per 30 Hz tick, the loudness of the line (0..255, steps of
5), the speaking unit's ``stand talk`` overlay (a jaw rotation) is scaled by it. The data sits in the permutation record (`snd!` permutation,
124 bytes) as a tag-data reference at +0x54 (u32 size, u32 flags, u32 file offset, u32 tag-space address, u32 id; the address points
into the tag region, VERIFIED on a10: 293 bytes for a 9.76 s line, 30.0 per second, 292.8 expected from the 10981 ms declared length x 64/72).
The subtitle data reference follows at +0x68 (empty in every dialogue line of a10).

MOUT layout (little endian):
    +0x00 char[4] "MOUT", u16 version 1, u16 0, u32 count, u32 pool offset
    +0x10 count x 12 bytes, sorted by (tag, variant): u32 snd! tag id, u16 variant, u16 length, u32 offset into the pool
    pool  the bytes
`variant` is the variant number of the STRM section (streamconv: one per chain head of every pitch range, in order), so the engine, which
picks a variant at random when a line starts (hs_host_misc.c), finds the mouth data of the take it plays.
"""
from __future__ import annotations

import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from halo2ps2 import soundconv as sc, streamconv, texconv  # noqa: E402

MAGIC = b"MOUT"
VERSION = 1
HEADER_SIZE = 16
REC_SIZE = 12
PERM_STRIDE = 124
MOUTH_AT = 0x54


def perm_records(t: "sc.Tags", e) -> dict[int, int]:
    """flat permutation index -> offset of its 124 byte record in the tag region."""
    off = e.data_address - t.base
    ext = t.extent[e.tag_id]
    nr, ro = t.reflexive(off + 0x98, sc.RANGE_STRIDE_SRC, ext)
    out: dict[int, int] = {}
    flat = 0
    for i in range(nr):
        npm, pmo = t.reflexive(ro + i * sc.RANGE_STRIDE_SRC + 0x3C, PERM_STRIDE, ext)
        for j in range(npm):
            out[flat] = pmo + j * PERM_STRIDE
            flat += 1
    return out


def mouth_of(t: "sc.Tags", rec: int) -> bytes:
    size, _flags, _foff, addr, _tid = t.u("<5I", rec + MOUTH_AT)
    if not size:
        return b""
    o = addr - t.base
    if size > 0xFFFF or o < 0 or o + size > len(t.data):
        raise sc.SoundError(f"mouth data {size} bytes at {addr:#x} outside the tag region")
    return bytes(t.data[o:o + size])


def collect(m: "texconv.MapData") -> dict[tuple[int, int], bytes]:
    """(sound tag id, STRM variant) -> mouth data, for every streamed dialogue line of the map that has some."""
    t = sc.Tags(m)
    items, _loops, _problems = streamconv.plan_items(m)
    ents = {e.tag_id: e for e in m.index.entries if e.group == "snd!"}
    out: dict[tuple[int, int], bytes] = {}
    for it in items:
        if it.kind != streamconv.KIND_DIALOGUE:
            continue
        recs = perm_records(t, ents[it.sound.tag_id])
        for vi, v in enumerate(it.variants):
            rec = recs.get(v.perms[0].index)
            data = mouth_of(t, rec) if rec is not None else b""
            if data:
                out[(it.sound.tag_id, vi)] = data
    return out


def build_blob(entries: dict[tuple[int, int], bytes]) -> bytes:
    keys = sorted(entries)
    pool = bytearray()
    recs = []
    for k in keys:
        data = entries[k]
        if len(data) > 0xFFFF:
            raise sc.SoundError(f"mouth data of {k} is {len(data)} bytes")
        recs.append(struct.pack("<IHHI", k[0], k[1], len(data), len(pool)))
        pool += data
        pool += bytes(-len(pool) % 4)
    pool_off = HEADER_SIZE + REC_SIZE * len(keys)
    return struct.pack("<4sHHII", MAGIC, VERSION, 0, len(keys), pool_off) + b"".join(recs) + bytes(pool)


def build_section(map_path: Path) -> bytes:
    return build_blob(collect(texconv.MapData.open(Path(map_path))))


def parse(blob: bytes) -> dict[tuple[int, int], bytes]:
    magic, version, _z, count, pool_off = struct.unpack_from("<4sHHII", blob)
    if magic != MAGIC or version != VERSION or pool_off != HEADER_SIZE + REC_SIZE * count:
        raise ValueError("bad MOUT header")
    out = {}
    for i in range(count):
        tag, variant, ln, off = struct.unpack_from("<IHHI", blob, HEADER_SIZE + REC_SIZE * i)
        out[(tag, variant)] = blob[pool_off + off:pool_off + off + ln]
    return out


def main(argv: list[str] | None = None) -> int:
    if len(argv or sys.argv[1:]) != 1:
        print(__doc__)
        return 2
    entries = collect(texconv.MapData.open(Path((argv or sys.argv[1:])[0])))
    total = sum(len(v) for v in entries.values())
    print(f"{len(entries)} lines with mouth data, {total} bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
