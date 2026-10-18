#!/usr/bin/env python3
"""The scenario's sky: model tag and fog/ambient parameters -> a small `SKY0` section (docs/13-scen-section.md §Sky).

Verified on a10 and a30 (the scenario's `skies` block at +0x30 lists `sky` tags; the first one is the one the
engine draws): sky tag layout (offsets in the tag's data block, all f32 unless noted):
    +0x00  tag reference to the sky `model` (group, path pointer, path length, tag id at +0x0C)
    +0x10  tag reference to its animation graph (not used)
    +0x48  indoor ambient radiosity rgb, +0x54 its power;   +0x58 outdoor ambient rgb
    +0x78  outdoor fog colour rgb, +0x8C maximum density, +0x90 starting distance, +0x94 opaque distance (world units)
Unverified: which of the two a30 skies is active at a given time (the scenario's script switches them; the first is used),
the animation of the sky model.

Section `SKY0`, 64 bytes, little-endian:
    char[4] 'SKYS', u16 version (1), u16 reserved, u32 model tag id (0 = none), f32 fog rgb[3] (0..1),
    f32 fog near, f32 fog far, f32 fog max density, f32 outdoor ambient rgb[3], f32 indoor ambient rgb[3],
    u32 model tag id of the scenario's SECOND sky (0 = none; a10: `sky_x10`, the nebula sky that the exterior BSP's cluster selects
    with its sky index 1) = 64 bytes

A cluster's `sky` field indexes the scenario's sky palette; the engine draws palette entry 0 or 1 by the cluster the camera is in
(a10: BSP 7, the intro's exterior, has one cluster with sky 1; every window onto space in the other BSPs is sky 0).
"""
from __future__ import annotations

import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from halo2ps2 import mapinfo, tagindex  # noqa: E402

SCENARIO_SKIES = 0x30
SKY_MODEL, SKY_INDOOR_AMBIENT, SKY_OUTDOOR_AMBIENT = 0x00, 0x48, 0x58
SKY_FOG_RGB, SKY_FOG_DENSITY, SKY_FOG_NEAR, SKY_FOG_FAR = 0x78, 0x8C, 0x90, 0x94
SECTION_SIZE = 64


def read_sky(map_path: Path) -> dict | None:
    hdr = mapinfo.read_header(map_path)
    idx = tagindex.TagIndex.read(map_path)
    with map_path.open("rb") as f:
        f.seek(hdr.tag_data_offset)
        region = f.read(hdr.tag_data_size)
    base = idx.base
    by_id = {e.tag_id: e for e in idx.entries}
    sc = idx.scenario.data_address - base
    count, addr = struct.unpack_from("<II", region, sc + SCENARIO_SKIES)
    if not 0 < count <= 8 or not 0 <= addr - base < len(region):
        return None
    ref = region[addr - base:addr - base + 16]
    entry = by_id.get(struct.unpack_from("<I", ref, 12)[0])
    if entry is None or entry.group != "sky ":
        return None
    model_tag2 = 0
    if count > 1 and addr - base + 32 <= len(region):
        e2 = by_id.get(struct.unpack_from("<I", region, addr - base + 16 + 12)[0])
        if e2 is not None and e2.group == "sky ":
            m2 = by_id.get(struct.unpack_from("<I", region, e2.data_address - base + SKY_MODEL + 12)[0])
            model_tag2 = m2.tag_id if m2 is not None and m2.group == "mode" else 0
    d = entry.data_address - base
    model_id = struct.unpack_from("<I", region, d + SKY_MODEL + 12)[0]
    model = by_id.get(model_id)
    f32 = lambda off, n: struct.unpack_from(f"<{n}f", region, d + off)  # noqa: E731
    return {
        "path": entry.path,
        "model_tag": model_id if model is not None and model.group == "mode" else 0, "model_tag2": model_tag2,
        "fog_rgb": f32(SKY_FOG_RGB, 3), "fog_density": f32(SKY_FOG_DENSITY, 1)[0],
        "fog_near": f32(SKY_FOG_NEAR, 1)[0], "fog_far": f32(SKY_FOG_FAR, 1)[0],
        "outdoor": f32(SKY_OUTDOOR_AMBIENT, 3), "indoor": f32(SKY_INDOOR_AMBIENT, 3),
    }


def build_section(map_path: Path) -> bytes:
    sky = read_sky(map_path)
    if sky is None:
        return struct.pack("<4sHHI", b"SKYS", 1, 0, 0).ljust(SECTION_SIZE, b"\0")
    clamp = lambda v: max(0.0, min(1.0, v)) if v == v else 0.0  # noqa: E731
    body = struct.pack("<4sHHI3f3f3f3fI", b"SKYS", 1, 0, sky["model_tag"],
                       *[clamp(c) for c in sky["fog_rgb"]], sky["fog_near"], sky["fog_far"], sky["fog_density"],
                       *[clamp(c) for c in sky["outdoor"]], *[clamp(c) for c in sky["indoor"]], sky["model_tag2"])
    # 4+2+2+4 + 12 (fog rgb) + 12 (near, far, density) + 12 + 12 + 4 (second sky model) = 64 bytes
    return body.ljust(SECTION_SIZE, b"\0")


def main(argv: list[str] | None = None) -> int:
    path = Path((argv or sys.argv[1:])[0])
    sky = read_sky(path)
    print(sky if sky else "no sky")
    return 0


if __name__ == "__main__":
    sys.exit(main())
