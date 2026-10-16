#!/usr/bin/env python3
"""Locate and summarise the structure BSPs of a decompressed Xbox map.

Each BSP is a self-contained block of the map file, loaded on the Xbox at its own base
address; every pointer inside it is absolute relative to that address.

scenario +0x5A4 reflexive -> bsp entries[32]:
    +0x00 u32 file offset of the block, +0x04 u32 block size, +0x08 u32 load address,
    +0x10 tag reference: group 'sbsp', path pointer, path length, tag id (+0x1C)

BSP block starts with a 20-byte compiled header (pointer to the structure_bsp struct,
lightmap material count, rendered vertex pointer, count again, lightmap vertex pointer,
'sbsp' signature) and holds the struct and everything it points to.

structure_bsp struct (Xbox: the three world-bounds fields are 8-byte bounds, so offsets
after 0xC8 are 12 bytes later than the PC/Invader definition; verified on all maps):
    +0x0A4 collision materials  +0x0BC nodes (6 B)      +0x0E0 leaves (16 B)
    +0x0EC leaf surfaces (8 B)  +0x0F8 surfaces (6 B)   +0x104 lightmaps (32 B)
    +0x128 clusters (16 B on Xbox; 104 B on PC)         +0x134 cluster portals
    +0x16C fog planes           +0x1D8 pathfinding surfaces   +0x234 markers
(the last four are only counted for now; their Xbox element sizes are still unverified)
lightmap: +0x14 materials reflexive; material (256 B): +0x14 first surface,
+0x18 surface count, +0xB0 rendered vertex type, +0xB4 rendered vertex count,
+0xC4 lightmap vertex type, +0xC8 lightmap vertex count.

Usage:
    python3 -I tools/halo2ps2/bsp.py assets/decompressed/a10.map
"""
from __future__ import annotations

import argparse
import struct
import sys
from dataclasses import dataclass, field
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from halo2ps2 import mapinfo, tagindex  # noqa: E402

SCENARIO_BSPS = 0x5A4
BSP_ENTRY_SIZE = 32
HEADER_SIZE = 20
SBSP_MAGIC = b"psbs"   # 'sbsp' stored byte-reversed
MATERIAL_SIZE = 256
LIGHTMAP_SIZE = 32

# name -> (offset in structure_bsp, element size)
REFLEXIVES = {
    "collision_materials": (0x0A4, 20), "nodes": (0x0BC, 6), "leaves": (0x0E0, 16),
    "leaf_surfaces": (0x0EC, 8), "surfaces": (0x0F8, 6), "lightmaps": (0x104, LIGHTMAP_SIZE),
    "clusters": (0x128, 16), "cluster_portals": (0x134, 1), "fog_planes": (0x16C, 1),
    "pathfinding_surfaces": (0x1D8, 1), "markers": (0x234, 1),
}
# Only the count field is read; element sizes are unverified so the pointers are not followed.
COUNT_ONLY = {"cluster_portals", "fog_planes", "pathfinding_surfaces", "markers"}


class BspError(ValueError):
    pass


@dataclass
class BspLocation:
    file_offset: int
    size: int
    address: int
    path: str = ""


@dataclass
class BspSummary:
    path: str
    block_bytes: int
    counts: dict[str, int]
    lightmap_materials: int = 0
    triangles: int = 0
    rendered_vertices: int = 0
    lightmap_vertices: int = 0
    problems: list[str] = field(default_factory=list)


def locate(idx: tagindex.TagIndex, region: bytes, file_size: int) -> list[BspLocation]:
    sc = idx.scenario.data_address - idx.base
    count, addr = struct.unpack_from("<II", region, sc + SCENARIO_BSPS)
    off = addr - idx.base
    if count > 64 or off < 0 or off + count * BSP_ENTRY_SIZE > len(region):
        raise BspError(f"scenario bsp block ({count} at {addr:#x}) outside tag region")
    paths = {e.tag_id: e.path for e in idx.entries if e.group == "sbsp"}
    out = []
    for i in range(count):
        start, size, base, _pad, _grp, _path_ptr, _path_len, tag_id = struct.unpack_from(
            "<8I", region, off + i * BSP_ENTRY_SIZE)
        if start + size > file_size or size < HEADER_SIZE:
            raise BspError(f"bsp {i}: block {start:#x}+{size:#x} outside the map file")
        out.append(BspLocation(start, size, base, paths.get(tag_id, f"#{tag_id:#x}")))
    return out


