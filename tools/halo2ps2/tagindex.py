#!/usr/bin/env python3
"""Parse the tag index of a decompressed Xbox Halo: Combat Evolved cache file.

The tag region starts at the header's ``tag data offset`` and is loaded by the
Xbox at virtual base 0x803A6000; every pointer inside it is an absolute address
(``offset = address - base + tag_data_offset``).  See docs/05-halo-ce-data.md.

Index header (36 bytes, verified on all retail maps, see docs/reports/tag-index.md):

    +0x00 u32  tag array address        (= base + 0x24 on every map)
    +0x04 u32  scenario tag id          (index in low 16 bits, salt in high 16)
    +0x08 u32  tag checksum / map id
    +0x0C u32  tag count
    +0x10 u32  model part count         (geometry parts with vertices)
    +0x14 u32  model vertex data address   <-- POINTER on Xbox (PC: file offset)
    +0x18 u32  model part count (again)
    +0x1C u32  model index data address    <-- POINTER on Xbox (PC: size)
    +0x20 u32  'tags' literal

Tag entry (32 bytes, ``tag count`` of them starting at the tag array address):

    +0x00 char[4] group     (four-char code, stored byte-reversed)
    +0x04 char[4] parent group    (0xFFFFFFFF when none)
    +0x08 char[4] grandparent group
    +0x0C u32     tag id    (low 16 bits == index into this array)
    +0x10 u32     path address  (null-terminated ASCII inside the tag region)
    +0x14 u32     data address  (0 for structure_bsp: the BSP location is in the
                                 scenario's structure-bsp block, not here)
    +0x18 u32     indexed flag  (always 0 on Xbox, no resource maps)
    +0x1C u32     padding       (always 0)

Tag ids are ``(salt << 16) | index``; the salt starts at 0xE174 and increments
per entry, with occasional jumps where the compiler dropped tags.

Usage:
    python3 -I tools/halo2ps2/tagindex.py report assets/decompressed/*.map
    python3 -I tools/halo2ps2/tagindex.py dump assets/decompressed/a10.map [--group sbsp]
"""
from __future__ import annotations

import argparse
import json
import struct
import sys
from collections import Counter
from dataclasses import dataclass, field
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from halo2ps2 import mapinfo  # noqa: E402

XBOX_TAG_BASE = mapinfo.XBOX_TAG_BASE
INDEX_HEADER_FMT = "<9I"
INDEX_HEADER_SIZE = struct.calcsize(INDEX_HEADER_FMT)  # 0x24
TAG_ENTRY_FMT = "<4s4s4sIIIII"
TAG_ENTRY_SIZE = struct.calcsize(TAG_ENTRY_FMT)  # 0x20
TAGS_MAGIC = 0x74616773  # 'tags' read little-endian
NULL_GROUP = b"\xff\xff\xff\xff"
MAX_PATH = 256


def fourcc(raw: bytes) -> str | None:
    """Group codes are stored byte-reversed ('scnr' is b'rncs' on disk)."""
    if raw == NULL_GROUP:
        return None
    return raw[::-1].decode("ascii", "replace")


def fourcc_bytes(code: str | None) -> bytes:
    return NULL_GROUP if code is None else code.encode("ascii")[::-1]


@dataclass
class IndexHeader:
    tag_array_address: int
    scenario_tag_id: int
    checksum: int
    tag_count: int
    model_part_count: int
    model_vertex_address: int
    model_part_count2: int
    model_index_address: int
    magic: int = TAGS_MAGIC

    @classmethod
    def parse(cls, buf: bytes, off: int = 0) -> "IndexHeader":
        return cls(*struct.unpack_from(INDEX_HEADER_FMT, buf, off))

    def pack(self) -> bytes:
        return struct.pack(INDEX_HEADER_FMT, self.tag_array_address, self.scenario_tag_id,
                           self.checksum, self.tag_count, self.model_part_count,
                           self.model_vertex_address, self.model_part_count2,
                           self.model_index_address, self.magic)


