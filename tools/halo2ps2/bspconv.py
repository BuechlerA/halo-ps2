#!/usr/bin/env python3
"""Convert Xbox structure BSP geometry into the PS2 `BSPn` section (docs/08-bsp-section.md).

Usage:
    python3 -I tools/halo2ps2/bspconv.py assets/decompressed/a10.map -o assets/hp2/a10.hp2 [--bsp 0]
    python3 -I tools/halo2ps2/bspconv.py --info assets/hp2/a10.hp2
"""
from __future__ import annotations

import argparse
import math
import struct
import sys
from dataclasses import dataclass, field
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from halo2ps2 import bsp, hp2, mapinfo, tagindex  # noqa: E402

MAGIC = b"BSPS"
VERSION = 1
HEADER_SIZE = 64
MATERIAL_REC = 32
BATCH_REC = 32
MAX_TRIS = 32                  # 96 expanded vertices per VU1 batch
MAX_VERTS = MAX_TRIS * 3
VU_POS, VU_UV = 4, 4 + MAX_VERTS
VIF_NOP = 0
VIF_UNPACK_V4_16 = 0x6D        # 0x60 | (V4 << 2) | 16-bit
TOPS_RELATIVE = 0x8000         # imm bit 15 (FLG); bit 14 clear = signed
RENDERED_VERTEX, LIGHTMAP_VERTEX = 32, 8
NO_LIGHTMAP = 0xFFFF


class ConvertError(ValueError):
    pass


@dataclass
class Material:
    shader_id: int
    lightmap_page: int
    pos: list[tuple[float, float, float]]
    uv: list[tuple[float, float]]
    lmuv: list[tuple[float, float]]
    tris: list[tuple[int, int, int]]


@dataclass
class Batch:
    material: int
    triangles: int
    vif: bytes
    centre: tuple[float, float, float]
    radius: float


@dataclass
class Section:
    bounds: tuple[float, ...]
    materials: list[dict]
    batches: list[dict]
    triangles: int
    vif: bytes


