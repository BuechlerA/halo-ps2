"""SCEN section on a synthetic map (no game data needed): palettes and placements, definitions, the idle
animation rule, player data, BSP membership tiers, lighting probes, the section writer and its reference
parser, corruption handling. Set SCEN_FIXTURE=<path> to also write the .hp2 the C reader test
(engine/tests/test_scen.c, which hard-codes the numbers of this world) opens. A skip-if-missing check
converts the real a10 map when assets/decompressed/a10.map exists."""
import math
import os
import struct
import sys
from pathlib import Path

sys.path[:0] = [str(Path(__file__).resolve().parents[1]), str(Path(__file__).resolve().parent)]
from halo2ps2 import bsp, bspconv, hp2, scenconv as sc, tagindex, texconv  # noqa: E402
import test_bsp as world  # noqa: E402

BASE = tagindex.XBOX_TAG_BASE
ROOT = Path(__file__).resolve().parents[2]
NOANIM = sc.NO_ANIM


# ---- a synthetic map region ------------------------------------------------------------------------
class Region:
    """Tag region builder: tags with zero-filled data blocks, then pokes (offsets are region offsets)."""

    def __init__(self, tags: list[tuple[str, str, int]]):
        self.region = bytearray(tagindex.TagIndex.build(tags, scenario_index=0))
        self.index = tagindex.TagIndex.parse_region(bytes(self.region), 0x1000)
        self.by_path = {e.path: e for e in self.index.entries}
        self.cursor = {}

    def block(self, path: str) -> int:
        return self.by_path[path].data_address - BASE

    def tid(self, path: str) -> int:
        return self.by_path[path].tag_id

    def alloc(self, owner, data: bytes) -> int:
        """Append data into the tail of a tag's block (``owner``: tag path (unique ones) or TagEntry), 4-aligned;
        returns its tag-space address."""
        e = self.by_path[owner] if isinstance(owner, str) else owner
        key = (e.group, e.path)
        cur = (self.cursor.get(key, 0x1000) + 3) & ~3
        off = e.data_address - BASE + cur
        self.region[off:off + len(data)] = data
        self.cursor[key] = cur + len(data)
        return BASE + off

    alloc_at = alloc

    def poke(self, off: int, fmt: str, *vals) -> None:
        struct.pack_into(fmt, self.region, off, *vals)

    def refl(self, off: int, count: int, addr: int) -> None:
        struct.pack_into("<III", self.region, off, count, addr, 0)

    def ref(self, group: str, path: str | None) -> bytes:
        if path is None:
            return struct.pack("<4sIII", group[::-1].encode(), 0, 0, 0xFFFFFFFF)
        return struct.pack("<4sIII", group[::-1].encode(), 0, 0, self.tid(path))

    def map(self) -> texconv.MapData:
        idx = tagindex.TagIndex.parse_region(bytes(self.region), 0x1000)
        return texconv.MapData(Path("synthetic.map"), None, idx, bytes(self.region),  # type: ignore[arg-type]
                               {e.tag_id: e for e in idx.entries})


OBJ = {"scen": 6, "bipd": 0, "vehi": 1, "weap": 2, "mach": 7}
TAGS = [
    ("scnr", "levels\\t\\t", 0x6000), ("matg", "globals\\globals", 0x2000),
    ("scen", "scenery\\crate\\crate", 0x2000), ("bipd", "characters\\cyborg\\cyborg", 0x2000),
    ("bipd", "characters\\marine\\marine", 0x2000), ("vehi", "levels\\t\\chair", 0x2000),
    ("weap", "weapons\\pistol\\pistol", 0x2000), ("mach", "levels\\t\\door", 0x2000),
    ("ssce", "sound\\hum", 0x2000),
    ("mode", "scenery\\crate\\crate", 0x2000), ("mode", "characters\\marine\\marine", 0x2000),
    ("mode", "weapons\\pistol\\pistol", 0x2000), ("mode", "characters\\cyborg\\fp\\fp", 0x2000),
    ("mode", "weapons\\pistol\\fp\\fp", 0x2000),
    ("antr", "scenery\\crate\\crate", 0x2000), ("antr", "characters\\marine\\marine", 0x2000),
    ("antr", "levels\\t\\chair", 0x2000), ("antr", "levels\\t\\door", 0x2000),
    ("antr", "weapons\\pistol\\fp\\fp", 0x2000), ("sky ", "sky\\t\\t", 0x2000),
]


def entry(r: Region, group: str, path: str) -> tagindex.TagEntry:
    return next(e for e in r.index.entries if e.group == group and e.path == path)