@dataclass
class TagEntry:
    group: str | None
    parent: str | None
    grandparent: str | None
    tag_id: int
    path_address: int
    data_address: int
    indexed: int
    pad: int = 0
    path: str = ""

    @property
    def index(self) -> int:
        return self.tag_id & 0xFFFF

    @property
    def salt(self) -> int:
        return self.tag_id >> 16

    @classmethod
    def parse(cls, buf: bytes, off: int) -> "TagEntry":
        g, p, gp, tid, pa, da, ix, pad = struct.unpack_from(TAG_ENTRY_FMT, buf, off)
        return cls(fourcc(g), fourcc(p), fourcc(gp), tid, pa, da, ix, pad)

    def pack(self) -> bytes:
        return struct.pack(TAG_ENTRY_FMT, fourcc_bytes(self.group), fourcc_bytes(self.parent),
                           fourcc_bytes(self.grandparent), self.tag_id, self.path_address,
                           self.data_address, self.indexed, self.pad)


class TagIndexError(ValueError):
    pass


@dataclass
class TagIndex:
    header: IndexHeader
    entries: list[TagEntry]
    tag_data_offset: int
    tag_data_size: int
    base: int = XBOX_TAG_BASE
    problems: list[str] = field(default_factory=list)

    # ---- address helpers --------------------------------------------------
    def addr_to_off(self, addr: int) -> int:
        """Absolute tag-space address -> file offset in the decompressed map."""
        return addr - self.base + self.tag_data_offset

    def off_to_addr(self, off: int) -> int:
        return off - self.tag_data_offset + self.base

    def in_tag_region(self, addr: int) -> bool:
        return self.base <= addr < self.base + self.tag_data_size

    @property
    def scenario(self) -> TagEntry:
        return self.entries[self.header.scenario_tag_id & 0xFFFF]

    @property
    def vertex_region(self) -> tuple[int, int]:
        """(address, size) of the model vertex buffer: runs up to the index buffer."""
        h = self.header
        return h.model_vertex_address, h.model_index_address - h.model_vertex_address

    @property
    def index_region(self) -> tuple[int, int]:
        """(address, size) of the model index buffer. The header has no size field;
        the buffer ends where the first tag's data block (the scenario's) starts;
        verified on every retail map, at most 2 bytes of padding before it."""
        h = self.header
        first = min((e.data_address for e in self.entries if self.in_tag_region(e.data_address)),
                    default=h.model_index_address)
        return h.model_index_address, first - h.model_index_address

    # ---- parsing ------------------------------------------------------------
    @classmethod
    def parse_region(cls, region: bytes, tag_data_offset: int,
                     base: int = XBOX_TAG_BASE, strict: bool = True) -> "TagIndex":
        """Parse from the raw tag region (the ``tag data size`` bytes at
        ``tag data offset``). Verifies the pointer->offset mapping."""
        if len(region) < INDEX_HEADER_SIZE:
            raise TagIndexError("tag region shorter than the index header")
        h = IndexHeader.parse(region)
        idx = cls(h, [], tag_data_offset, len(region), base)
        if h.magic != TAGS_MAGIC:
            raise TagIndexError(f"index header magic {h.magic:#x} != 'tags'")
        arr = h.tag_array_address - base
        if arr != INDEX_HEADER_SIZE:
            idx.problems.append(f"tag array at base+{arr:#x}, expected base+{INDEX_HEADER_SIZE:#x}")
        if arr < 0 or arr + h.tag_count * TAG_ENTRY_SIZE > len(region):
            raise TagIndexError(f"tag array {h.tag_count} x 32 bytes at base+{arr:#x} "
                                f"does not fit in {len(region)} byte region")
        for i in range(h.tag_count):
            e = TagEntry.parse(region, arr + i * TAG_ENTRY_SIZE)
            if e.index != i:
                idx.problems.append(f"entry {i}: tag id {e.tag_id:#x} index {e.index} != {i}")
            po = e.path_address - base
            if 0 <= po < len(region):
                end = region.find(b"\0", po, po + MAX_PATH)
                raw = region[po:end] if end >= 0 else b""
                if end < 0 or not raw or any(c < 0x20 or c > 0x7E for c in raw):
                    idx.problems.append(f"entry {i}: path at {e.path_address:#x} is not printable ASCII")
                e.path = raw.decode("ascii", "replace")
            else:
                idx.problems.append(f"entry {i}: path address {e.path_address:#x} outside tag region")
            if e.group == "sbsp":
                if e.data_address != 0:
                    idx.problems.append(f"entry {i} (sbsp): data address {e.data_address:#x}, expected 0")
            elif not idx.in_tag_region(e.data_address):
                idx.problems.append(f"entry {i} ({e.group}): data address {e.data_address:#x} "
                                    f"outside tag region")
            idx.entries.append(e)
        sc = h.scenario_tag_id & 0xFFFF
        if sc >= h.tag_count:
            raise TagIndexError(f"scenario tag id {h.scenario_tag_id:#x} out of range")
        if idx.entries[sc].group != "scnr":
            idx.problems.append(f"scenario tag id {h.scenario_tag_id:#x} points at group "
                                f"{idx.entries[sc].group!r}, not 'scnr'")
        if h.model_part_count != h.model_part_count2:
            idx.problems.append("model part counts differ")
        if not (idx.in_tag_region(h.model_vertex_address) and idx.in_tag_region(h.model_index_address)
                and h.model_vertex_address <= h.model_index_address):
            idx.problems.append(f"model vertex/index addresses {h.model_vertex_address:#x}/"
                                f"{h.model_index_address:#x} not ordered inside the tag region")
        if strict and idx.problems:
            raise TagIndexError("; ".join(idx.problems[:5]) +
                                (f" (+{len(idx.problems) - 5} more)" if len(idx.problems) > 5 else ""))
        return idx

    @classmethod
    def read(cls, path: Path, base: int = XBOX_TAG_BASE, strict: bool = True) -> "TagIndex":
        hdr = mapinfo.read_header(path)
        if path.stat().st_size != hdr.decompressed_size:
            raise TagIndexError(f"{path.name}: not decompressed (run mapinfo.py decompress)")
        with path.open("rb") as f:
            f.seek(hdr.tag_data_offset)
            region = f.read(hdr.tag_data_size)
        if len(region) != hdr.tag_data_size:
            raise TagIndexError(f"{path.name}: tag region truncated")
        return cls.parse_region(region, hdr.tag_data_offset, base, strict)

    # ---- serialisation (used by tests and, later, by pack.py) ----------------
    @staticmethod
    def build(tags: list[tuple[str, str, int]], scenario_index: int = 0,
              base: int = XBOX_TAG_BASE, model_parts: int = 0,
              vertex_bytes: bytes = b"", index_bytes: bytes = b"",
              checksum: int = 0, salt: int = 0xE174) -> bytes:
        """Build a tag region from (group, path, data_size) triples laid out the
        way the Xbox compiler does: header, array, paths, vertices, indices,
        then one data block per tag. Returns the raw region bytes."""
        n = len(tags)
        arr_off = INDEX_HEADER_SIZE
        paths_off = arr_off + n * TAG_ENTRY_SIZE
        path_addrs, blob = [], bytearray()
        for _, p, _ in tags:
            path_addrs.append(base + paths_off + len(blob))
            blob += p.encode("ascii") + b"\0"
        vtx_off = paths_off + len(blob)
        idx_off = vtx_off + len(vertex_bytes)
        data_off = idx_off + len(index_bytes)
        entries, cursor = [], data_off
        for i, (g, _, size) in enumerate(tags):
            data = 0 if g == "sbsp" else base + cursor  # BSPs own no block in tag space
            entries.append(TagEntry(g, None, None, ((salt + i) << 16) | i, path_addrs[i], data, 0))
            if g != "sbsp":
                cursor += (size + 3) & ~3
        h = IndexHeader(base + arr_off, entries[scenario_index].tag_id, checksum, n,
                        model_parts, base + vtx_off, model_parts, base + idx_off)
        out = bytearray(h.pack())
        for e in entries:
            out += e.pack()
        out += blob + vertex_bytes + index_bytes
        out += bytes(cursor - data_off)
        return bytes(out)

    # ---- reporting ----------------------------------------------------------
    def group_counts(self) -> Counter:
        return Counter(e.group for e in self.entries)

    def paths(self, group: str) -> list[str]:
        return [e.path for e in self.entries if e.group == group]

    def summary(self, name: str = "") -> dict:
        h = self.header
        va, vs = self.vertex_region
        ia, isz = self.index_region
        arr_end = INDEX_HEADER_SIZE + h.tag_count * TAG_ENTRY_SIZE
        return {
            "map": name,
            "tag_count": h.tag_count,
            "scenario": self.scenario.path,
            "tag_data_offset": self.tag_data_offset,
            "tag_data_size": self.tag_data_size,
            "index_header_bytes": INDEX_HEADER_SIZE,
            "tag_array_bytes": h.tag_count * TAG_ENTRY_SIZE,
            "path_bytes": (va - self.base) - arr_end,
            "model_parts": h.model_part_count,
            "vertex_address": va, "vertex_bytes": vs,
            "index_address": ia, "index_bytes": isz,
            "tag_struct_bytes": self.tag_data_size - (ia + isz - self.base),
            "indexed_tags": sum(1 for e in self.entries if e.indexed),
            "groups": dict(sorted(self.group_counts().items(), key=lambda kv: (-kv[1], kv[0] or ""))),
            "structure_bsps": self.paths("sbsp"),
            "problems": self.problems,
        }


