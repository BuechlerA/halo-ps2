#!/usr/bin/env python3
"""Colour change of objects (`CCHG` section): the tinted parts of crew, marines, Keyes, the Master Chief.

Source (verified on a10; offsets inside the tag's data block):
    object tag (bipd, vehi, weap, scen, ...): +0x28 tag reference to the `model`, +0x164 reflexive of change colours (count, address);
    change colour (0x2C bytes): +0x20 reflexive of permutations (count, address); permutation (28 bytes): f32 weight, f32 rgb lower
    bound, f32 rgb upper bound. Weights are CUMULATIVE (crewman: 0.159, 0.317, ... 1.0); an object picks the first permutation whose
    weight reaches a random number and a random point between the bounds. Change colour N is `a`, `b`, `c`, `d` for the shader's
    colour change source 1..4 (shader_model +0x4C u16); the multipurpose map (+0xBC) carries the mask in its BLUE channel
    (objtex.py makes the mask the alpha of a second copy of the base map).
Xbox look this reproduces: the crewmen's red / yellow / orange / purple jumpsuits, marine khaki armour, skin tones (change colour b).
Keyes (`captain`): his uniform's permutations are green / orange / red / grey / blue; the Xbox intro shows grey, so only the grey one is kept.

Section `CCHG`, little-endian:  char[4] 'CCHG', u16 version 1, u16 0, u32 count, then `count` records sorted by model tag id:
    u32 model tag id, 4 x { u8 permutations (0 = this change colour does not exist), u8[3] 0,
                            8 x { u8 cumulative weight (0..255), u8 lower rgb[3], u8 upper rgb[3], u8 0 } }   = 4 + 4 * 68 = 276 bytes.
"""
from __future__ import annotations

import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from halo2ps2 import texconv  # noqa: E402

OBJ_MODEL, OBJ_CHANGE = 0x28, 0x164
CHANGE_SIZE, PERM_SIZE, PERM_OFF = 0x2C, 28, 0x20
MAX_CHANGE, MAX_PERM = 4, 8
REC_SIZE = 4 + MAX_CHANGE * (4 + MAX_PERM * 8)
OBJECT_GROUPS = ("bipd", "vehi", "weap", "scen", "eqip", "garb", "proj", "mach", "ctrl", "lifi", "plac", "ssce")
GREY_ONLY = ("characters\\captain\\captain",)       # tags whose permutations are reduced to the least saturated one (see the docstring)


def _sat(lo: tuple[int, int, int], hi: tuple[int, int, int]) -> int:
    return max(max(lo) - min(lo), max(hi) - min(hi))


def read_changes(m: texconv.MapData, entry) -> tuple[int, list[list[tuple[float, tuple[int, int, int], tuple[int, int, int]]]]] | None:
    """(model tag id, [change colour: [(weight, lower rgb, upper rgb)]]) of an object tag, or None."""
    r, base = m.region, m.base
    b = m.tag_off(entry)
    if b < 0 or b + OBJ_CHANGE + 8 > len(r):
        return None
    group, _p, _n, model = struct.unpack_from("<4sIII", r, b + OBJ_MODEL)
    cnt, addr = struct.unpack_from("<II", r, b + OBJ_CHANGE)
    me = m.by_id.get(model)
    if group != b"edom" or me is None or me.group != "mode" or not 0 < cnt <= MAX_CHANGE or not 0 <= addr - base < len(r) - cnt * CHANGE_SIZE:
        return None
    out = []
    for i in range(cnt):
        o = addr - base + i * CHANGE_SIZE
        pc, pa = struct.unpack_from("<II", r, o + PERM_OFF)
        perms = []
        if 0 < pc <= MAX_PERM and 0 <= pa - base < len(r) - pc * PERM_SIZE:
            for j in range(pc):
                w, *f = struct.unpack_from("<7f", r, pa - base + j * PERM_SIZE)
                if not all(0.0 <= x <= 1.0 for x in (w, *f)):
                    return None
                q = lambda v: max(0, min(255, int(v * 255.0 + 0.5)))  # noqa: E731
                perms.append((w, (q(f[0]), q(f[1]), q(f[2])), (q(f[3]), q(f[4]), q(f[5]))))
        out.append(perms)
    return model, out


def collect(m: texconv.MapData, model_tags: set[int] | None = None) -> dict[int, list]:
    """Change colours per model tag id (the first object tag that has some wins)."""
    res: dict[int, list] = {}
    for e in m.index.entries:
        if e.group not in OBJECT_GROUPS:
            continue
        got = read_changes(m, e)
        if got is None or not any(got[1]):
            continue
        model, changes = got
        if model_tags is not None and model not in model_tags:
            continue
        if e.path in GREY_ONLY:
            if changes[0]:                                  # change colour a only: the uniform, not the skin
                g = min(changes[0], key=lambda q: _sat(q[1], q[2]))
                changes = [[(1.0, g[1], g[2])]] + changes[1:]
        res.setdefault(model, changes)
    return res


def pack_record(model: int, changes: list) -> bytes:
    out = struct.pack("<I", model)
    for c in range(MAX_CHANGE):
        perms = changes[c] if c < len(changes) else []
        out += struct.pack("<B3x", len(perms))
        for j in range(MAX_PERM):
            if j < len(perms):
                w, lo, hi = perms[j]
                out += struct.pack("<B3B3BB", max(0, min(255, int(w * 255.0 + 0.5))), *lo, *hi, 0)
            else:
                out += bytes(8)
    assert len(out) == REC_SIZE
    return out


def build_section(path: Path | texconv.MapData, model_tags: set[int] | None = None) -> bytes:
    m = path if isinstance(path, texconv.MapData) else texconv.MapData.open(path)
    recs = sorted(collect(m, model_tags).items())
    return struct.pack("<4sHHI", b"CCHG", 1, 0, len(recs)) + b"".join(pack_record(k, v) for k, v in recs)


def parse_section(data: bytes) -> dict[int, list]:
    magic, ver, _r, n = struct.unpack_from("<4sHHI", data, 0)
    if magic != b"CCHG" or ver != 1 or len(data) != 12 + n * REC_SIZE:
        raise ValueError("bad CCHG section")
    out = {}
    for i in range(n):
        o = 12 + i * REC_SIZE
        model, = struct.unpack_from("<I", data, o)
        changes = []
        for c in range(MAX_CHANGE):
            co = o + 4 + c * 68
            pc = data[co]
            changes.append([(data[co + 4 + 8 * j] / 255.0, tuple(data[co + 5 + 8 * j:co + 8 + 8 * j]), tuple(data[co + 8 + 8 * j:co + 11 + 8 * j]))
                            for j in range(pc)])
        out[model] = changes
    return out


def main(argv: list[str] | None = None) -> int:
    path = Path((argv or sys.argv[1:])[0])
    m = texconv.MapData.open(path)
    for model, changes in sorted(collect(m).items()):
        e = m.by_id[model]
        print(f"{e.path}: " + ", ".join(f"{chr(97 + i)}:{len(p)}" for i, p in enumerate(changes) if p))
    return 0


if __name__ == "__main__":
    sys.exit(main())