def write_graph(r: Region, e: tagindex.TagEntry, anims: list[str], units=(), devices=False, fp_idle=None,
                nodes=()) -> None:
    """units: (label, [(weapon name, [animation indices])...]); nodes: (name, parent)."""
    b = e.data_address - BASE
    arr = lambda data: r.alloc_at(e, data)  # noqa: E731
    an = b"".join(struct.pack("<32sHHHH", n.encode(), 0, 10, 0, 0).ljust(sc.ANIM_REC, b"\0") for n in anims)
    r.refl(b + sc.ANIM_ANIMATIONS, len(anims), arr(an))
    ub = bytearray()
    for label, weapons in units:
        wb = bytearray()
        for wname, idx in weapons:
            w = bytearray(sc.UNIT_WEAPON_REC)
            w[:len(wname)] = wname.encode()
            struct.pack_into("<III", w, 0x98, len(idx), arr(struct.pack(f"<{len(idx)}H", *idx)) if idx else 0, 0)
            wb += w
        u = bytearray(sc.UNIT_REC)
        u[:len(label)] = label.encode()
        struct.pack_into("<III", u, 0x58, len(weapons), arr(bytes(wb)) if weapons else 0, 0)
        ub += u
    if units:
        r.refl(b + sc.ANIM_UNITS, len(units), arr(bytes(ub)))
    if devices:
        r.refl(b + 0x30, 1, arr(bytes(0x80)))
    if fp_idle is not None:
        el = bytearray(0x1C)
        struct.pack_into("<III", el, 0x10, len(fp_idle), arr(struct.pack(f"<{len(fp_idle)}H", *fp_idle)), 0)
        r.refl(b + sc.ANIM_FP_WEAPONS, 1, arr(bytes(el)))
    if nodes:
        nb = b"".join(struct.pack("<32shhh", n.encode(), -1, -1, p).ljust(sc.NODE_REC, b"\0") for n, p in nodes)
        r.refl(b + sc.ANIM_NODES, len(nodes), arr(nb))


def write_model(r: Region, e: tagindex.TagEntry, nodes: list[str]) -> None:
    b = e.data_address - BASE
    nb = b"".join(n.encode().ljust(32, b"\0") + bytes(156 - 32) for n in nodes)
    r.refl(b + 0xB8, len(nodes), r.alloc_at(e, nb))


def write_object(r: Region, e: tagindex.TagEntry, otype: int, radius: float, off, model, anim) -> None:
    b = e.data_address - BASE
    r.poke(b, "<H", otype)
    r.poke(b + 4, "<4f", radius, *off)
    null = struct.pack("<4sIII", b"edom", 0, 0, 0xFFFFFFFF)         # null references carry tag id 0xFFFFFFFF
    r.region[b + sc.OBJ_MODEL:b + sc.OBJ_MODEL + 16] = null
    r.region[b + sc.OBJ_ANIM:b + sc.OBJ_ANIM + 16] = null.replace(b"edom", b"rtna")
    if model:
        r.region[b + sc.OBJ_MODEL:b + sc.OBJ_MODEL + 16] = struct.pack("<4sIII", b"edom", 0, 0, model.tag_id)
    if anim:
        r.region[b + sc.OBJ_ANIM:b + sc.OBJ_ANIM + 16] = struct.pack("<4sIII", b"rtna", 0, 0, anim.tag_id)