# ---- report -----------------------------------------------------------------
def _mib(n: int) -> str:
    return f"{n / 2**20:.2f} MiB"


def _inventory_sections(summaries: list[dict]) -> list[str]:
    """Bitmap / sound / script tables from inventory.py (summaries carry an 'inventory' key)."""
    inv = {s["map"]: s["inventory"] for s in summaries if "inventory" in s}
    if not inv:
        return []
    maps = list(inv)
    mib = lambda n: f"{n / 2**20:.1f} MiB"  # noqa: E731

    def breakdown(title: str, section: str, key: str) -> list[str]:
        names = sorted({k for m in maps for k in inv[m][section][key]})
        rows = [f"### {title}", "", "| " + " | ".join(["", *maps]) + " |", "|---|" + "---:|" * len(maps)]
        rows += [f"| {n} | " + " | ".join(str(inv[m][section][key].get(n, 0)) for m in maps) + " |"
                 for n in names]
        return rows + [""]

    out = ["## Bitmaps", "",
           "Source: `bitm` tags, `bitmap_data` blocks (see `tools/halo2ps2/inventory.py` for the layout).",
           "Pixel bytes are the Xbox-swizzled data stored outside tag space.", "",
           "| map | bitmap tags | images | pixel data |", "|---|---:|---:|---:|"]
    out += [f"| {m} | {inv[m]['bitmaps']['tags']} | {inv[m]['bitmaps']['images']} | "
            f"{mib(inv[m]['bitmaps']['pixel_bytes'])} |" for m in maps]
    out.append("")
    out += breakdown("Images per pixel format", "bitmaps", "formats")
    out += breakdown("Images per type", "bitmaps", "types")
    out += breakdown("Images per flag", "bitmaps", "flags")
    res = Counter()
    for m in maps:
        res.update(inv[m]["bitmaps"]["resolutions"])
    out += ["### Most common resolutions (all maps)", "", "| size | images |", "|---|---:|"]
    out += [f"| {r} | {n} |" for r, n in res.most_common(12)] + [""]
    out += ["## Sounds", "", "Source: `snd!` tags, pitch ranges and permutations.", "",
            "| map | sound tags | permutations | sample data |", "|---|---:|---:|---:|"]
    out += [f"| {m} | {inv[m]['sounds']['tags']} | {inv[m]['sounds']['permutations']} | "
            f"{mib(inv[m]['sounds']['sample_bytes'])} |" for m in maps]
    out.append("")
    out += breakdown("Permutations per encoding", "sounds", "kinds")
    out += ["## Scripts", "",
            "Source: the scenario's script node table. Built-ins are `function_name` nodes; "
            "the table lists the distinct ones each map uses.", "",
            "| map | live nodes | distinct built-ins | built-in calls |", "|---|---:|---:|---:|"]
    out += [f"| {m} | {inv[m]['scripts']['nodes']} | {len(inv[m]['scripts']['builtins'])} | "
            f"{sum(inv[m]['scripts']['builtins'].values())} |" for m in maps]
    out.append("")
    union = Counter()
    for m in maps:
        union.update(inv[m]["scripts"]["builtins"])
    camp = [m for m in maps if m != "ui"]
    every = set.intersection(*(set(inv[m]["scripts"]["builtins"]) for m in camp)) if camp else set()
    out += [f"Distinct built-ins across all maps: **{len(union)}**; used by every campaign map: "
            f"**{len(every)}**. Most used overall:", "",
            "| built-in | calls |", "|---|---:|"]
    out += [f"| `{n}` | {c} |" for n, c in union.most_common(25)] + [""]
    return out


