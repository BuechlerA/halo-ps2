"""CMOD section on a synthetic map (no game data needed): the collision model tag reader (materials, regions,
permutations, nodes, per-node bsp rings), the box fit (conservative on every source vertex, oriented when that is
smaller), the object table, the section writer and its reference parser, corruption handling. Set CMOD_FIXTURE=<path>
to also write the .hp2 the C reader test (engine/tests/test_cmod.c, which hard-codes the numbers of this world) opens.
A skip-if-missing check converts the real a10 map when assets/decompressed/a10.map exists."""
import math
import os
import random
import struct
import sys
from pathlib import Path

sys.path[:0] = [str(Path(__file__).resolve().parents[1]), str(Path(__file__).resolve().parent)]
from halo2ps2 import cmodconv as cc, hp2, scenconv, tagindex, texconv  # noqa: E402

BASE = tagindex.XBOX_TAG_BASE
ROOT = Path(__file__).resolve().parents[2]
TAGS = [
    ("scnr", "levels\\t\\t", 0x400),
    ("coll", "characters\\hero\\hero", 0x4000), ("coll", "vehicles\\cart\\cart", 0x4000),
    ("coll", "scenery\\box\\box", 0x4000),
    ("bipd", "characters\\hero\\hero", 0x400), ("bipd", "characters\\hero2\\hero2", 0x400),
    ("scen", "scenery\\box\\box", 0x400), ("scen", "scenery\\plain\\plain", 0x400),
    ("vehi", "vehicles\\cart\\cart", 0x400), ("weap", "weapons\\gun\\gun", 0x400),
]
NULL = struct.pack("<4sIII", b"lloc", 0, 0, 0xFFFFFFFF)


# ---- a synthetic map region ----------------------------------------------------------------------------------
class Region:
    def __init__(self, tags=TAGS):
        self.region = bytearray(tagindex.TagIndex.build(tags, scenario_index=0))
        self.index = tagindex.TagIndex.parse_region(bytes(self.region), 0x1000)
        self.by_key = {(e.group, e.path): e for e in self.index.entries}
        self.cursor: dict = {}

    def entry(self, group: str, path: str) -> tagindex.TagEntry:
        return self.by_key[(group, path)]

    def block(self, e) -> int:
        return e.data_address - BASE

    def alloc(self, e, data: bytes) -> int:
        key = (e.group, e.path)
        cur = (self.cursor.get(key, 0x400) + 3) & ~3
        off = e.data_address - BASE + cur
        self.region[off:off + len(data)] = data
        self.cursor[key] = cur + len(data)
        assert cur + len(data) <= TAGS[[i for i, t in enumerate(TAGS) if (t[0], t[1]) == key][0]][2]
        return BASE + off

    def poke(self, off: int, fmt: str, *vals) -> None:
        struct.pack_into(fmt, self.region, off, *vals)

    def refl(self, off: int, count: int, addr: int) -> None:
        struct.pack_into("<III", self.region, off, count, addr, 0)

    def map(self) -> texconv.MapData:
        idx = tagindex.TagIndex.parse_region(bytes(self.region), 0x1000)
        return texconv.MapData(Path("synthetic.map"), None, idx, bytes(self.region),  # type: ignore[arg-type]
                               {e.tag_id: e for e in idx.entries})


# ---- box meshes the way the tag stores them: vertices, edges with forward / reverse links, surfaces --------------
FACES = [(0, 2, 3, 1), (4, 5, 7, 6), (0, 1, 5, 4), (2, 6, 7, 3), (0, 4, 6, 2), (1, 3, 7, 5)]


def box_points(lo, hi, yaw=0.0):
    c, s = math.cos(yaw), math.sin(yaw)
    pts = []
    for i in range(8):
        x = hi[0] if i & 1 else lo[0]
        y = hi[1] if i & 2 else lo[1]
        z = hi[2] if i & 4 else lo[2]
        pts.append((x * c - y * s, x * s + y * c, z))
    return pts