def build_synthetic() -> tuple[Region, texconv.MapData]:
    r = Region(TAGS)
    E = lambda g, p: entry(r, g, p)  # noqa: E731
    crate_m, marine_m = E("mode", "scenery\\crate\\crate"), E("mode", "characters\\marine\\marine")
    pistol_m, arms_m = E("mode", "weapons\\pistol\\pistol"), E("mode", "characters\\cyborg\\fp\\fp")
    pfp_m = E("mode", "weapons\\pistol\\fp\\fp")
    crate_a, marine_a = E("antr", "scenery\\crate\\crate"), E("antr", "characters\\marine\\marine")
    chair_a, door_a = E("antr", "levels\\t\\chair"), E("antr", "levels\\t\\door")
    fp_a = E("antr", "weapons\\pistol\\fp\\fp")

    # models: the hands have 3 nodes, the pistol's first person model 2
    write_model(r, arms_m, ["frame bone24", "frame l arm", "frame r wriste"])
    write_model(r, pfp_m, ["frame gun", "frame mag"])
    # object tags: radius + bounding offset, model, graph
    write_object(r, E("scen", "scenery\\crate\\crate"), 6, 1.0, (0.5, 0, 0), crate_m, crate_a)
    write_object(r, E("bipd", "characters\\cyborg\\cyborg"), 0, 0.4, (0, 0, 0), marine_m, marine_a)
    write_object(r, E("bipd", "characters\\marine\\marine"), 0, 0.4, (0, 0, 0), marine_m, marine_a)
    write_object(r, E("vehi", "levels\\t\\chair"), 1, 0.5, (0, 0, 0), None, chair_a)
    write_object(r, E("weap", "weapons\\pistol\\pistol"), 2, 0.1, (0, 0, 0), pistol_m, None)
    write_object(r, E("mach", "levels\\t\\door"), 7, 0.9, (0, 0, 0), None, door_a)
    write_object(r, E("ssce", "sound\\hum"), 11, 0.0, (0, 0, 0), None, None)
    wb = E("weap", "weapons\\pistol\\pistol").data_address - BASE
    r.region[wb + sc.WEAPON_FP_MODEL:wb + sc.WEAPON_FP_MODEL + 16] = struct.pack("<4sIII", b"edom", 0, 0, pfp_m.tag_id)
    r.region[wb + sc.WEAPON_FP_ANIM:wb + sc.WEAPON_FP_ANIM + 16] = struct.pack("<4sIII", b"rtna", 0, 0, fp_a.tag_id)
    # graphs
    write_graph(r, crate_a, ["wobble", "stand fixed idle"], units=[("stand", [("fixed", [1])])])
    write_graph(r, marine_a, ["crouch rifle idle", "stand rifle idle", "stand unarmed idle", "stand opening"],
                units=[("crouch", [("rifle", [0])]), ("stand", [("rifle", [1]), ("unarmed", [2])])])
    write_graph(r, chair_a, ["stand opening"], units=[("stand", [("fixed", [0])])], devices=True)
    write_graph(r, door_a, ["device position"], devices=True)
    write_graph(r, fp_a, ["first-person idle", "first-person fire-1"], fp_idle=[0, 1],
                nodes=[("frame bone24", -1), ("frame l arm", 0), ("frame r wriste", 0), ("frame gun", 2), ("frame mag", 3)])

    # globals: player biped and first person hands
    gb = r.block("globals\\globals")
    el = r.alloc("globals\\globals", struct.pack("<4sIII", b"dpib", 0, 0, r.tid("characters\\cyborg\\cyborg")))
    r.refl(gb + 0x170, 1, el)
    el = r.alloc("globals\\globals", struct.pack("<4sIII", b"edom", 0, 0, arms_m.tag_id))
    r.refl(gb + 0x17C, 1, el)

    # scenario
    sb = r.block("levels\\t\\t")
    S = "levels\\t\\t"

    def palette(group: str, off: int, paths: list[str]) -> None:
        data = b"".join(struct.pack("<4sIII", group[::-1].encode(), 0, 0, entry(r, group, p).tag_id).ljust(48, b"\0")
                        for p in paths)
        r.refl(sb + off, len(paths), r.alloc(S, data))

    def placements(off: int, stride: int, rows: list[tuple]) -> None:
        data = bytearray()
        for ptype, name, flags, pos, rot, manual in rows:
            e = bytearray(stride)
            struct.pack_into("<HHHH3f3f", e, 0, ptype, name, flags, 0, *pos, *rot)
            if manual:
                struct.pack_into("<H", e, 0x20, manual)
            data += e
        r.refl(sb + off, len(rows), r.alloc(S, bytes(data)))

    names = b"".join(n.encode().ljust(32, b"\0") + struct.pack("<HH", 6, i) for i, n in enumerate(["crate_a", "crate_b"]))
    r.refl(sb + sc.SCN_NAMES, 2, r.alloc(S, names))
    palette("scen", 0x21C, ["scenery\\crate\\crate"])
    placements(0x210, 72, [
        (0, 0, 0, (-5.0, 100.0, 0.5), (0.5, 0.0, 0.0), 0),            # BSP 0 exact, placed automatically
        (0, 0xFFFF, 1, (5.0, 101.0, 0.0), (0.0, 0.0, 0.0), 0),        # BSP 0, not placed automatically
        (0, 1, 0x0A, (25.0, 100.0, 0.0), (1.0, 0.25, -0.25), 0),      # BSP 1 exact, difficulty bits 1 and 3
        (0xFFFF, 0xFFFF, 0, (0.0, 0.0, 0.0), (0.0, 0.0, 0.0), 0),     # deleted palette entry: unresolved
        (0, 0xFFFF, 0, (15.0, 100.0, 0.0), (0.0, 0.0, 0.0), 2),       # nowhere, manual flags say BSP 1
    ])
    palette("bipd", 0x234, ["characters\\marine\\marine"])
    placements(0x228, 120, [(0, 0xFFFF, 1, (10.05, 100.0, 0.0), (1.0, 0.0, 0.0), 0)])    # probe tier
    palette("vehi", 0x24C, ["levels\\t\\chair"])
    placements(0x240, 120, [(0, 0xFFFF, 0, (10.8, 100.0, 0.0), (0.0, 0.0, 0.0), 0)])     # bounds tier
    palette("weap", 0x27C, ["weapons\\pistol\\pistol"])
    placements(0x270, 92, [(0, 0xFFFF, 0, (40.0, 100.0, 0.0), (0.0, 0.0, 0.0), 0)])      # in no BSP
    palette("mach", 0x2A0, ["levels\\t\\door"])
    placements(0x294, 64, [(0, 0xFFFF, 0, (-3.0, 100.0, 0.0), (3.14159, 0.0, 0.0), 0)])
    palette("ssce", 0x2E8, ["sound\\hum"])
    placements(0x2DC, 40, [(0, 0xFFFF, 0, (0.0, 0.0, 0.0), (0.0, 0.0, 0.0), 0)] * 2)
    # sky palette: one sky with two lights (the brighter one, index 1, is the sun)
    sky = entry(r, "sky ", "sky\\t\\t")
    r.refl(sb + sc.SCN_SKIES, 1, r.alloc(S, struct.pack("<4sIII", b" yks", 0, 0, sky.tag_id)))
    lights = bytearray()
    for col, power, yaw, pitch in (((0.2, 0.2, 0.2), 1.0, 0.0, 1.5), ((1.0, 1.0, 1.0), 2.0, 0.0, 0.0)):
        el = bytearray(sc.SKY_LIGHT_REC)
        struct.pack_into("<3f", el, 0x50, *col)
        struct.pack_into("<f", el, 0x5C, power)
        struct.pack_into("<2f", el, 0x68, yaw, pitch)
        lights += el
    r.refl(sky.data_address - BASE + sc.SKY_LIGHTS, 2, r.alloc_at(sky, bytes(lights)))
    # player starting profiles: unarmed, then the pistol
    prof = bytearray(2 * sc.PROFILE_REC)
    prof[sc.PROFILE_REC + 0x28:sc.PROFILE_REC + 0x38] = struct.pack("<4sIII", b"paew", 0, 0, entry(r, "weap", "weapons\\pistol\\pistol").tag_id)
    prof[0x28:0x38] = struct.pack("<4sIII", b"paew", 0, 0, 0xFFFFFFFF)
    prof[0x3C:0x4C] = struct.pack("<4sIII", b"paew", 0, 0, 0xFFFFFFFF)
    prof[sc.PROFILE_REC + 0x3C:sc.PROFILE_REC + 0x4C] = struct.pack("<4sIII", b"paew", 0, 0, 0xFFFFFFFF)
    r.refl(sb + sc.SCN_PROFILES, 2, r.alloc(S, bytes(prof)))
    return r, r.map()