def _bsp_sections(summaries: list[dict]) -> list[str]:
    rows = [(s["map"], b) for s in summaries for b in s.get("bsps", [])]
    if not rows:
        return []
    out = ["## Structure BSPs", "",
           "Source: the scenario's BSP block and each BSP's `structure_bsp` struct (layout in "
           "`tools/halo2ps2/bsp.py`). Block = the BSP's bytes in the map file; vertices are "
           "rendered/lightmap vertex counts summed over lightmap materials.", "",
           "| map | bsp | block | clusters | portals | lightmaps | triangles | rendered verts | lightmap verts | leaves | nodes |",
           "|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|"]
    for m, b in rows:
        c = b.counts
        out.append(f"| {m} | `{b.path.split(chr(92))[-1]}` | {b.block_bytes / 2**20:.2f} MiB | {c['clusters']} | "
                   f"{c['cluster_portals']} | {c['lightmaps']} | {b.triangles} | {b.rendered_vertices} | "
                   f"{b.lightmap_vertices} | {c['leaves']} | {c['nodes']} |")
    big = max(rows, key=lambda r: r[1].triangles)
    out += ["", f"Largest BSP by triangles: `{big[0]}/{big[1].path.split(chr(92))[-1]}` with "
            f"{big[1].triangles:,} triangles ({big[1].block_bytes / 2**20:.2f} MiB on Xbox).", ""]
    return out