def mesh(boxes):
    """boxes: [(8 points in bit order, material)] -> (vertices, edges, surfaces) with global indices."""
    verts, edges, surfs = [], [], []
    for pts, mat in boxes:
        base, sbase, ebase = len(verts), len(surfs), len(edges)
        verts += pts
        loops = [[base + i for i in f] for f in FACES]
        edge_of: dict = {}
        recs: list = []
        for fi, loop in enumerate(loops):
            for k in range(4):
                a, b = loop[k], loop[(k + 1) % 4]
                if (b, a) in edge_of:
                    rec = edge_of[(b, a)]
                    recs[rec][5] = sbase + fi
                    edge_of[(a, b)] = rec
                else:
                    recs.append([a, b, -1, -1, sbase + fi, -1])
                    edge_of[(a, b)] = len(recs) - 1
        for loop in loops:
            for k in range(4):
                a, b, c = loop[k], loop[(k + 1) % 4], loop[(k + 2) % 4]
                rec, nxt = edge_of[(a, b)], edge_of[(b, c)]
                recs[rec][2 if recs[rec][0] == a else 3] = nxt + ebase
        edges += recs
        for loop in loops:
            surfs.append((0, edge_of[(loop[0], loop[1])] + ebase, 0, 0, mat))
    return verts, edges, surfs


def write_bsp_array(r: Region, e, perm_boxes) -> int:
    """One 0x60 byte bsp per permutation; returns the address of the array."""
    blocks = b""
    for boxes in perm_boxes:
        verts, edges, surfs = mesh(boxes)
        va = r.alloc(e, b"".join(struct.pack("<3fi", *v, 0) for v in verts))
        ea = r.alloc(e, b"".join(struct.pack("<6i", *x) for x in edges))
        sa = r.alloc(e, b"".join(struct.pack("<iiBBh", *x) for x in surfs))
        refs = [(0, 0)] * 5 + [(len(surfs), sa), (len(edges), ea), (len(verts), va)]
        blocks += b"".join(struct.pack("<III", c, a, 0) for c, a in refs)
    return r.alloc(e, blocks)


def write_coll(r: Region, e, materials, regions, nodes, *, flags=0, indirect=-1, body=0.0, shock=0.0, destroyed=0.0,
               shield=0.0, shield_mat=0, stun=0.0, recharge=0.0, rate=1.0) -> None:
    b = r.block(e)
    r.poke(b, "<I", flags)
    r.poke(b + 4, "<h", indirect)
    r.poke(b + 8, "<f", body)
    r.poke(b + 0x0C, "<f", shock)
    r.poke(b + 0x94, "<f", destroyed)
    r.poke(b + 0xCC, "<f", shield)
    r.poke(b + 0xD2, "<H", shield_mat)
    r.poke(b + 0x10C, "<2f", stun, recharge)
    r.poke(b + 0x1C0, "<f", rate)
    mats = bytearray()
    for name, mflags, mtype, leak, sm, bm in materials:
        m = bytearray(72)
        m[:len(name)] = name.encode()
        struct.pack_into("<IHH2f", m, 0x20, mflags, mtype, 0, leak, sm)
        struct.pack_into("<f", m, 0x3C, bm)
        mats += m
    r.refl(b + 0x234, len(materials), r.alloc(e, bytes(mats)))
    regs = bytearray()
    for name, rflags, thr, perms in regions:
        g = bytearray(84)
        g[:len(name)] = name.encode()
        struct.pack_into("<I", g, 0x20, rflags)
        struct.pack_into("<f", g, 0x28, thr)
        g[0x38:0x48] = struct.pack("<4sIII", b"effe", 0, 0, 0xFFFFFFFF)
        pa = r.alloc(e, b"".join(p.encode().ljust(32, b"\0") for p in perms))
        struct.pack_into("<III", g, 0x48, len(perms), pa, 0)
        regs += g
    r.refl(b + 0x240, len(regions), r.alloc(e, bytes(regs)))
    nds = bytearray()
    for name, region, parent, nxt, child, perm_boxes in nodes:
        n = bytearray(64)
        n[:len(name)] = name.encode()
        struct.pack_into("<4h", n, 0x20, region, parent, nxt, child)
        if perm_boxes:
            struct.pack_into("<III", n, 0x34, len(perm_boxes), write_bsp_array(r, e, perm_boxes), 0)
        nds += n
    r.refl(b + 0x28C, len(nodes), r.alloc(e, bytes(nds)))