def synthetic_probes() -> list[sc.BspProbe]:
    block, loc = world.build_world()
    st0 = bsp.read_structure(loc, block)
    # BSP 1: x in [20, 30) is one leaf (cluster 0)
    st1 = bsp.Structure(0, [(1.0, 0.0, 0.0, 20.0), (1.0, 0.0, 0.0, 30.0)], [(0, 1, bsp.NO_CHILD), (1, bsp.NO_CHILD, bsp.LEAF_FLAG | 0)],
                        [0], [()], [bsp.Cluster(-1, -1, -1, -1, -1, 0, [], set())], [], 0, {})
    return [sc.BspProbe(0, "t0", st0, (-11.0, 11.0, 90.0, 120.0, -1.0, 1.0)),
            sc.BspProbe(1, "t1", st1, (19.0, 31.0, 90.0, 120.0, -1.0, 1.0))]


def expect(fn, exc=sc.ScenError) -> None:
    try:
        fn()
    except exc:
        return
    raise SystemExit("corrupt input accepted")


# ---- tests --------------------------------------------------------------------------------------
def check_collect() -> sc.ObjectSet:
    r, m = build_synthetic()
    objs = sc.collect_objects(m, synthetic_probes())
    assert objs.problems == [], objs.problems
    assert objs.counts == {"scenery": 5, "biped": 1, "vehicle": 1, "equipment": 0, "weapon": 1, "machine": 1,
                           "control": 0, "light_fixture": 0, "sound_scenery": 2}
    assert objs.unresolved == 1 and len(objs.placements) == 8
    # definitions sorted by tag id: crate, cyborg (player biped, never placed), marine, chair, pistol, door
    assert [d.name for d in objs.defs] == ["crate", "cyborg", "marine", "chair", "pistol", "door"]
    assert [d.tag_id for d in objs.defs] == sorted(d.tag_id for d in objs.defs)
    assert [sc.TYPE_NAMES[d.type] for d in objs.defs] == ["scenery", "biped", "biped", "vehicle", "weapon", "machine"]
    crate, cyborg, marine, chair, pistol, door = objs.defs
    assert abs(crate.radius - 1.5) < 1e-6 and abs(door.radius - 0.9) < 1e-6        # radius + |bounding offset|
    # idle rule: scenery with a unit-style idle, biped: stand/unarmed first, chair: only "stand opening" -> static
    assert (crate.idle_anim, marine.idle_anim, cyborg.idle_anim) == (1, 2, 2)
    assert chair.idle_anim == NOANIM and door.idle_anim == NOANIM and pistol.idle_anim == NOANIM
    assert chair.anim_tag and door.anim_tag and not pistol.anim_tag and not chair.model_tag and crate.model_tag
    # placements in scenario order: scenery x4 (one unresolved dropped), biped, vehicle, weapon, machine
    P = objs.placements
    assert [p.kind for p in P] == ["scenery"] * 4 + ["biped", "vehicle", "weapon", "machine"]
    assert [p.def_index for p in P] == [0, 0, 0, 0, 2, 3, 4, 5]
    assert [p.flags for p in P] == [1, 0, 0x0B, 1, 0, 1, 1, 1]                   # bit 0 = placed automatically
    assert [p.name_index for p in P[:3]] == [0, -1, 1] and P[1].name_index == -1
    assert P[0].pos == (-5.0, 100.0, 0.5) and P[2].rot == (1.0, 0.25, -0.25)
    assert [p.bsp_mask for p in P] == [1, 1, 2, 2, 1, 1, 0, 1]
    assert [p.mask_tier for p in P] == ["exact", "exact", "exact", "flags", "probe", "bounds", "none", "exact"]
    assert objs.bsp_counts == [5, 2] and objs.bsp_count == 2
    # player
    pl = objs.player
    assert pl.notes == [] and pl.biped_def == 2 and pl.weapon_def == 5            # def index + 1
    assert pl.fp_arms_model_tag == entry(r, "mode", "characters\\cyborg\\fp\\fp").tag_id
    fp_a = entry(r, "antr", "weapons\\pistol\\fp\\fp").tag_id
    assert pl.fp_arms_anim_tag == pl.weapon_fp_anim_tag == fp_a
    assert pl.weapon_fp_model_tag == entry(r, "mode", "weapons\\pistol\\fp\\fp").tag_id
    assert pl.arms_idle_anim == pl.weapon_idle_anim == 0 and pl.weapon_hand_marker == "frame r wriste"
    assert pl.fp_offset == (0.0, 0.0, 0.0)
    # tag sets: models of the used defs + player; graphs likewise, sorted, no duplicates
    want_models = {entry(r, "mode", p).tag_id for p in ("scenery\\crate\\crate", "characters\\marine\\marine",
                                                          "weapons\\pistol\\pistol", "characters\\cyborg\\fp\\fp",
                                                          "weapons\\pistol\\fp\\fp")}
    assert objs.model_tags == sorted(want_models)
    assert objs.anim_tags == sorted({entry(r, "antr", p).tag_id for p in (
        "scenery\\crate\\crate", "characters\\marine\\marine", "levels\\t\\chair", "levels\\t\\door",
        "weapons\\pistol\\fp\\fp")})
    # without BSPs nothing is located (collect() on an empty probe list)
    assert all(p.bsp_mask == 0 for p in sc.collect_objects(m, []).placements)
    return objs