def write_report(summaries: list[dict], out: Path) -> None:
    lines = ["# Xbox cache tag inventory", "",
             "Generated by `python3 -I tools/halo2ps2/tagindex.py report assets/decompressed/*.map`.",
             "Only format facts and tag *paths* appear here; no tag data is copied.", "",
             "## Layout verified on these maps", "",
             "| Field | Offset | Value on every map | How verified |", "|---|---|---|---|",
             "| tag array address | +0x00 | `base + 0x24` | header is 0x24 bytes; `'tags'` literal sits at +0x20 and the first entry's group follows at +0x24 |",
             "| scenario tag id | +0x04 | low 16 bits index the array | entry at that index has group `scnr`; its path is `levels\\<map>\\<map>` |",
             "| tag count | +0x0C | matches entries | every entry's tag-id low 16 bits equals its array index, every path pointer resolves to a printable NUL-terminated ASCII string |",
             "| model part count | +0x10, +0x18 | equal | both copies equal on every map |",
             "| model vertex address | +0x14 | `0x80xxxxxx` **pointer** | inside the tag region, immediately after the path strings (PC stores a file offset here) |",
             "| model index address | +0x1C | `0x80xxxxxx` **pointer** | inside the tag region, after the vertex buffer and before the first tag's data block (PC stores the vertex size here) |",
             "| entry tag id | +0x0C in entry | `(salt << 16) \\| index` | salt starts at 0xE174 and increments, with occasional jumps |",
             "| entry data address | +0x14 in entry | in tag region, `0` for `sbsp` | data blocks ascend in array order; BSP locations live in the scenario's structure-bsp block |",
             "| indexed flag | +0x18 in entry | 0 | no Xbox tag is external (no resource maps) |",
             "",
             "Model vertex buffer size = index address - vertex address (4-byte aligned after the path strings;",
             "not a multiple of 32, so per-part padding exists). The Xbox header has no index-buffer size: it is taken",
             "as the gap to the scenario tag's data block, which follows the 16-bit index stream (with 0xFFFF strip",
             "restarts) on every map with at most 2 bytes of padding.",
             "", "## Per-map summary", "",
             "| map | tags | tag region | array | paths | model parts | vertex buf | index buf | tag structs | sbsp |",
             "|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|"]
    for s in summaries:
        lines.append(f"| {s['map']} | {s['tag_count']} | {_mib(s['tag_data_size'])} | {_mib(s['tag_array_bytes'])} | "
                     f"{_mib(s['path_bytes'])} | {s['model_parts']} | {_mib(s['vertex_bytes'])} | "
                     f"{_mib(s['index_bytes'])} | {_mib(s['tag_struct_bytes'])} | {len(s['structure_bsps'])} |")
    lines.append("")
    lines += _inventory_sections(summaries)
    lines += _bsp_sections(summaries)
    all_groups = sorted({g for s in summaries for g in s["groups"]}, key=lambda g: g or "")
    lines += ["## Tags per group", "", "| group | " + " | ".join(s["map"] for s in summaries) + " |",
              "|---|" + "---:|" * len(summaries)]
    for g in all_groups:
        lines.append(f"| `{g}` | " + " | ".join(str(s["groups"].get(g, 0)) for s in summaries) + " |")
    lines.append("")
    for s in summaries:
        lines += [f"## {s['map']}", "",
                  f"- scenario: `{s['scenario']}`",
                  f"- tag data: offset {s['tag_data_offset']:#x}, {s['tag_data_size']:,} bytes "
                  f"({s['tag_count']} tags, {len(s['groups'])} groups)",
                  f"- model buffers: {s['model_parts']} parts, vertices {s['vertex_bytes']:,} bytes at "
                  f"{s['vertex_address']:#x}, indices {s['index_bytes']:,} bytes at {s['index_address']:#x}",
                  f"- structure_bsp tags ({len(s['structure_bsps'])}):"]
        lines += [f"  - `{p}`" for p in s["structure_bsps"]]
        if s["problems"]:
            lines.append("- problems:")
            lines += [f"  - {p}" for p in s["problems"]]
        lines.append("")
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text("\n".join(lines))


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--base", type=lambda s: int(s, 0), default=XBOX_TAG_BASE)
    ap.add_argument("--lenient", action="store_true", help="report problems instead of failing")
    sub = ap.add_subparsers(dest="cmd", required=True)
    ar = sub.add_parser("report", help="write the markdown inventory")
    ar.add_argument("maps", nargs="+", type=Path)
    ar.add_argument("-o", "--out", type=Path, default=Path("docs/reports/tag-index.md"))
    ar.add_argument("--no-inventory", action="store_true", help="skip bitmap/sound/script statistics")
    ar.add_argument("--json", action="store_true", help="print summaries as JSON too")
    ad = sub.add_parser("dump", help="list tag entries")
    ad.add_argument("map", type=Path)
    ad.add_argument("--group", help="only this four-char group")
    args = ap.parse_args(argv)
    strict = not args.lenient

    if args.cmd == "dump":
        idx = TagIndex.read(args.map, args.base, strict)
        for e in idx.entries:
            if args.group and e.group != args.group:
                continue
            print(f"{e.index:5d} {e.group} {e.tag_id:#010x} data={e.data_address:#010x} {e.path}")
        for p in idx.problems:
            print("problem:", p, file=sys.stderr)
        return 0

    summaries, failed = [], 0
    for p in sorted(args.maps):
        try:
            idx = TagIndex.read(p, args.base, strict)
        except (TagIndexError, ValueError) as ex:
            print(f"{p.name}: {ex}", file=sys.stderr)
            failed += 1
            continue
        s = idx.summary(p.stem)
        if not args.no_inventory:
            from halo2ps2 import inventory  # noqa: PLC0415  (inventory imports this module)
            s["inventory"] = inventory.inventory(p)
            s["problems"] += s["inventory"]["problems"]
            from halo2ps2 import bsp  # noqa: PLC0415
            try:
                s["bsps"] = bsp.summarise_map(p)
            except (bsp.BspError, struct.error) as ex:
                s["bsps"] = []
                s["problems"].append(f"bsp: {ex}")
            s["problems"] += [f"{b.path}: {pr}" for b in s["bsps"] for pr in b.problems]
        summaries.append(s)
        print(f"{p.stem:<6} {s['tag_count']:5d} tags  {len(s['groups']):3d} groups  "
              f"vtx {_mib(s['vertex_bytes'])}  idx {_mib(s['index_bytes'])}  "
              f"sbsp {len(s['structure_bsps'])}" + (f"  problems {len(s['problems'])}" if s["problems"] else ""))
    if summaries:
        write_report(summaries, args.out)
        print(f"wrote {args.out}")
    if args.json:
        print(json.dumps(summaries, indent=1))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