def write_object(r: Region, e, otype, radius, offset, origin, render, coll) -> None:
    b = r.block(e)
    r.poke(b, "<H", otype)
    r.poke(b + 4, "<4f", radius, *offset)
    r.poke(b + 0x14, "<3f", *origin)
    r.region[b + 0x28:b + 0x38] = NULL.replace(b"lloc", b"edom")
    r.region[b + 0x70:b + 0x80] = NULL if coll is None else struct.pack("<4sIII", b"lloc", 0, 0, coll.tag_id)
    r.poke(b + 0x104, "<f", render)


HERO_REGIONS = [("body", 0, 0.0, ["__base"]), ("arm", 0x60, 0.0, ["__base", "~damaged"]), ("head", 0x202, 0.25, ["__base"])]
HERO_MATERIALS = [("body", 0, 21, 0.0, 1.0, 1.0), ("head", 1, 21, 0.0, 1.0, 2.0), ("limb", 0, 21, 0.2, 1.0, 0.8)]


def build_synthetic() -> tuple[Region, texconv.MapData]:
    r = Region()
    E = r.entry
    root_box = box_points((-0.1, -0.2, -0.1), (0.1, 0.2, 0.1))
    arm0 = box_points((0.0, -0.05, -0.05), (0.3, 0.05, 0.05), yaw=math.radians(30))
    arm1 = box_points((0.0, -0.05, -0.05), (0.1, 0.05, 0.05))
    head_a = box_points((-0.05, -0.05, -0.05), (0.15, 0.05, 0.05))
    head_b = box_points((0.2, -0.05, -0.05), (0.3, 0.05, 0.05))
    write_coll(r, E("coll", "characters\\hero\\hero"), HERO_MATERIALS, HERO_REGIONS, [
        ("root", 0, -1, -1, 1, [[(root_box, 0)]]),
        ("arm", 1, 0, 2, -1, [[(arm0, 2)], [(arm1, 0)]]),
        ("head", 2, 0, 3, -1, [[(head_a, 1), (head_b, 2)]]),
        ("tag", -1, 0, -1, -1, []),
    ], flags=7, indirect=1, body=75.0, shock=0.0, destroyed=0.0, shield=75.0, shield_mat=22, stun=6.0,
        recharge=4.0, rate=1.0 / 120.0)
    cart_hull = box_points((-1.0, -0.5, 0.0), (1.0, 0.5, 0.6))
    wheel0 = box_points((-0.2, -0.1, -0.2), (0.2, 0.1, 0.2))
    wheel1 = box_points((-0.25, -0.1, -0.25), (0.25, 0.1, 0.25))
    write_coll(r, E("coll", "vehicles\\cart\\cart"), [("metal", 0, 7, 0.0, 0.0, 0.0), ("glass", 0, 9, 0.0, 0.0, 0.0)],
               [("hull", 0x4, 0.0, ["__base"]), ("wheel", 0, 0.0, ["__base", "~blur"])], [
        ("frame", 0, -1, -1, 1, [[(cart_hull, 0)]]),
        ("wheel", 1, 0, -1, -1, [[(wheel0, 1)], [(wheel1, 1)]]),
    ], flags=0x28, indirect=-1, body=0.0)
    box = box_points((-1.0, -1.0, 0.0), (1.0, 1.0, 2.0))
    write_coll(r, E("coll", "scenery\\box\\box"), [("stone", 0, 4, 0.0, 0.0, 0.0)],
               [("__unnamed", 0, 0.0, ["__base"])], [("frame", 0, -1, -1, -1, [[(box, 0)]])])
    hero, cart, boxc = (E("coll", p) for p in ("characters\\hero\\hero", "vehicles\\cart\\cart", "scenery\\box\\box"))
    write_object(r, E("bipd", "characters\\hero\\hero"), 0, 0.45, (-0.02, 0, 0), (0, 0, 0), 0.45, hero)
    write_object(r, E("bipd", "characters\\hero2\\hero2"), 0, 0.4, (0, 0, 0.1), (0, 0, -0.38), 2.0, hero)
    write_object(r, E("scen", "scenery\\box\\box"), 6, 1.5, (0, 0, 1.0), (0, 0, 0), 1.5, boxc)
    write_object(r, E("scen", "scenery\\plain\\plain"), 6, 1.0, (0, 0, 0), (0, 0, 0), 1.0, None)
    write_object(r, E("vehi", "vehicles\\cart\\cart"), 1, 1.2, (0, 0, 0), (0, 0, 0), 10.0, cart)
    write_object(r, E("weap", "weapons\\gun\\gun"), 2, 0.1, (0, 0, 0), (0, 0, 0), 0.1, None)
    return r, r.map()