def check_locate() -> None:
    probes = synthetic_probes()
    f = sc.locate_mask
    assert f(probes, (-5.0, 100.0, 0.0)) == (1, "exact") and f(probes, (25.0, 100.0, 0.0)) == (2, "exact")
    assert f(probes, (15.0, 100.0, 0.0)) == (0, "none") and f(probes, (15.0, 100.0, 0.0), 1) == (1, "flags")
    assert f(probes, (15.0, 100.0, 0.0), 0x80) == (0, "none")            # a flag for a BSP the map does not have
    assert f(probes, (25.0, 100.0, 0.0), 3) == (3, "exact")              # both: exact wins the label, flags add bits
    assert f(probes, (10.0, 100.0, 0.0)) == (1, "probe")                 # the plane itself belongs to the outside
    assert f(probes, (-10.05, 100.0, 0.0)) == (1, "probe")
    assert f(probes, (10.4, 100.0, 0.0)) == (1, "probe")                 # 0.5 wu is still inside the tolerance
    assert f(probes, (10.6, 100.0, 0.0)) == (1, "bounds")                # beyond it, the bounding box decides
    assert f(probes, (19.5, 100.0, 0.0)) == (2, "probe")                 # the probe finds BSP 1, 0.5 away
    assert f(probes, (60.0, 100.0, 0.0)) == (0, "none")
    assert f([], (0.0, 0.0, 0.0)) == (0, "none")


def check_idle_rule() -> None:
    r, m = build_synthetic()
    R = sc.Region(m)
    marine = entry(r, "antr", "characters\\marine\\marine")
    assert sc.idle_animation(R, marine.data_address - BASE) == 2
    # a graph without units, with an out-of-range or non-idle entry: static
    e = entry(r, "antr", "levels\\t\\door")
    assert sc.idle_animation(R, e.data_address - BASE) == NOANIM
    # corrupt the weapon animation list entry: out of range, still static
    base = marine.data_address - BASE
    n, uo = R.reflexive(base + sc.ANIM_UNITS, sc.UNIT_REC)
    wn, wo = R.reflexive(uo + sc.UNIT_REC + 0x58, sc.UNIT_WEAPON_REC)         # unit 'stand'
    ln, lo = R.reflexive(wo + sc.UNIT_WEAPON_REC + 0x98, 2)                    # weapon 'unarmed'
    r.poke(lo, "<H", 500)
    m2 = r.map()
    assert sc.idle_animation(sc.Region(m2), base) == 1                         # falls through to 'rifle'
    expect(lambda: sc.Region(m2).reflexive(base + sc.ANIM_UNITS, 1 << 30))
    r.poke(base + sc.ANIM_UNITS, "<II", 3, 0x10)                               # pointer outside the region
    expect(lambda: sc.idle_animation(sc.Region(r.map()), base))