# ---- reading the Xbox BSP -----------------------------------------------------------
def read_materials(block: bytes, loc: bsp.BspLocation) -> tuple[list[Material], tuple[float, ...]]:
    A = loc.address
    ptr = struct.unpack_from("<I", block, 0)[0]
    root = ptr - A

    def refl(off: int, stride: int) -> bytes:
        count, addr = struct.unpack_from("<II", block, root + off)
        o = addr - A
        if count and (o < 0 or o + count * stride > len(block)):
            raise ConvertError(f"reflexive at +{off:#x} outside the block")
        return block[o:o + count * stride] if count else b""

    surfaces = refl(0xF8, 6)
    nsurf = len(surfaces) // 6
    bounds_raw = struct.unpack_from("<6f", block, root + 0xC8)
    bounds = (bounds_raw[0], bounds_raw[2], bounds_raw[4], bounds_raw[1], bounds_raw[3], bounds_raw[5])
    out: list[Material] = []
    lms = refl(0x104, bsp.LIGHTMAP_SIZE)
    for li in range(len(lms) // bsp.LIGHTMAP_SIZE):
        page = struct.unpack_from("<H", lms, li * bsp.LIGHTMAP_SIZE)[0]
        mcount, maddr = struct.unpack_from("<II", lms, li * bsp.LIGHTMAP_SIZE + 0x14)
        mo = maddr - A
        if mcount and (mo < 0 or mo + mcount * bsp.MATERIAL_SIZE > len(block)):
            raise ConvertError("lightmap materials outside the block")
        for m in range(mcount):
            mat = block[mo + m * bsp.MATERIAL_SIZE:mo + (m + 1) * bsp.MATERIAL_SIZE]
            shader_id = struct.unpack_from("<I", mat, 0x0C)[0]
            first, n = struct.unpack_from("<ii", mat, 0x14)
            rv, lv = struct.unpack_from("<I", mat, 0xB4)[0], struct.unpack_from("<I", mat, 0xC8)[0]
            if struct.unpack_from("<I", mat, 0xD8)[0]:
                raise ConvertError("uncompressed vertex data present (not seen on retail maps)")
            csize, _f, _o, cptr = struct.unpack_from("<4I", mat, 0xEC)
            if n == 0 or rv == 0:
                continue
            if csize != rv * RENDERED_VERTEX + lv * LIGHTMAP_VERTEX:
                raise ConvertError(f"vertex blob {csize} != {rv}x32 + {lv}x8")
            vo = cptr - A
            if vo < 0 or vo + csize > len(block):
                raise ConvertError("vertex blob outside the block")
            if first < 0 or first + n > nsurf:
                raise ConvertError("material surface range outside the surface array")
            blob = block[vo:vo + csize]
            pos, uv, lmuv = [], [], []
            for i in range(rv):
                x, y, z = struct.unpack_from("<3f", blob, i * RENDERED_VERTEX)
                u, v = struct.unpack_from("<2f", blob, i * RENDERED_VERTEX + 24)
                pos.append((x, y, z))
                uv.append((u, v))
            lbase = rv * RENDERED_VERTEX
            for i in range(rv):
                if i < lv:
                    a, b = struct.unpack_from("<2h", blob, lbase + i * LIGHTMAP_VERTEX + 4)
                    lmuv.append((a / 32767.0, b / 32767.0))
                else:
                    lmuv.append((0.0, 0.0))
            tris = [struct.unpack_from("<3H", surfaces, (first + s) * 6) for s in range(n)]
            if any(max(t) >= rv for t in tris):
                raise ConvertError("surface index beyond the material's vertex array")
            out.append(Material(shader_id, page if lv else NO_LIGHTMAP, pos, uv, lmuv, tris))
    return out, bounds


# ---- baking ---------------------------------------------------------------------------
def _s16(v: float) -> int:
    return max(-32768, min(32767, int(round(v))))


def _unpack(addr: int, data: bytes) -> bytes:
    """UNPACK V4-16 (signed, TOPS-relative): one command word then `num` 8-byte elements."""
    num = len(data) // 8
    return struct.pack("<I", (VIF_UNPACK_V4_16 << 24) | (num << 16) | TOPS_RELATIVE | addr) + data


SANE_UV = 64.0         # tiling beyond +-64 is not authored; larger values are garbage and clamp


def uv_shift(uvs: list[tuple[float, float]]) -> int:
    """Largest power of two such that every sane texture uv fits an s16 (retail data has a
    handful of vertices with |u| > 1e6, which are ignored here and clamp when baked)."""
    ref = max((abs(c) for uv in uvs for c in uv if math.isfinite(c) and abs(c) <= SANE_UV), default=1.0)
    return max(0, min(14, int(math.floor(math.log2(32767.0 / max(ref, 1.0))))))


def bake_material(index: int, m: Material) -> tuple[dict, list[Batch]]:
    shift = uv_shift(m.uv)
    batches: list[Batch] = []
    for start in range(0, len(m.tris), MAX_TRIS):
        chunk = m.tris[start:start + MAX_TRIS]
        pts = [m.pos[vi] for tri in chunk for vi in tri]
        c = tuple(sum(p[k] for p in pts) / len(pts) for k in range(3))
        r = max(math.dist(c, p) for p in pts)
        scale = (r / 32767.0) or 1.0          # positions are relative to the batch centre
        pos_b, uv_b = bytearray(), bytearray()
        for tri in chunk:
            for vi in tri:
                p = m.pos[vi]
                pos_b += struct.pack("<4h", *[_s16((p[k] - c[k]) / scale) for k in range(3)], 0)
                u, v = (x if math.isfinite(x) else 0.0 for x in m.uv[vi])
                uv_b += struct.pack("<4h", _s16(u * (1 << shift)), _s16(v * (1 << shift)),
                                    _s16(m.lmuv[vi][0] * 32767), _s16(m.lmuv[vi][1] * 32767))
        raw = _unpack(VU_POS, bytes(pos_b)) + _unpack(VU_UV, bytes(uv_b))
        raw += bytes(-len(raw) % 16)
        batches.append(Batch(index, len(chunk), raw, c, r))
    rec = {"shader_id": m.shader_id, "lightmap_page": m.lightmap_page, "shift": shift,
           "triangles": len(m.tris)}
    return rec, batches


def build_section(materials: list[Material], bounds: tuple[float, ...]) -> bytes:
    mats, batches = [], []
    for i, m in enumerate(materials):
        rec, bs = bake_material(i, m)
        rec["first_batch"], rec["batch_count"] = len(batches), len(bs)
        mats.append(rec)
        batches += bs
    mat_off = HEADER_SIZE
    batch_off = mat_off + MATERIAL_REC * len(mats)
    vif_off = (batch_off + BATCH_REC * len(batches) + 15) & ~15
    vif, table = bytearray(), bytearray()
    for b in batches:
        table += struct.pack("<HHII3ffI", b.material, b.triangles, len(vif), len(b.vif) // 16, *b.centre, b.radius, 0)
        vif += b.vif
    mtab = bytearray()
    for r in mats:
        mtab += struct.pack("<IHHHHI16x", r["shader_id"], r["lightmap_page"], r["first_batch"],
                            r["batch_count"], r["shift"], r["triangles"])
    total = vif_off + len(vif)
    head = struct.pack("<4sHHIIIIII6fII", MAGIC, VERSION, 0, len(mats), len(batches),
                       sum(r["triangles"] for r in mats), mat_off, batch_off, vif_off, *bounds, total, 0)
    out = bytearray(head + mtab + table)
    out += bytes(vif_off - len(out))
    return bytes(out + vif)


# ---- reading back (tests and --info) ------------------------------------------------------
def parse_section(data: bytes) -> Section:
    if len(data) < HEADER_SIZE or data[:4] != MAGIC:
        raise ConvertError("not a BSPS section")
    (_m, ver, _r, nm, nb, tris, mo, bo, vo, *rest) = struct.unpack_from("<4sHHIIIIII6fII", data)
    bounds, total = tuple(rest[:6]), rest[6]
    if ver != VERSION or total != len(data) or mo + nm * MATERIAL_REC > len(data) \
            or bo + nb * BATCH_REC > len(data) or vo > len(data):
        raise ConvertError("inconsistent BSPS header")
    materials = []
    for i in range(nm):
        sid, page, fb, bc, shift, t = struct.unpack_from("<IHHHHI", data, mo + i * MATERIAL_REC)
        materials.append({"shader_id": sid, "lightmap_page": page, "first_batch": fb, "batch_count": bc,
                          "shift": shift, "triangles": t})
    batches = []
    for i in range(nb):
        mat, t, off, qw, cx, cy, cz, r, _ = struct.unpack_from("<HHII3ffI", data, bo + i * BATCH_REC)
        if t > MAX_TRIS or vo + off + qw * 16 > len(data) or off % 16:
            raise ConvertError(f"batch {i}: bad size/offset")
        batches.append({"material": mat, "triangles": t, "vif_offset": off, "vif_qwords": qw,
                        "centre": (cx, cy, cz), "radius": r})
    if sum(m["triangles"] for m in materials) != tris or sum(b["triangles"] for b in batches) != tris:
        raise ConvertError("triangle counts disagree")
    return Section(bounds, materials, batches, tris, data[vo:])


def decode_batch(sec: Section, batch: dict) -> list[dict]:
    """Expanded vertices of one batch: world position, texture uv, lightmap uv."""
    mat = sec.materials[batch["material"]]
    raw = sec.vif[batch["vif_offset"]:batch["vif_offset"] + batch["vif_qwords"] * 16]
    verts: list[dict] = []
    streams, i = {}, 0
    while i < len(raw):
        word = struct.unpack_from("<I", raw, i)[0]
        i += 4
        if word == VIF_NOP:
            continue
        cmd, num, imm = word >> 24, (word >> 16) & 0xFF, word & 0xFFFF
        if cmd != VIF_UNPACK_V4_16 or not imm & TOPS_RELATIVE:
            raise ConvertError(f"unexpected VIF command {word:#010x}")
        streams[imm & 0x3FF] = [struct.unpack_from("<4h", raw, i + k * 8) for k in range(num)]
        i += num * 8
    scale = (batch["radius"] / 32767.0) or 1.0
    pos, uv = streams[VU_POS], streams[VU_UV]
    if len(pos) != batch["triangles"] * 3 or len(uv) != len(pos):
        raise ConvertError("vertex stream length does not match the triangle count")
    for (x, y, z, _), (s, t, ls, lt) in zip(pos, uv):
        verts.append({"pos": tuple(batch["centre"][k] + q * scale for k, q in enumerate((x, y, z))),
                      "uv": (s / (1 << mat["shift"]), t / (1 << mat["shift"])),
                      "lmuv": (ls / 32767.0, lt / 32767.0)})
    return verts


def convert_map(path: Path, bsp_indices: list[int] | None = None) -> bytes:
    hdr = mapinfo.read_header(path)
    idx = tagindex.TagIndex.read(path)
    with path.open("rb") as f:
        f.seek(hdr.tag_data_offset)
        region = f.read(hdr.tag_data_size)
        locs = bsp.locate(idx, region, hdr.decompressed_size)
        sections = []
        for i, loc in enumerate(locs):
            if bsp_indices is not None and i not in bsp_indices:
                continue
            f.seek(loc.file_offset)
            mats, bounds = read_materials(f.read(loc.size), loc)
            sections.append((f"BSP{i}", build_section(mats, bounds)))
    return hp2.build(path.stem, sections)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("input", type=Path)
    ap.add_argument("-o", "--out", type=Path)
    ap.add_argument("--bsp", type=int, action="append", help="only this BSP index (repeatable)")
    ap.add_argument("--info", action="store_true", help="input is a .hp2: print its BSP sections")
    args = ap.parse_args(argv)
    if args.info:
        pk = hp2.parse(args.input.read_bytes())
        for s in pk.sections:
            if s.tag.startswith("BSP"):
                sec = parse_section(pk.read(s.tag))
                print(f"{s.tag}: {len(sec.materials)} materials, {len(sec.batches)} batches, "
                      f"{sec.triangles} triangles, {s.raw_size / 2**20:.2f} MiB raw, "
                      f"{s.stored_size / 2**20:.2f} MiB stored")
        return 0
    if not args.out:
        ap.error("-o is required")
    data = convert_map(args.input, args.bsp)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_bytes(data)
    print(f"wrote {args.out} ({len(data) / 2**20:.2f} MiB)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