def expect(fn, exc=cc.CmodError) -> None:
    try:
        fn()
    except exc:
        return
    raise SystemExit("corrupt input accepted")


# ---- tests --------------------------------------------------------------------------------------------------------
def inside(box, p, tol=0.0) -> bool:
    d = [p[k] - box[0][k] for k in range(3)]
    return all(abs(sum(box[2][k][i] * d[i] for i in range(3))) <= box[1][k] + tol for k in range(3))


def check_fit() -> None:
    rnd = random.Random(7)
    # axis-aligned box: the AABB is kept, tight to the margin
    pts = [(x, y, z) for x in (-1.0, 3.0) for y in (0.0, 2.0) for z in (5.0, 6.0)]
    c, h, axes, vol = cc.fit_box(pts)
    assert axes == ((1.0, 0.0, 0.0), (0.0, 1.0, 0.0), (0.0, 0.0, 1.0))
    assert all(abs(a - b) < 1e-6 for a, b in zip(c, (1.0, 1.0, 5.5)))
    assert all(abs(a - b) < 1e-6 for a, b in zip(h, (2.0 + cc.MARGIN, 1.0 + cc.MARGIN, 0.5 + cc.MARGIN)))
    # a box turned by 30 degrees: the oriented box is chosen and its volume is close to the true one
    pts = cc_box = box_points((0.0, -0.05, -0.05), (0.3, 0.05, 0.05), yaw=math.radians(30))
    c, h, axes, vol_aabb = cc.fit_box(pts)
    true_vol = 0.3 * 0.1 * 0.1
    vol = 8 * h[0] * h[1] * h[2]
    assert axes != ((1.0, 0.0, 0.0), (0.0, 1.0, 0.0), (0.0, 0.0, 1.0)) and vol < 1.35 * true_vol < 0.9 * vol_aabb
    for k in range(3):
        assert abs(sum(a * a for a in axes[k]) - 1.0) < 1e-6
    assert abs(sum(axes[0][i] * axes[1][i] for i in range(3))) < 1e-6
    # random clouds, planes, lines, a point: every source vertex is inside with the float32 numbers
    for n in (1, 2, 3, 4, 17, 200):
        for kind in range(4):
            cloud = []
            for _ in range(n):
                v = [rnd.uniform(-3, 3), rnd.uniform(-1, 1), rnd.uniform(-0.2, 0.2)]
                if kind == 1:
                    v[2] = 0.0                                             # planar
                if kind == 2:
                    v[1] = v[2] = 0.0                                      # collinear
                if kind == 3:
                    v = [30.0 + v[0] * 1e-3, -50.0 + v[1] * 1e-3, 8.0]     # tiny and far from the origin
                cloud.append(tuple(v))
            box = cc.fit_box(cloud)
            assert all(inside(box, p) for p in cloud), (n, kind)
            assert all(h > 0 for h in box[1])
    # rotate a long thin cloud about a skew axis: the oriented box beats the AABB by a wide margin
    cloud = []
    for _ in range(300):
        t, a, b = rnd.uniform(-2, 2), rnd.uniform(-0.05, 0.05), rnd.uniform(-0.05, 0.05)
        cloud.append((t * 0.6 + a, t * 0.64 + b, t * 0.48 + a))
    box = cc.fit_box(cloud)
    assert 8 * box[1][0] * box[1][1] * box[1][2] < 0.2 * box[3] and all(inside(box, p) for p in cloud)
    assert cc.f32_up(0.1) >= 0.1 and cc.f32_up(-0.1) >= -0.1 and cc.f32_up(1.0) == 1.0