def check_player_variants() -> None:
    r, m = build_synthetic()
    sb = r.block("levels\\t\\t")
    # no weapon anywhere: player data without weapon, a note, and nothing crashes
    n, po = sc.Region(m).reflexive(sb + sc.SCN_PROFILES, sc.PROFILE_REC)
    r.region[po + sc.PROFILE_REC + 0x28:po + sc.PROFILE_REC + 0x38] = struct.pack("<4sIII", b"paew", 0, 0, 0xFFFFFFFF)
    objs = sc.collect_objects(r.map(), synthetic_probes())
    pl = objs.player
    assert pl.weapon_def == 0 and pl.weapon_fp_anim_tag == 0 and pl.notes and pl.biped_def == 2
    assert pl.arms_idle_anim == NOANIM and pl.fp_arms_model_tag
    # a secondary weapon is used when no profile has a primary
    r2, _ = build_synthetic()
    sb2 = r2.block("levels\\t\\t")
    n, po2 = sc.Region(r2.map()).reflexive(sb2 + sc.SCN_PROFILES, sc.PROFILE_REC)
    pistol = entry(r2, "weap", "weapons\\pistol\\pistol").tag_id
    r2.region[po2 + sc.PROFILE_REC + 0x28:po2 + sc.PROFILE_REC + 0x38] = struct.pack("<4sIII", b"paew", 0, 0, 0xFFFFFFFF)
    r2.region[po2 + 0x3C:po2 + 0x4C] = struct.pack("<4sIII", b"paew", 0, 0, pistol)
    assert sc.collect_objects(r2.map(), []).player.weapon_def == 5


def check_corrupt_map() -> None:
    r, m = build_synthetic()
    sb = r.block("levels\\t\\t")
    for off, why in ((0x210, "scenery"), (0x228, "biped"), (0x204, "names"), (0x21C, "palette")):
        r2, _ = build_synthetic()
        r2.poke(sb + off, "<II", 5, 0x80000000 + 0x7FFF0000)
        expect(lambda: sc.collect_objects(r2.map(), []))
    r3, _ = build_synthetic()
    r3.poke(sb + 0x214, "<I", 1)            # scenery pointer below the region base
    expect(lambda: sc.collect_objects(r3.map(), []))
    r4, _ = build_synthetic()
    r4.poke(sb + 0x21C + 4, "<I", BASE + 0x7FFFFFF)
    expect(lambda: sc.collect_objects(r4.map(), []))
    # NaN position: the placement is dropped, not carried
    r5, _ = build_synthetic()
    n, o = sc.Region(r5.map()).reflexive(sb + 0x210, 72)
    r5.poke(o + 8, "<I", 0x7FC00000)
    objs = sc.collect_objects(r5.map(), [])
    assert objs.unresolved == 2 and len(objs.placements) == 7
    # an object tag of the wrong group in a palette is dropped, with a problem note
    r6, _ = build_synthetic()
    n, po = sc.Region(r6.map()).reflexive(sb + 0x21C, 48)
    r6.region[po:po + 16] = struct.pack("<4sIII", b"necs", 0, 0, entry(r6, "mach", "levels\\t\\door").tag_id)
    objs = sc.collect_objects(r6.map(), [])
    assert objs.problems and all(p.kind != "scenery" for p in objs.placements)


def check_serialisation(objs: sc.ObjectSet) -> bytes:
    lights = [[sc.ClusterLight((20, 30, 40), (50, 60, 70), (0.0, 0.0, 1.0)),
               sc.ClusterLight((255, 0, 128), (1, 2, 3), (-0.6, 0.8, 0.0))], [],
              [sc.ClusterLight((9, 9, 9), (0, 0, 0), (0.0, 0.6, -0.8)) for _ in range(3)]]
    blob = sc.build(objs.defs, objs.placements, objs.player, lights, 3)
    assert len(blob) % 16 == 0
    sec = sc.parse_section(blob)
    assert sec.size == len(blob) and len(sec.defs) == 6 and len(sec.placements) == 8
    assert [len(c) for c in sec.lights] == [2, 0, 3]
    assert sec.type_counts == [4, 1, 1, 1, 0, 1, 0, 0, 0, 0]
    d = sec.defs[1]
    assert d["name"] == "cyborg" and d["idle_anim"] == 2 and d["anim_tag"] == objs.defs[1].anim_tag
    p = sec.placements[2]
    assert p["def_index"] == 0 and p["flags"] == 0x0B and p["bsp_mask"] == 2 and p["name_index"] == 1
    assert p["rot"] == (1.0, 0.25, -0.25) and p["variant"] == ""
    L = sec.lights[0][1]
    assert L["ambient"] == (255, 0, 128) and L["sun"] == (1, 2, 3)
    assert all(abs(a - b) < 2e-5 for a, b in zip(L["sun_dir"], (-0.6, 0.8, 0.0)))
    assert abs(math.sqrt(sum(c * c for c in L["sun_dir"])) - 1.0) < 1e-4
    assert sec.player["weapon_def"] == 5 and sec.player["weapon_hand_marker"] == "frame r wriste"
    # empty section
    empty = sc.build([], [], sc.Player(), [], 0)
    e = sc.parse_section(empty)
    assert (e.defs, e.placements, e.lights) == ([], [], [])
    # reference parser: every truncation and a bad signature / version are rejected
    for n in range(len(blob)):
        expect(lambda n=n: sc.parse_section(blob[:n]))
    expect(lambda: sc.parse_section(b"XXXX" + blob[4:]))
    expect(lambda: sc.parse_section(blob[:4] + struct.pack("<H", 2) + blob[6:]))
    # writer refuses unsorted defs, a placement def out of range, too many BSPs
    expect(lambda: sc.build(list(reversed(objs.defs)), [], objs.player, []))
    bad = sc.Placement(0, "scenery", 0, (0.0,) * 3, (0.0,) * 3, def_index=99)
    expect(lambda: sc.build(objs.defs, [bad], objs.player, []))
    # the desired permutation of a biped (Sergeant Johnson: 100) survives the build and the parse; every other placement keeps 0
    jp = sc.Placement(objs.defs[0].tag_id, "biped", 0, (0.0,) * 3, (0.0,) * 3, permutation=101)
    jb = sc.parse_section(sc.build(objs.defs, [jp], objs.player, [[]]))
    assert jb.placements[0]["permutation"] == 101 and all(q["permutation"] == 0 for q in sec.placements)
    expect(lambda: sc.build(objs.defs, [], objs.player, [[]] * 33))
    # strings are cut to fit and stay NUL terminated
    long = sc.Def(1, sc.SCENERY, "x" * 80)
    assert sc.parse_section(sc.build([long], [], sc.Player(), [])).defs[0]["name"] == "x" * 31
    return blob