def summarise(loc: BspLocation, block: bytes) -> BspSummary:
    ptr, nmat, _vp, nmat2, _lp = struct.unpack_from("<5I", block, 0)
    magic = block[0x14:0x18]
    s = BspSummary(loc.path, len(block), {})
    if magic != SBSP_MAGIC:
        raise BspError(f"{loc.path}: missing 'sbsp' signature")
    if nmat != nmat2:
        s.problems.append("lightmap material counts differ")
    s.lightmap_materials = nmat
    root = ptr - loc.address

    def reflexive(off: int, stride: int, name: str) -> tuple[int, bytes]:
        count, addr = struct.unpack_from("<II", block, root + off)
        if count == 0:
            return 0, b""
        o = addr - loc.address
        if o < 0 or o + count * stride > len(block):
            s.problems.append(f"{name}: {count} x {stride} at {addr:#x} outside the block")
            return count, b""
        return count, block[o:o + count * stride]

    if not 0 <= root <= len(block) - 0x27C:
        raise BspError(f"{loc.path}: structure_bsp root {ptr:#x} outside the block")
    for name, (off, stride) in REFLEXIVES.items():
        if name in COUNT_ONLY:
            s.counts[name] = struct.unpack_from("<I", block, root + off)[0]
        else:
            s.counts[name], _ = reflexive(off, stride, name)
    s.triangles = s.counts["surfaces"]
    _, lms = reflexive(*REFLEXIVES["lightmaps"], "lightmaps")
    nsurf = s.counts["surfaces"]
    for i in range(0, len(lms), LIGHTMAP_SIZE):
        mcount, maddr = struct.unpack_from("<II", lms, i + 0x14)
        o = maddr - loc.address
        if mcount == 0:
            continue
        if o < 0 or o + mcount * MATERIAL_SIZE > len(block):
            s.problems.append("lightmap materials outside the block")
            continue
        for m in range(mcount):
            mat = block[o + m * MATERIAL_SIZE:o + (m + 1) * MATERIAL_SIZE]
            first, n = struct.unpack_from("<ii", mat, 0x14)
            rv, lv = struct.unpack_from("<I", mat, 0xB4)[0], struct.unpack_from("<I", mat, 0xC8)[0]
            if first < 0 or n < 0 or first + n > nsurf:
                s.problems.append(f"material surface range {first}+{n} exceeds {nsurf} surfaces")
            s.rendered_vertices += rv
            s.lightmap_vertices += lv
    return s


def summarise_map(path: Path) -> list[BspSummary]:
    hdr = mapinfo.read_header(path)
    idx = tagindex.TagIndex.read(path)
    with path.open("rb") as f:
        f.seek(hdr.tag_data_offset)
        region = f.read(hdr.tag_data_size)
        locs = locate(idx, region, hdr.decompressed_size)
        out = []
        for loc in locs:
            f.seek(loc.file_offset)
            out.append(summarise(loc, f.read(loc.size)))
    return out


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("maps", nargs="+", type=Path)
    args = ap.parse_args(argv)
    rc = 0
    for p in args.maps:
        for s in summarise_map(p):
            c = s.counts
            print(f"{p.stem:<4} {s.path.split(chr(92))[-1]:<12} {s.block_bytes / 2**20:5.2f} MiB  "
                  f"clusters {c['clusters']:4d}  tris {s.triangles:6d}  "
                  f"verts {s.rendered_vertices:6d}/{s.lightmap_vertices:6d}  "
                  f"lightmaps {c['lightmaps']:3d}  portals {c['cluster_portals']:4d}")
            for pr in s.problems:
                print("  problem:", pr, file=sys.stderr)
                rc = 1
    return rc


if __name__ == "__main__":
    sys.exit(main())