def check_read() -> cc.CmodSet:
    r, m = build_synthetic()
    cs = cc.collect(m)
    assert cs.problems == [], cs.problems
    assert [x.path for x in cs.models] == ["characters\\hero\\hero", "vehicles\\cart\\cart", "scenery\\box\\box"]
    hero, cart, box = cs.models
    assert hero.flags == 7 and hero.indirect_damage_material == 1 and hero.shield_material == 22
    assert (hero.max_body_vitality, hero.max_shield_vitality, hero.shield_stun_time, hero.shield_recharge_time) == (75.0, 75.0, 6.0, 4.0)
    assert abs(hero.shield_recharge_rate - 1 / 120) < 1e-9
    assert [x.name for x in hero.materials] == ["body", "head", "limb"] and hero.materials[1].flags == 1
    assert [x.body_multiplier for x in hero.materials] == [1.0, 2.0, f32(0.8)] and hero.materials[2].shield_leak == f32(0.2)
    assert [(x.name, x.flags, x.perms) for x in hero.regions] == [
        ("body", 0, ["__base"]), ("arm", 0x60, ["__base", "~damaged"]), ("head", 0x202, ["__base"])]
    assert hero.regions[2].damage_threshold == 0.25 and hero.regions[0].destroyed_effect == 0
    assert [(n.name, n.region, n.parent, n.first_child, n.next_sibling) for n in hero.nodes] == [
        ("root", 0, -1, 1, -1), ("arm", 1, 0, -1, 2), ("head", 2, 0, -1, 3), ("tag", -1, 0, -1, -1)]
    # shapes: (region, perm, node, material) order; the head node splits into two materials, the node without a bsp has none
    assert [(s.region, s.perm, s.node, s.material, s.flags, s.vertices) for s in hero.shapes] == [
        (0, 0, 0, 0, 0, 8), (1, 0, 1, 2, 0, 8), (1, 1, 1, 0, 0, 8), (2, 0, 2, 1, cc.SHAPE_HEAD, 8), (2, 0, 2, 2, 0, 8)]
    assert hero.source_vertices == 40 and hero.empty_bsps == 0
    assert len(cart.shapes) == 3 and len(cart.regions[1].perms) == 2 and cart.max_body_vitality == 0.0 and cart.flags == 0x28
    assert cart.indirect_damage_material == -1 and cart.shield_recharge_rate == 1.0
    assert len(box.shapes) == 1 and box.regions[0].name == "__unnamed"
    # every source vertex is inside its own shape, with the float32 numbers
    pts: dict = {}
    cc.read_collision_model(scenconv.Region(m), m.by_id[hero.tag_id], pts)
    for s in hero.shapes:
        assert all(inside((s.center, s.half, s.axes), p) for p in pts[(s.node, s.perm, s.material)])
    arm0 = hero.shapes[1]
    assert arm0.axes[0] != (1.0, 0.0, 0.0), "the turned arm box must be oriented"
    # objects: only tags with a collision model, sorted by tag id; the two bipeds share the model
    assert [o.path for o in cs.objects] == ["characters\\hero\\hero", "characters\\hero2\\hero2", "scenery\\box\\box",
                                            "vehicles\\cart\\cart"]
    assert [o.group for o in cs.objects] == ["bipd", "bipd", "scen", "vehi"]
    assert cs.objects[0].coll_tag == cs.objects[1].coll_tag == hero.tag_id
    o = cs.objects[1]
    assert (o.type, f32(o.radius), tuple(map(f32, o.offset)), tuple(map(f32, o.origin)), f32(o.render_radius)) == (
        0, f32(0.4), (0.0, 0.0, f32(0.1)), (0.0, 0.0, f32(-0.38)), 2.0)
    return cs


def f32(v: float) -> float:
    return cc.f32(v)