# ---- lighting probes ----------------------------------------------------------------------------
def page(w: int, h: int, fn) -> texconv.Rgba:
    out = bytearray()
    for y in range(h):
        for x in range(w):
            out += bytes(fn(x, y)) + b"\xff"
    return texconv.Rgba(w, h, bytes(out))


def room(floor_lm: tuple[int, int, int], ceil_lm: tuple[int, int, int]) -> tuple[list[bspconv.Material], bsp.Structure, dict]:
    """A floor (clockwise seen from above: -(e1 x e2) = +z) and a ceiling (clockwise seen from below), one cluster.
    The floor samples the left half of page 0, the ceiling the right half."""
    fl = [(0, 0, 0), (0, 4, 0), (4, 0, 0), (4, 4, 0)]          # (e1 x e2) for (0, 1, 2) is -z: floor normal +z
    ce = [(0, 0, 3), (4, 0, 3), (0, 4, 3), (4, 4, 3)]          # (e1 x e2) is +z: ceiling normal -z
    pos = fl + ce
    lm = [(0.1, 0.5)] * 4 + [(0.9, 0.5)] * 4
    tris = [(0, 1, 2), (1, 3, 2), (4, 5, 6), (5, 7, 6)]
    mat = bspconv.Material(1, 0, pos, [(0, 0)] * 8, lm, tris, [0, 1, 2, 3])
    st = bsp.Structure(4, [], [], [0], [()], [bsp.Cluster(-1, -1, -1, -1, -1, 0, [], set())], [], 0,
                       {s: {0} for s in range(4)})
    pg = page(8, 2, lambda x, y: floor_lm if x < 4 else ceil_lm)
    return [mat], st, {0: pg}


