#!/usr/bin/env python3
"""Locate and summarise the structure BSPs of a decompressed Xbox map, and read the parts of
structure_bsp that Phase 1 needs (tree, leaves, clusters, portals, surface ownership).

Each BSP is a self-contained block of the map file, loaded on the Xbox at its own base
address; every pointer inside it is absolute relative to that address.

scenario +0x5A4 reflexive -> bsp entries[32]:
    +0x00 u32 file offset of the block, +0x04 u32 block size, +0x08 u32 load address,
    +0x10 tag reference: group 'sbsp', path pointer, path length, tag id (+0x1C)

BSP block starts with a 20-byte compiled header (pointer to the structure_bsp struct,
lightmap material count, rendered vertex pointer, count again, lightmap vertex pointer,
'sbsp' signature) and holds the struct and everything it points to.

structure_bsp struct, Xbox layout. Reflexive = u32 count, u32 address, u32 pad (12 bytes).
All offsets below were checked on all 11 retail maps (69 BSPs): every table lies inside the
block and its element size reproduces the pointer chain of the neighbouring tables.
    +0x0A4 collision materials (20 B)
    +0x0B0 collision bsp (96 B, count is always 1): eight reflexives of its own, in order
           bsp3d nodes (12 B) / planes (16 B) / leaves (8 B) / bsp2d refs / bsp2d nodes /
           surfaces / edges / vertices (only the first two are read here)
    +0x0BC "nodes" (6 B). Same count as the collision bsp3d nodes but the content is NOT a
           tree (bytes pair up as (b, ~b)-like values); purpose unidentified, ignored.
    +0x0C8 world bounds x min,max, y min,max, z min,max (6 x f32)
    +0x0E0 leaves (16 B)        +0x0EC leaf surfaces (8 B)       +0x0F8 surfaces (6 B)
    +0x104 lightmaps (32 B)     +0x11C lens flares (16 B)        +0x128 lens flare markers (16 B)
    +0x134 clusters (104 B)     +0x140 cluster data (size/ptr, unused)
    +0x154 cluster portals (64 B)                                +0x178 fog planes (32 B)
    +0x184 fog regions          +0x190 fog palette               +0x1B4 weather palette
    +0x1E4 pathfinding surfaces (1 B)  +0x1FC background sounds  +0x208 sound environments
    +0x240 markers (60 B)       +0x24C detail objects            +0x258 runtime decals
(clusters/portals were previously listed at 0x128/0x134 with a 16 B element: wrong, that is the
lens flare marker table; fixed.)

Collision bsp (what the tree is made of; verified: root = node 0, every child index is greater
than its parent, no node has two parents, all indices in range):
    bsp3d node (12 B): u32 plane, u32 back child, u32 front child
        child: 0xFFFFFFFF = none (outside the world), bit 31 set = leaf (index in the low
        bits), otherwise node index.
    plane (16 B): f32 nx, ny, nz, d. A point is in front iff n.p - d >= 0 (portal vertices
        and node splits both satisfy n.p = d to better than 5e-4).
Point location: start at node 0, front child if n.p - d >= 0 else back child, until a leaf.
    leaf (16 B): +0x00 3 x u16 (6 B, unidentified, ignored)  +0x06 u16 always 0
        +0x08 u16 cluster (checked < cluster count on all maps; 0xFFFF reserved for none)
        +0x0A u16 surface count  +0x0C u32 first index into the leaf surface table
        (the first index needs the full 32 bits: a10b has 78052 leaf surfaces; the table is
        consecutive: first = sum of previous counts)
    leaf surface (8 B): +0x00 u32 surface index (< surface count on all maps), +0x04 u32 other
        index (not a collision bsp3d node index: exceeds the node count on most maps; unused)
    surface (6 B): 3 x u16 vertex indices local to the lightmap material that owns it.

Clusters (104 B). Verified fields:
    +0x00 s16 sky (-1 none, else index into the scenario's sky palette; range not checkable here)
    +0x02 s16 fog: -1 none, else low 15 bits index the BSP's fog planes (all 700 uses in range);
          bit 15 is set on 634 of 700, meaning unknown
    +0x04 s16 background sound   +0x06 s16 sound environment   +0x08 s16 weather
          (all three in range of their palettes, -1 none)
    +0x0A s16 transition bsp (always 0)       +0x0C s16 first decal index, +0x0E s16 count
    +0x28 predicted resources (8 B, ignored)
    +0x34 subclusters (36 B): 6 x f32 bounds (x min,max, y min,max, z min,max) then a reflexive
          of u32 surface indices at +0x18
    +0x40 s16 first lens flare marker, s16 count (running prefix over the clusters)
    +0x44 surface indices: always empty on Xbox (the surfaces live in the subclusters)
    +0x50 mirrors (count read only)
    +0x5C portal list: s16 indices into the cluster portal table (symmetric with the portals'
          cluster pair on all maps)
Cluster portals (64 B), verified including the polygons:
    +0x00 s16 cluster on the BACK side of the plane   +0x02 s16 cluster on the FRONT side
          (checked by point-locating points 3 cm either side of the 3676 portals: of 14098
          decisive samples none had the sides swapped; 15 portals have the same cluster on both
          sides and some sample points fall in solid space, those are not decisive)
    +0x04 s32 plane index into the collision bsp planes
    +0x08 f32[3] centre, +0x14 f32 radius, +0x18 u32 flags (always 0)
    +0x34 vertices reflexive, 12 B each (f32 x3): planar to < 5e-4, counter-clockwise seen from
          the front of the plane; 2 of 3676 polygons are slightly concave (others have collinear
          vertices), up to 50 vertices.
Surface -> cluster: every surface is listed by the leaves of its clusters (leaf -> cluster) and
independently by the subclusters of those clusters. The two routes agree exactly except for one
surface (b40_c1); some BSPs have a few surfaces (<= 37) that neither route reaches (the Xbox
renderer, which draws cluster -> subcluster -> surface, never draws them either).

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

STRUCT_SIZE = 0x27C        # bytes of structure_bsp up to and including the last reflexive read
CLUSTER_SIZE = 104
PORTAL_SIZE = 64
SUBCLUSTER_SIZE = 36
LEAF_SIZE = 16
NODE_SIZE = 12             # collision bsp3d node
PLANE_SIZE = 16
NO_CHILD = 0xFFFFFFFF
LEAF_FLAG = 0x80000000
NO_CLUSTER = 0xFFFF

# name -> (offset in structure_bsp, element size)
REFLEXIVES = {
    "collision_materials": (0x0A4, 20), "collision_bsp": (0x0B0, 96), "nodes": (0x0BC, 6),
    "leaves": (0x0E0, LEAF_SIZE), "leaf_surfaces": (0x0EC, 8), "surfaces": (0x0F8, 6),
    "lightmaps": (0x104, LIGHTMAP_SIZE), "lens_flares": (0x11C, 16), "lens_flare_markers": (0x128, 16),
    "clusters": (0x134, CLUSTER_SIZE), "cluster_portals": (0x154, PORTAL_SIZE), "fog_planes": (0x178, 32),
    "pathfinding_surfaces": (0x1E4, 1), "markers": (0x240, 60),
}
# Only the count field is read; the element size is not needed (or not verified).
COUNT_ONLY = {"pathfinding_surfaces"}


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
    unreferenced_surfaces: int = 0     # surfaces no leaf and no subcluster lists (never drawn)


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


@dataclass
class Cluster:
    sky: int
    fog: int                     # raw s16 (see module docstring)
    background_sound: int
    sound_environment: int
    weather: int
    mirrors: int
    portals: list[int]           # indices into Structure.portals
    surfaces: set[int]           # surfaces listed by this cluster's subclusters


@dataclass
class Portal:
    back: int                    # cluster behind the plane (-1: none)
    front: int                   # cluster in front of the plane (-1: none)
    plane: tuple[float, float, float, float]    # n.p = d
    flags: int
    centre: tuple[float, float, float]
    radius: float
    vertices: list[tuple[float, float, float]]  # counter-clockwise seen from the front


@dataclass
class Structure:
    """Everything of one structure_bsp that is not material geometry, fully range-checked."""
    surface_count: int
    planes: list[tuple[float, float, float, float]]
    nodes: list[tuple[int, int, int]]           # (plane, front, back); child NO_CHILD / LEAF_FLAG|leaf / node
    leaves: list[int]                           # cluster per leaf (NO_CLUSTER: none)
    leaf_surfaces: list[tuple[int, ...]]        # surface indices per leaf
    clusters: list[Cluster]
    portals: list[Portal]
    fog_planes: int
    cluster_surfaces: dict[int, set[int]] = field(default_factory=dict)   # surface -> clusters (both routes)


def read_structure(loc: BspLocation, block: bytes) -> Structure:
    """Parse and validate the tree, leaves, clusters and portals of a BSP block."""
    A = loc.address
    if len(block) < HEADER_SIZE:
        raise BspError("block shorter than the compiled header")
    if block[0x14:0x18] != SBSP_MAGIC:
        raise BspError(f"{loc.path}: missing 'sbsp' signature")
    root = struct.unpack_from("<I", block, 0)[0] - A
    if not 0 <= root <= len(block) - STRUCT_SIZE:
        raise BspError(f"{loc.path}: structure_bsp root outside the block")

    def table(off: int, stride: int, name: str, base: int | None = None) -> tuple[int, int]:
        at = (root + off) if base is None else base + off
        count, addr = struct.unpack_from("<II", block, at)
        if count == 0:
            return 0, 0
        o = addr - A
        if o < 0 or count > 0x400000 or o + count * stride > len(block):
            raise BspError(f"{name}: {count} x {stride} at {addr:#x} outside the block")
        return count, o

    cb_n, cb_o = table(0xB0, 96, "collision bsp")
    if cb_n < 1:
        raise BspError("no collision bsp (the point-location tree lives there)")
    pn, po = table(0x0C, PLANE_SIZE, "bsp planes", base=cb_o)
    nn, no = table(0x00, NODE_SIZE, "bsp nodes", base=cb_o)
    cb_leaves = struct.unpack_from("<I", block, cb_o + 0x18)[0]
    ln, lo = table(0xE0, LEAF_SIZE, "leaves")
    if ln != cb_leaves:
        raise BspError(f"{ln} leaves but the collision bsp has {cb_leaves}")
    lsn, lso = table(0xEC, 8, "leaf surfaces")
    sn, _ = table(0xF8, 6, "surfaces")
    cn, co = table(0x134, CLUSTER_SIZE, "clusters")
    pon, poo = table(0x154, PORTAL_SIZE, "cluster portals")
    fog_planes = struct.unpack_from("<I", block, root + 0x178)[0]

    planes = [t for t in struct.iter_unpack("<4f", block[po:po + pn * PLANE_SIZE])]
    nodes = []
    for k, (plane, back, front) in enumerate(struct.iter_unpack("<3I", block[no:no + nn * NODE_SIZE])):
        if plane >= pn:
            raise BspError(f"node {k}: plane {plane} >= {pn}")
        for c in (back, front):
            if c == NO_CHILD:
                continue
            if c & LEAF_FLAG:
                if (c & ~LEAF_FLAG) >= ln:
                    raise BspError(f"node {k}: leaf child {c & ~LEAF_FLAG} >= {ln}")
            elif not k < c < nn:
                raise BspError(f"node {k}: child {c} is not a later node (< {nn})")
        nodes.append((plane, front, back))
    if nn == 0 and ln > 1:
        raise BspError("no nodes but several leaves")

    leaf_surface = [t[0] for t in struct.iter_unpack("<II", block[lso:lso + lsn * 8])]
    for v in leaf_surface:
        if v >= sn:
            raise BspError(f"leaf surface {v} >= {sn} surfaces")
    leaves, leaf_surfaces = [], []
    for k in range(ln):
        e = lo + k * LEAF_SIZE
        cluster, count = struct.unpack_from("<HH", block, e + 8)
        first = struct.unpack_from("<I", block, e + 12)[0]
        if cluster != NO_CLUSTER and cluster >= cn:
            raise BspError(f"leaf {k}: cluster {cluster} >= {cn}")
        if count and first + count > lsn:      # an empty leaf may carry any first index (ui: 0xFFFFFFFF)
            raise BspError(f"leaf {k}: surfaces {first}+{count} > {lsn}")
        leaves.append(cluster)
        leaf_surfaces.append(tuple(leaf_surface[first:first + count]) if count else ())

    clusters: list[Cluster] = []
    for c in range(cn):
        e = co + c * CLUSTER_SIZE
        sky, fog, bg, env, weather = struct.unpack_from("<5h", block, e)
        mirrors = struct.unpack_from("<I", block, e + 0x50)[0]
        sub_n, sub_o = table(0x34, SUBCLUSTER_SIZE, f"cluster {c} subclusters", base=e)
        surfs: set[int] = set()
        for k in range(sub_n):
            n2, o2 = table(0x18, 4, f"cluster {c} subcluster {k} surfaces", base=sub_o + k * SUBCLUSTER_SIZE)
            for v in struct.unpack_from(f"<{n2}I", block, o2) if n2 else ():
                if v >= sn:
                    raise BspError(f"cluster {c}: subcluster surface {v} >= {sn}")
                surfs.add(v)
        pl_n, pl_o = table(0x5C, 2, f"cluster {c} portal list", base=e)
        plist = list(struct.unpack_from(f"<{pl_n}h", block, pl_o)) if pl_n else []
        if any(not 0 <= v < pon for v in plist):
            raise BspError(f"cluster {c}: portal index outside 0..{pon}")
        clusters.append(Cluster(sky, fog, bg, env, weather, mirrors, plist, surfs))

    portals: list[Portal] = []
    for k in range(pon):
        e = poo + k * PORTAL_SIZE
        back, front, plane = struct.unpack_from("<hhi", block, e)
        centre = struct.unpack_from("<3f", block, e + 8)
        radius, flags = struct.unpack_from("<fI", block, e + 0x14)
        vn, vo = table(0x34, 12, f"portal {k} vertices", base=e)
        if not 0 <= plane < pn or not -1 <= back < cn or not -1 <= front < cn:
            raise BspError(f"portal {k}: plane {plane} / clusters {back},{front} out of range")
        verts = [t for t in struct.iter_unpack("<3f", block[vo:vo + vn * 12])]
        portals.append(Portal(back, front, planes[plane], flags, centre, radius, verts))
    for c, cl in enumerate(clusters):
        for k in cl.portals:
            if c not in (portals[k].back, portals[k].front):
                raise BspError(f"cluster {c} lists portal {k}, which does not touch it")

    cluster_surfaces: dict[int, set[int]] = {}
    for leaf, surfs in zip(leaves, leaf_surfaces):
        if leaf != NO_CLUSTER:
            for v in surfs:
                cluster_surfaces.setdefault(v, set()).add(leaf)
    for c, cl in enumerate(clusters):
        for v in cl.surfaces:
            cluster_surfaces.setdefault(v, set()).add(c)
    return Structure(sn, planes, nodes, leaves, leaf_surfaces, clusters, portals, fog_planes, cluster_surfaces)


def find_cluster(st: Structure, p: tuple[float, float, float]) -> int:
    """Reference point location (the C reader's bsp_find_cluster): -1 outside the world."""
    if not st.nodes:
        return st.leaves[0] if len(st.leaves) == 1 and st.leaves[0] != NO_CLUSTER else -1
    n = 0
    for _ in range(len(st.nodes) + 1):
        plane, front, back = st.nodes[n]
        pl = st.planes[plane]
        c = front if pl[0] * p[0] + pl[1] * p[1] + pl[2] * p[2] - pl[3] >= 0.0 else back
        if c == NO_CHILD:
            return -1
        if c & LEAF_FLAG:
            cl = st.leaves[c & ~LEAF_FLAG]
            return -1 if cl == NO_CLUSTER else cl
        n = c
    return -1


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
            block = f.read(loc.size)
            s = summarise(loc, block)
            try:
                st = read_structure(loc, block)
                s.unreferenced_surfaces = st.surface_count - len(st.cluster_surfaces)
            except BspError as e:
                s.problems.append(str(e))
            out.append(s)
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
                  f"lightmaps {c['lightmaps']:3d}  portals {c['cluster_portals']:4d}  "
                  f"leaves {c['leaves']:5d}  unreferenced {s.unreferenced_surfaces:3d}")
            for pr in s.problems:
                print("  problem:", pr, file=sys.stderr)
                rc = 1
    return rc


if __name__ == "__main__":
    sys.exit(main())