def check_serialisation(cs: cc.CmodSet) -> bytes:
    blob = cc.build(cs.models, cs.objects)
    assert len(blob) % 16 == 0
    sec = cc.parse_section(blob)
    assert len(sec.models) == 3 and len(sec.objects) == 4 and len(sec.shapes) == 9 and len(sec.materials) == 6
    assert len(sec.regions) == 6 and len(sec.perms) == 8 and len(sec.nodes) == 7 and abs(sec.margin - cc.MARGIN) < 1e-9
    hero = sec.models[0]
    assert hero["node_count"] == 4 and hero["region_count"] == 3 and hero["max_shield_vitality"] == 75.0
    assert [m["first_shape"] for m in sec.models] == [0, 5, 8] and [m["shape_count"] for m in sec.models] == [5, 3, 1]
    assert sec.perms == ["__base", "__base", "~damaged", "__base", "__base", "__base", "~blur", "__base"]
    assert sec.objects[1]["model_index"] == 0 and sec.objects[3]["model_index"] == 1
    # a section round trips through a pack
    pk = hp2.parse(hp2.build("t", [("CMOD", blob)]))
    assert pk.read("CMOD") == blob
    # an empty section is valid
    empty = cc.parse_section(cc.build([], []))
    assert empty.models == [] and empty.objects == [] and empty.shapes == []
    # unsorted input is refused by the writer
    expect(lambda: cc.build(list(reversed(cs.models)), cs.objects))
    expect(lambda: cc.build(cs.models, list(reversed(cs.objects))))
    expect(lambda: cc.build(cs.models[:1], cs.objects))      # object whose model is missing
    return blob


def check_corrupt(blob: bytes) -> None:
    expect(lambda: cc.parse_section(b""))
    for n in range(0, len(blob), 7):
        expect(lambda n=n: cc.parse_section(blob[:n]))        # truncated at every 7th length
    expect(lambda: cc.parse_section(b"XXXX" + blob[4:]))
    expect(lambda: cc.parse_section(blob[:4] + struct.pack("<H", 2) + blob[6:]))
    rejected = survived = 0
    for pos in range(len(blob)):
        for v in (0x00, 0xFF, blob[pos] ^ 0x80):
            if v == blob[pos]:
                continue
            bad = bytearray(blob)
            bad[pos] = v
            try:
                cc.parse_section(bytes(bad))
                survived += 1
            except cc.CmodError:
                rejected += 1
    assert rejected > 200 and survived > 0                      # structural bytes are caught, payload bytes (floats) are not
    # a header count past the table
    bad = bytearray(blob)
    struct.pack_into("<I", bad, 0x24, 1 << 30)
    expect(lambda: cc.parse_section(bytes(bad)))


def check_bad_tags() -> None:
    # a broken ring, a material outside the table, a node whose bsp count disagrees with its region: the model is
    # refused with CmodError, collect() reports it as a problem and still converts the others
    def corrupt(fn) -> cc.CmodSet:
        r, m = build_synthetic()
        e = r.entry("coll", "scenery\\box\\box")
        fn(r, e)
        return cc.collect(r.map())

    def ring(r, e):
        b = r.block(e)
        nc, na = struct.unpack_from("<II", r.region, b + 0x28C)
        bc, ba = struct.unpack_from("<II", r.region, na - BASE + 0x34)
        sc_, sa = struct.unpack_from("<II", r.region, ba - BASE + 5 * 12)
        ec_, ea = struct.unpack_from("<II", r.region, ba - BASE + 6 * 12)
        struct.pack_into("<i", r.region, ea - BASE + 8, 3)           # edge 0: forward edge -> another face's edge
    cs = corrupt(ring)
    assert len(cs.models) == 2 and len(cs.problems) == 1 and cs.problems[0].startswith("scenery\\box\\box: surface"), cs.problems

    def material(r, e):
        b = r.block(e)
        nc, na = struct.unpack_from("<II", r.region, b + 0x28C)
        bc, ba = struct.unpack_from("<II", r.region, na - BASE + 0x34)
        sc_, sa = struct.unpack_from("<II", r.region, ba - BASE + 5 * 12)
        struct.pack_into("<h", r.region, sa - BASE + 10, 5)          # surface 0 material 5 of 1
    cs = corrupt(material)
    assert len(cs.models) == 2 and "material" in cs.problems[0]

    def permcount(r, e):
        b = r.block(e)
        nc, na = struct.unpack_from("<II", r.region, b + 0x28C)
        struct.pack_into("<I", r.region, na - BASE + 0x34, 2)        # two bsps for a one permutation region
    cs = corrupt(permcount)
    assert len(cs.models) == 2 and "bsps" in cs.problems[0]

    def outside(r, e):
        b = r.block(e)
        struct.pack_into("<I", r.region, b + 0x28C + 4, 0x10)        # node array address outside the tag region
    cs = corrupt(outside)
    assert len(cs.models) == 2 and len(cs.problems) == 1