def check_lighting() -> None:
    mats, st, pages = room((120, 100, 80), (20, 20, 20))
    (L,) = sc.cluster_lights(mats, st, pages)
    # the lit floor faces up: the light comes from above, colour follows the floor, no black
    assert L.sun_dir[2] > 0.99 and abs(math.sqrt(sum(c * c for c in L.sun_dir)) - 1) < 1e-6
    assert L.sun[0] > L.sun[1] > L.sun[2] > 0 and L.ambient[0] > L.ambient[1] > L.ambient[2]
    assert all(sc.AMBIENT_FLOOR <= a <= 255 for a in L.ambient) and all(0 <= s <= 255 for s in L.sun)
    # lit ceiling instead: the light comes from below
    mats, st, pages = room((20, 20, 20), (120, 100, 80))
    assert sc.cluster_lights(mats, st, pages)[0].sun_dir[2] < -0.99
    # flat lighting: no direction (neutral up) and no sun; black pages: the floor holds, nothing divides by zero
    mats, st, pages = room((60, 60, 60), (60, 60, 60))
    L = sc.cluster_lights(mats, st, pages)[0]
    assert L.sun == (0, 0, 0) and L.sun_dir == sc.NEUTRAL_DIR and L.ambient == (60, 60, 60)
    mats, st, pages = room((0, 0, 0), (0, 0, 0))
    L = sc.cluster_lights(mats, st, pages)[0]
    assert L.ambient == (sc.AMBIENT_FLOOR,) * 3 and L.sun == (0, 0, 0)
    # no lightmap at all (page missing): the neutral colours, floored
    L = sc.cluster_lights(mats, st, {})[0]
    assert L.ambient == (64, 64, 64) and L.sun_dir == sc.NEUTRAL_DIR
    # the gain scales the lightmap colour
    mats, st, pages = room((60, 60, 60), (60, 60, 60))
    assert sc.cluster_lights(mats, st, pages, gain=0.5)[0].ambient == (30, 30, 30)
    # a sky cluster takes the sky sun direction, strength still from the lightmap
    mats, st, pages = room((120, 100, 80), (20, 20, 20))
    st.clusters[0].sky = 0
    L = sc.cluster_lights(mats, st, pages, [(1.0, 0.0, 0.0)])[0]
    assert L.sun_dir == (1.0, 0.0, 0.0) and L.sun[0] > 0
    st.clusters[0].sky = 3                       # sky index outside the palette: gradient again
    assert sc.cluster_lights(mats, st, pages, [(1.0, 0.0, 0.0)])[0].sun_dir[2] > 0.99
    # determinism
    assert sc.cluster_lights(mats, st, pages) == sc.cluster_lights(mats, st, pages)

    # the synthetic BSP world: cluster 0 is lit twice as bright as cluster 1 on the shared page
    block, loc = world.build_world()
    wmats, _ = bspconv.read_materials(block, loc)
    wst = bsp.read_structure(loc, block)
    pg = page(16, 16, lambda x, y: (180, 160, 140) if x < 8 else (30, 30, 30))
    lights = sc.cluster_lights(wmats, wst, {3: pg})
    assert len(lights) == 2 and all(sc.AMBIENT_FLOOR <= c <= 255 for L in lights for c in L.ambient)
    assert sum(lights[0].ambient) != sum(lights[1].ambient)
    # prior area: a cluster without any lightmapped triangle gets the BSP mean, not black
    wst2 = bsp.Structure(wst.surface_count, wst.planes, wst.nodes, wst.leaves, wst.leaf_surfaces, wst.clusters,
                         wst.portals, wst.fog_planes, {s: {0} for s in range(60)})
    l2 = sc.cluster_lights(wmats, wst2, {3: pg})
    assert l2[1].ambient == tuple(max(sc.AMBIENT_FLOOR, c) for c in l2[1].ambient) and sum(l2[1].ambient) > 3 * sc.AMBIENT_FLOOR


def check_sky_suns() -> None:
    r, m = build_synthetic()
    suns = sc.sky_suns(sc.Region(m))
    assert len(suns) == 1 and suns[0] is not None
    # the brighter light (colour 1.0 x power 2.0, yaw 0, pitch 0) is the sun: towards +x
    assert all(abs(a - b) < 1e-6 for a, b in zip(suns[0], (1.0, 0.0, 0.0)))
    assert math.isclose(sum(c * c for c in suns[0]), 1.0, rel_tol=1e-6)


# ---- real maps (skipped without game data) --------------------------------------------------------
def check_real_maps() -> None:
    path = ROOT / "assets" / "decompressed" / "a10.map"
    if not path.exists():
        print("  (a10.map not present: real-map checks skipped)")
        return
    objs = sc.collect(path)
    assert objs.counts["scenery"] == 274 and objs.counts["biped"] == 107 and objs.counts["vehicle"] == 56
    assert objs.counts["machine"] == 245 and objs.counts["weapon"] == 29 and objs.counts["equipment"] == 32
    assert objs.bsp_count == 9 and len(objs.placements) == 742 and objs.unresolved == 1
    assert all(p.bsp_mask for p in objs.placements)
    assert objs.player.weapon_hand_marker == "frame r wriste" and objs.player.weapon_def and objs.player.biped_def
    blob = sc.build_section(path, [7, 8])
    sec = sc.parse_section(blob)
    assert len(sec.placements) == 742 and [len(c) > 0 for c in sec.lights] == [False] * 7 + [True] * 2
    assert objs.defs == sorted(objs.defs, key=lambda d: d.tag_id)


def main() -> None:
    objs = check_collect()
    check_locate()
    check_idle_rule()
    check_player_variants()
    check_corrupt_map()
    blob = check_serialisation(objs)
    check_lighting()
    check_sky_suns()
    check_real_maps()
    # a pack with the world's section as SCEN (3 BSPs: lights for 0 and 2, none for 1) and an empty SCE1
    lights = [[sc.ClusterLight((20, 30, 40), (50, 60, 70), (0.0, 0.0, 1.0)),
               sc.ClusterLight((255, 0, 128), (1, 2, 3), (-0.6, 0.8, 0.0))], [],
              [sc.ClusterLight((9, 9, 9), (0, 0, 0), (0.0, 0.6, -0.8)) for _ in range(3)]]
    pack = hp2.build("test", [("SCEN", sc.build(objs.defs, objs.placements, objs.player, lights, 3)),
                              ("SCE1", sc.build([], [], sc.Player(), [], 0))])
    got = hp2.parse(pack)
    assert sc.parse_section(got.read("SCEN")).placements[2]["bsp_mask"] == 2
    assert sc.parse_section(got.read("SCE1")).defs == []
    if os.environ.get("SCEN_FIXTURE"):
        Path(os.environ["SCEN_FIXTURE"]).write_bytes(pack)
    print("ok")


if __name__ == "__main__":
    main()