# ---- real maps (skipped without game data) ---------------------------------------------------------------------
def check_real_maps() -> None:
    path = ROOT / "assets" / "decompressed" / "a10.map"
    if not path.exists():
        print("  (a10.map not present: real-map checks skipped)")
        return
    mp = texconv.MapData.open(path)
    cs = cc.collect(mp)
    assert cs.problems == [], cs.problems
    assert len(cs.models) == 47 and len(cs.objects) == 58
    by_path = {m.path: m for m in cs.models}
    cy = by_path["characters\\cyborg\\cyborg"]
    assert (cy.max_body_vitality, cy.max_shield_vitality, cy.shield_recharge_time) == (75.0, 75.0, 4.0)
    assert abs(cy.shield_recharge_rate - 1.0 / (30 * cy.shield_recharge_time)) < 1e-6 and cy.flags == 7
    assert [m.name for m in cy.materials] == ["head", "body", "legs", "shield generator"] and cy.materials[0].flags == 1
    assert [m.body_multiplier for m in cy.materials] == [1.0, 1.0, f32(0.8), 1.0]
    assert [r.name for r in cy.regions] == ["cyborg"] and cy.regions[0].perms == ["__base"]
    assert len(cy.nodes) == 19 and sum(1 for n in cy.nodes if n.region < 0) == 3
    head = [s for s in cy.shapes if cy.nodes[s.node].name == "bip01 head"]
    assert len(head) == 1 and head[0].flags == cc.SHAPE_HEAD and head[0].material == 0
    assert by_path["characters\\elite\\elite"].max_shield_vitality == 100.0
    assert by_path["weapons\\pistol\\pistol"].max_body_vitality == 0.0
    # node names equal the render model's, index for index, for every object that has both
    r = scenconv.Region(mp)
    checked = 0
    for o in cs.objects:
        if not o.model_tag:
            continue
        m = by_path[mp.by_id[o.coll_tag].path]
        mod = scenconv.model_nodes(r, r.block(mp.by_id[o.model_tag]))
        if mod != [n.name for n in m.nodes] and len(mod) != len(m.nodes):
            raise AssertionError(f"{o.path}: node count differs")
        checked += 1
    assert checked >= 45
    # conservative: every source vertex inside its box (after float32 rounding), measured on the written section
    res = cc.measure(path, render=False)
    assert res["coll_vertices"] > 4000 and res["coll_inside"] == res["coll_vertices"]
    assert min(res["volume_ratio"]) < 0.9 and max(res["volume_ratio"]) <= 1.0 + 1e-6
    blob = cc.build_section(path)
    sec = cc.parse_section(blob)
    assert len(sec.models) == 47 and len(sec.shapes) == 234


def main() -> None:
    check_fit()
    cs = check_read()
    blob = check_serialisation(cs)
    check_corrupt(blob)
    check_bad_tags()
    check_real_maps()
    # the fixture pack for the C reader test: the world's section as CMOD and an empty one as CMD1
    pack = hp2.build("test", [("CMOD", blob), ("CMD1", cc.build([], []))])
    got = hp2.parse(pack)
    assert cc.parse_section(got.read("CMOD")).objects[2]["radius"] == 1.5 and cc.parse_section(got.read("CMD1")).models == []
    if os.environ.get("CMOD_FIXTURE"):
        Path(os.environ["CMOD_FIXTURE"]).write_bytes(pack)
    print("ok")


if __name__ == "__main__":
    main()
