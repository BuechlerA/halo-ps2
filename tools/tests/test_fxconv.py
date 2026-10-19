"""FXD0 / FXTX on a synthetic map (no game data needed): the effect closure, every tag layout, the section writer and
its reference parser, the textures, corruption handling. Set FXDEF_FIXTURE=<path> to also write the .hp2 the C reader
test (engine/tests/test_fxdef.c, which hard-codes the numbers of this world) opens. A skip-if-missing check converts
the real a10 map when assets/decompressed/a10.map exists."""
import math
import os
import random
import struct
import sys
from pathlib import Path

sys.path[:0] = [str(Path(__file__).resolve().parents[1]), str(Path(__file__).resolve().parent)]
from halo2ps2 import fxconv as fx, hp2, tagindex, texconv  # noqa: E402

BASE = tagindex.XBOX_TAG_BASE
ROOT = Path(__file__).resolve().parents[2]
NULL = 0xFFFFFFFF


def f32(x):
    return struct.unpack('<f', struct.pack('<f', x))[0]


# ---- a synthetic tag region -------------------------------------------------------------------------------------
class Region:
    """Tag region builder: tags with zero-filled blocks; ``alloc`` appends nested data after the root struct."""

    def __init__(self, tags):
        self.region = bytearray(tagindex.TagIndex.build(tags, scenario_index=0))
        self.index = tagindex.TagIndex.parse_region(bytes(self.region), 0x1000)
        self.by_path = {(e.group, e.path): e for e in self.index.entries}
        self.cursor = {}

    def e(self, group, path):
        return self.by_path[(group, path)]

    def tid(self, group, path):
        return self.e(group, path).tag_id

    def block(self, group, path):
        return self.e(group, path).data_address - BASE

    def alloc(self, group, path, data, start=0x800):
        e = self.e(group, path)
        cur = (self.cursor.get((group, path), start) + 3) & ~3
        off = e.data_address - BASE + cur
        assert cur + len(data) <= self.size(e), "tag block overflow"
        self.region[off:off + len(data)] = data
        self.cursor[(group, path)] = cur + len(data)
        return BASE + off

    def size(self, e):
        nxt = [x.data_address for x in self.index.entries if x.data_address > e.data_address]
        return (min(nxt) if nxt else BASE + len(self.region)) - e.data_address

    def poke(self, off, fmt, *vals):
        struct.pack_into(fmt, self.region, off, *vals)

    def refl(self, off, count, addr):
        struct.pack_into("<III", self.region, off, count, addr, 0)

    def ref(self, group, tid):
        return struct.pack("<4sIII", group[::-1].encode(), 0, 0, tid)

    def set_ref(self, off, group, tid):
        self.region[off:off + 16] = self.ref(group, tid)


class FakeMap(texconv.MapData):
    pix: dict = {}

    def pixels(self, img):
        return self.pix[img.pixel_offset]


def tag(g, p, size=0x400):
    return (g, p, size)


TAGS = [
    tag("scnr", "levels\\t\\t", 0x1000), tag("matg", "globals\\globals", 0x1000),
    tag("weap", "weapons\\ar\\ar", 0x2000), tag("proj", "weapons\\ar\\bullet", 0x1000),
    tag("effe", "weapons\\ar\\fire", 0x4000), tag("effe", "weapons\\ar\\impact", 0x4000),
    tag("effe", "weapons\\ar\\splash", 0x4000), tag("effe", "levels\\t\\boom", 0x4000),
    tag("snd!", "sound\\fire", 0x100), tag("jpt!", "weapons\\ar\\damage", 0x400),
    tag("ligh", "weapons\\ar\\muzzle", 0x400), tag("deca", "effects\\hole", 0x400), tag("deca", "effects\\hole2", 0x400),
    tag("pctl", "effects\\boom", 0x1000), tag("part", "effects\\dust", 0x400), tag("part", "effects\\spark", 0x400),
    tag("pphy", "effects\\dust", 0x100), tag("bitm", "effects\\dust", 0x1000), tag("bitm", "effects\\hole", 0x1000),
    tag("garb", "debris\\piece", 0x800), tag("mode", "debris\\piece", 0x400),
]


def le(r, g, p):
    return r.tid(g, p)


def write_bitmap(r, path, kind, images, sequences, base_offset, pix):
    """images: [(w, h)]; sequences: [(first, count, [(img, l, r, t, b, rx, ry)])]."""
    e = r.e("bitm", path)
    b = e.data_address - BASE
    r.poke(b, "<H", kind)
    data = bytearray()
    for i, (w, h) in enumerate(images):
        off = base_offset + i * 0x10000
        pixels = bytearray()
        for y in range(h):
            for x in range(w):
                pixels += bytes((x * 255 // max(1, w - 1), y * 255 // max(1, h - 1), 128 + 8 * i, 255 if (x + y) % 5 else 40))
        pix[off] = bytes(pixels)
        data += struct.pack("<4sHHHHHHhhHHII", b"mtib", w, h, 1, 0, 11, 1, 0, 0, 0, 0, off, len(pixels)).ljust(48, b"\0")
    r.refl(b + 0x60, len(images), r.alloc("bitm", path, bytes(data)))
    seqs = bytearray()
    for si, (first, count, sprites) in enumerate(sequences):
        sp = b"".join(struct.pack("<H2x4x4f2f", *s) for s in sprites)
        spaddr = r.alloc("bitm", path, sp) if sprites else 0
        q = bytearray(64)
        q[:2] = b"s%d" % si
        struct.pack_into("<HH", q, 0x20, first, count)
        struct.pack_into("<III", q, 0x34, len(sprites), spaddr, 0)
        seqs += q
    r.refl(b + 0x54, len(sequences), r.alloc("bitm", path, bytes(seqs)))


def write_effect(r, path, flags, ls, lp, radius, locs, events):
    e = r.e("effe", path)
    b = e.data_address - BASE
    r.poke(b, "<Ihhf", flags, ls, lp, radius)
    if locs:
        r.refl(b + 0x28, len(locs), r.alloc("effe", path, b"".join(n.encode().ljust(32, b"\0") for n in locs), 0x100))
    ev = bytearray()
    for skip, delay, dur, parts, emitters in events:
        v = bytearray(68)
        struct.pack_into("<f2f2f", v, 4, skip, *delay, *dur)
        pb = bytearray()
        for cin, viol, loc, pf, group, tid, vel, cone, angvel, radmod, a, bb in parts:
            q = bytearray(104)
            struct.pack_into("<4H", q, 0, cin, viol, loc, pf)
            q[0x14:0x18] = group.encode()[::-1]
            q[0x18:0x28] = r.ref(group, tid)
            struct.pack_into("<2ff2f2f", q, 0x40, *vel, cone, *angvel, *radmod)
            struct.pack_into("<II", q, 0x60, a, bb)
            pb += q
        if parts:
            r.refl_at = None
            struct.pack_into("<III", v, 0x2C, len(parts), r.alloc("effe", path, bytes(pb), 0x300), 0)
        cb = bytearray()
        for (cin, viol, create, loc, yaw, pitch, off, tid, flags_, dist, count, dr, vel, cone, angvel, rad, tlo, thi) in emitters:
            q = bytearray(232)
            struct.pack_into("<5H", q, 0, cin, viol, create, 0, loc)
            struct.pack_into("<2f3f3f", q, 0x0C, yaw, pitch, *off,
                             math.cos(pitch) * math.cos(yaw), math.cos(pitch) * math.sin(yaw), math.sin(pitch))
            q[0x54:0x64] = r.ref("part", tid)
            struct.pack_into("<IH2x2h2f", q, 0x64, flags_, dist, *count, *dr)
            struct.pack_into("<2f", q, 0x84, *vel)
            struct.pack_into("<f2f", q, 0x8C, cone, *angvel)
            struct.pack_into("<2f", q, 0xA0, *rad)
            struct.pack_into("<4f4f", q, 0xB0, *tlo, *thi)
            cb += q
        if emitters:
            struct.pack_into("<III", v, 0x38, len(emitters), r.alloc("effe", path, bytes(cb), 0x1000), 0)
        ev += v
    r.refl(b + 0x34, len(events), r.alloc("effe", path, bytes(ev), 0x200))


def build_synthetic():
    r = Region(TAGS)
    T = lambda g, p: r.tid(g, p)  # noqa: E731
    pix: dict = {}
    # bitmaps: a sprite sheet with two images, and a plain 2D bitmap whose one sequence lists both of its images
    write_bitmap(r, "effects\\dust", 3, [(16, 16), (32, 16)],
                 [(0, 0, [(0, 0.0, 0.5, 0.0, 0.5, 0.0, 0.0), (0, 0.5, 1.0, 0.0, 0.5, 0.1, -0.1), (1, 0.0, 1.0, 0.0, 1.0, 0.0, 0.0)]),
                  (0, 0, [(1, 0.25, 0.75, 0.25, 0.75, 0.5, 0.5)]), (0, 0, [])],
                 0x100000, pix)
    write_bitmap(r, "effects\\hole", 0, [(16, 16), (16, 16)], [(0, 2, [])], 0x200000, pix)
    # point physics
    b = r.block("pphy", "effects\\dust")
    r.poke(b, "<I", 3)
    r.poke(b + 8, "<2f", 0.5, -0.999)
    r.poke(b + 0x20, "<5f", 2.4, 900.0, 14000.0, 0.1, 0.35)
    # particles
    for path, bitmap, phys, blend, coll, death, life in (
            ("effects\\dust", T("bitm", "effects\\dust"), T("pphy", "effects\\dust"), 3, T("snd!", "sound\\fire"),
             T("effe", "weapons\\ar\\splash"), (0.5, 0.3)),
            ("effects\\spark", T("bitm", "effects\\dust"), NULL, 0, NULL, NULL, (0.2, 0.2))):
        b = r.block("part", path)
        r.poke(b, "<I", 0x208)
        r.set_ref(b + 0x04, "bitm", bitmap)
        r.set_ref(b + 0x14, "pphy", phys)
        r.set_ref(b + 0x24, "foot", NULL)
        r.poke(b + 0x38, "<2f", *life)
        r.poke(b + 0x40, "<2f", 0.1, 0.2)
        r.set_ref(b + 0x48, "snd!" if coll != NULL else "effe", coll)
        r.set_ref(b + 0x58, "effe", death)
        r.poke(b + 0x68, "<f", 4.0)
        r.poke(b + 0x74, "<2f", 1.0, 2.0)
        r.poke(b + 0x80, "<2f", 30.0, 30.0)
        r.poke(b + 0x88, "<3f", 0.0, 5.0, 4.0)
        r.poke(b + 0x98, "<4h", 0, 1, 0, 0)
        r.poke(b + 0xA8, "<f", 0.02)
        r.poke(b + 0xAC, "<H", 1)
        r.poke(b + 0xD4, "<6H", 1, 0, 1, blend, 1, 0)
    # decals (a chain of two) and a light
    for path, nxt, bitmap, blend in (("effects\\hole", T("deca", "effects\\hole2"), T("bitm", "effects\\hole"), 2),
                                     ("effects\\hole2", NULL, T("bitm", "effects\\hole"), 1)):
        b = r.block("deca", path)
        r.poke(b, "<3H", 8, 2, 0)
        r.set_ref(b + 0x08, "deca", nxt)
        r.poke(b + 0x18, "<2f", 0.15, 0.1)
        r.poke(b + 0x2C, "<2f", 0.5, 1.0)
        r.poke(b + 0x34, "<6f", 0.25, 0.5, 0.75, 1.0, 0.5, 0.0)
        r.poke(b + 0x58, "<2h", 2, 3)
        r.poke(b + 0x78, "<4f", 180.0, 180.0, 20.0, 20.0)
        r.poke(b + 0xC0, "<H", blend)
        r.set_ref(b + 0xD8, "bitm", bitmap)
        r.poke(b + 0xFC, "<f", 16.0)
    b = r.block("ligh", "weapons\\ar\\muzzle")
    r.poke(b, "<If2f2f", 0x21, 2.0, 1.0, 1.0, math.pi, math.pi)
    r.poke(b + 0x30, "<I8f", 3, 0.0, 0.0, 0.0, 0.0, 1.0, 0.96, 0.91, 0.69)
    r.poke(b + 0xF4, "<f", 30.0)
    r.poke(b + 0xFA, "<H", 3)
    # a particle system: 1 type, 2 states, 2 particle states, constants
    b = r.block("pctl", "effects\\boom")
    r.set_ref(b + 0x38, "pphy", T("pphy", "effects\\dust"))
    r.poke(b + 0x48, "<H", 1)
    t = bytearray(128)
    t[:4] = b"fire"
    struct.pack_into("<IH2xH2xf", t, 0x20, 0x10, 12, 1, 1.5)
    struct.pack_into("<H", t, 0x54, 1)
    states = bytearray()
    for nm, d in (("birth", (1.0, 1.5)), ("death", (0.0, 0.0))):
        st = bytearray(192)
        st[:len(nm)] = nm.encode()
        struct.pack_into("<2f2f", st, 0x20, *d, 0.25, 0.5)
        struct.pack_into("<3f4f3f", st, 0x34, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 0.5, 1.0, 15.0)
        struct.pack_into("<HH", st, 0xB0, 1, 0)
        states += st
    pstates = bytearray()
    for nm, seq, blend in (("fire", 0, 3), ("smoke", 1, 0)):
        ps = bytearray(376)
        ps[:len(nm)] = nm.encode()
        struct.pack_into("<2f2f", ps, 0x20, 0.1, 0.2, 0.1, 0.8)
        ps[0x30:0x40] = r.ref("bitm", T("bitm", "effects\\dust"))
        struct.pack_into("<H", ps, 0x40, seq)
        struct.pack_into("<2f2f2f", ps, 0x48, 0.01, 0.03, 0.0, 0.0, 3.5, 7.0)
        struct.pack_into("<4f4f", ps, 0x60, 1.0, 1.0, 0.92, 0.64, 1.0, 1.0, 0.5, 0.28)
        struct.pack_into("<f", ps, 0x80, 1.0)
        ps[0x84:0x94] = r.ref("pphy", T("pphy", "effects\\dust"))
        struct.pack_into("<3H", ps, 0xE0, 1, blend, 0)
        pstates += ps
    consts = struct.pack("<2f", 0.1, 0.2)
    r.refl(b + 0x50, 1, r.alloc("pctl", "effects\\boom", struct.pack("<f", 0.1), 0x100))
    # type element first, then its blocks (addresses relative to the allocation order)
    taddr = r.alloc("pctl", "effects\\boom", bytes(t), 0x200)
    struct.pack_into("<III", t, 0x5C, 2, r.alloc("pctl", "effects\\boom", consts), 0)
    struct.pack_into("<III", t, 0x68, 2, r.alloc("pctl", "effects\\boom", bytes(states)), 0)
    struct.pack_into("<III", t, 0x74, 2, r.alloc("pctl", "effects\\boom", bytes(pstates)), 0)
    off = taddr - BASE
    r.region[off:off + 128] = t
    r.refl(b + 0x5C, 1, taddr)
    # objects: a garbage piece with a model
    ob = r.block("garb", "debris\\piece")
    r.poke(ob, "<H", 4)
    r.set_ref(ob + 0x28, "mode", T("mode", "debris\\piece"))
    # effects
    write_effect(r, "weapons\\ar\\fire", 2, -1, -1, 0.0, ["", "muzzle"], [
        (0.0, (0.0, 0.0), (0.0, 0.0),
         [(0, 0, 0, 0, "snd!", T("snd!", "sound\\fire"), (1.0, 1.0), 0.0, (0.0, 0.0), (1.0, 1.0), 0, 0),
          (0, 1, 1, 0, "ligh", T("ligh", "weapons\\ar\\muzzle"), (0.0, 0.0), 0.0, (0.0, 0.0), (1.0, 1.0), 0, 0)],
         [(0, 0, 0, 1, 0.5, 0.25, (0.1, 0.2, 0.3), T("part", "effects\\spark"), 1, 2, (1, 3), (0.0, 0.5), (1.0, 2.0), 0.1,
           (1.0, -1.0), (0.05, 0.1), (1.0, 1.0, 0.5, 0.25), (1.0, 1.0, 1.0, 1.0))]),
        (0.25, (0.5, 0.2), (1.0, 2.0),
         [(1, 0, 0, 0, "jpt!", T("jpt!", "weapons\\ar\\damage"), (0.0, 0.0), 0.0, (0.0, 0.0), (1.0, 1.0), 0, 0),
          (0, 0, 0, 0, "pctl", T("pctl", "effects\\boom"), (0.0, 0.0), 0.0, (0.0, 0.0), (1.0, 1.0), 0, 0)], []),
    ])
    write_effect(r, "weapons\\ar\\impact", 0, 1, -1, 4.5, [""], [
        (0.0, (0.0, 0.0), (0.0, 0.0),
         [(0, 0, 0, 1, "deca", T("deca", "effects\\hole"), (1.0, 1.0), 0.0, (0.0, 0.0), (0.25, 0.25), 0, 0),
          (0, 0, 0xFFFF, 0, "snd!", T("snd!", "sound\\fire"), (0.0, 0.0), 0.0, (0.0, 0.0), (1.0, 1.0), 0, 0)],
         [(1, 0, 0, 0, 0.0, 0.0, (0.0, 0.0, 0.0), T("part", "effects\\dust"), 0, 3, (2, 5), (0.0, 0.0), (1.0, 3.0), 0.5,
           (-6.0, 6.0), (0.08, 0.15), (1.0, 0.5, 0.4, 0.3), (1.0, 0.4, 0.3, 0.3))]),
    ])
    write_effect(r, "weapons\\ar\\splash", 1, -1, -1, 0.0, [], [
        (0.0, (0.0, 0.0), (0.0, 0.0), [(0, 0, 0xFFFF, 0, "snd!", T("snd!", "sound\\fire"), (0.0, 0.0), 0.0, (0.0, 0.0), (1.0, 1.0), 0, 0)], [])])
    write_effect(r, "levels\\t\\boom", 0, -1, -1, 8.0, [""], [
        (0.0, (0.0, 0.0), (0.0, 0.0),
         [(0, 0, 0, 0, "garb", T("garb", "debris\\piece"), (2.0, 4.0), 0.5, (0.0, 0.0), (1.0, 1.0), 0, 0)], [])])
    # gameplay roots: the weapon references fire (at the top and, nested far down, impact again), the projectile impact
    wb = r.block("weap", "weapons\\ar\\ar")
    r.set_ref(wb + 0x500, "effe", T("effe", "weapons\\ar\\fire"))
    r.set_ref(wb + 0x1900, "effe", T("effe", "weapons\\ar\\fire"))
    pb = r.block("proj", "weapons\\ar\\bullet")
    r.set_ref(pb + 0x300, "effe", T("effe", "weapons\\ar\\impact"))
    idx = tagindex.TagIndex.parse_region(bytes(r.region), 0x1000)
    m = FakeMap(Path("synthetic.map"), None, idx, bytes(r.region), {e.tag_id: e for e in idx.entries})  # type: ignore[arg-type]
    m.pix = pix
    return r, m


def expect(fn, exc=fx.FxError):
    try:
        fn()
    except exc:
        return
    raise SystemExit("corrupt input accepted")


# ---- tests -------------------------------------------------------------------------------------------------------
def check_closure(r, m):
    ids, objs = fx.effect_closure(m)
    paths = [m.by_id[i].path for i in ids]
    # weap -> fire, proj -> impact, the dust particle's death effect -> splash; boom only with --all
    assert ids == sorted(ids) and set(paths) == {"weapons\\ar\\fire", "weapons\\ar\\impact", "weapons\\ar\\splash"}, paths
    assert objs == []
    ids2, objs2 = fx.effect_closure(m, True)
    assert len(ids2) == 4 and [m.by_id[o].path for o in objs2] == ["debris\\piece"]
    models = fx.object_models(m, objs2)
    assert models == {r.tid("mode", "debris\\piece")} and fx.object_models(m, objs) == set()


def check_collect(r, m) -> fx.FxSet:
    s = fx.collect(m)
    assert s.problems == ["effects\\dust: sequence 2 has neither sprites nor images"], s.problems   # the empty sequence
    assert [e.tag_id for e in s.effects] == sorted(e.tag_id for e in s.effects)
    fire = next(e for e in s.effects if e.path.endswith("fire"))
    assert fire.flags == 2 and fire.locations == ["", "muzzle"] and fire.loop_start == -1 and len(fire.events) == 2
    e0, e1 = fire.events
    assert [p.kind for p in e0.parts] == [fx.K_SOUND, fx.K_LIGHT] and [p.location for p in e0.parts] == [0, 1]
    assert e0.parts[1].create_in == 0 and e0.parts[1].violence == 1 and e0.parts[0].velocity == (1.0, 1.0)
    c = e0.emitters[0]
    assert c.location == 1 and c.count == (1, 3) and c.distribution == 2 and c.flags == 1 and c.create == 0
    assert c.yaw == 0.5 and c.pitch == 0.25 and c.offset == (0.10000000149011612, 0.20000000298023224, 0.30000001192092896)
    assert c.angvel == (-1.0, 1.0)                       # given (1, -1): bounds are normalised
    assert c.tint_lo == (255, 128, 64, 255) and c.tint_hi == (255, 255, 255, 255)
    assert e1.skip == 0.25 and e1.delay == (0.20000000298023224, 0.5) and e1.duration == (1.0, 2.0)   # delay was (0.5, 0.2)
    assert [p.kind for p in e1.parts] == [fx.K_DAMAGE, fx.K_PCTL] and e1.parts[0].create_in == 1
    assert s.stats["swapped bounds"] >= 3
    imp = next(e for e in s.effects if e.path.endswith("impact"))
    assert imp.damage_radius == 4.5 and imp.loop_start == 1
    ev = imp.events[0]
    assert ev.parts[0].kind == fx.K_DECAL and ev.parts[0].flags == 1 and ev.parts[0].radius_mod == (0.25, 0.25)
    assert ev.parts[1].location == 0xFFFF and ev.emitters[0].create_in == 1 and ev.emitters[0].angvel == (-6.0, 6.0)
    # particles
    dust = next(p for p in s.particles if p.path.endswith("dust"))
    assert dust.flags == 0x208 and dust.lifespan == (0.30000001192092896, 0.5) and dust.blend == 3 and dust.fade_mode == 1
    assert dust.collision_kind == fx.K_SOUND and dust.death_kind == fx.K_EFFECT and dust.death_effect == r.tid("effe", "weapons\\ar\\splash")
    assert (dust.seq_first, dust.seq_initial, dust.seq_loop, dust.seq_final) == (0, 1, 0, 0) and dust.orientation == 1
    assert dust.radius_anim == (1.0, 2.0) and dust.anim_rate == (30.0, 30.0) and dust.min_size == 4.0
    spark = next(p for p in s.particles if p.path.endswith("spark"))
    assert spark.physics_tag == NULL and spark.collision_kind == fx.K_NONE and spark.blend == 0
    ph = s.physics[0]
    assert ph.flags == 3 and ph.density == f32(2.4) and ph.air_gravity == -0.9990000128746033 and ph.elasticity == 0.3499999940395355
    assert [d.tag_id for d in s.decals] == sorted(d.tag_id for d in s.decals)
    hole = next(d for d in s.decals if d.path.endswith("hole"))
    assert hole.next_tag == r.tid("deca", "effects\\hole2") and hole.blend == 2 and hole.flags == 8 and hole.type == 2
    assert hole.radius == (0.10000000149011612, 0.15000000596046448) and hole.lifetime == (180.0, 180.0) and hole.max_extent == 16.0
    assert (hole.loop_frame, hole.anim_speed) == (2, 3) and hole.color_lo == (0.25, 0.5, 0.75) and hole.color_hi == (1.0, 0.5, 0.0)
    assert len(s.decals) == 2                                       # the chain pulled in hole2
    L = s.lights[0]
    assert L.flags == 0x21 and L.radius == 2.0 and abs(L.cutoff_angle - math.pi) < 1e-6 and L.color_hi[3] == 0.6899999976158142
    assert L.duration == 30.0 and L.falloff_fn == 3
    pc = s.pctl[0]
    assert pc.update_physics == 1 and len(pc.types) == 1 and pc.physics_tag == r.tid("pphy", "effects\\dust")
    t = pc.types[0]
    assert t.name == "fire" and t.initial_count == 12 and t.radius == 1.5 and t.creation_physics == 1 and t.consts == [0.10000000149011612, 0.20000000298023224]
    assert [x.name for x in t.states] == ["birth", "death"] and [x.name for x in t.pstates] == ["fire", "smoke"]
    assert t.states[0].creation_rate == 15.0 and t.pstates[0].blend == 3 and t.pstates[1].sequence == 1
    assert t.pstates[0].color1 == (1.0, 1.0, f32(0.92), f32(0.64))
    # bitmaps: the sprite sheet keeps its sprites, the 2D bitmap lists its two images as sprites
    bd = next(b for b in s.bitmaps if b.path.endswith("dust"))
    assert bd.kind == 3 and bd.images == 2 and [len(q) for q in bd.sequences] == [3, 1, 0]
    assert bd.sequences[0][1].image == 0 and bd.sequences[0][1].left == 0.5 and bd.sequences[0][1].reg == (0.10000000149011612, -0.10000000149011612)
    hb = next(b for b in s.bitmaps if b.path.endswith("hole"))
    assert hb.kind == 0 and [[(sp.image, sp.left, sp.right) for sp in q] for q in hb.sequences] == [[(0, 0.0, 1.0), (1, 0.0, 1.0)]]
    assert s.objects == []
    sa = fx.collect(m, True)
    assert len(sa.effects) == 4 and sa.objects == [r.tid("garb", "debris\\piece")]
    boom = next(e for e in sa.effects if e.path.endswith("boom"))
    assert boom.events[0].parts[0].kind == fx.K_OBJECT and boom.events[0].parts[0].group == "garb"
    return s


def check_corrupt_map(r, m):
    def run(mutate):
        r2, m2 = build_synthetic()
        mutate(r2, fx.Region(m2))
        idx = tagindex.TagIndex.parse_region(bytes(r2.region), 0x1000)
        m3 = FakeMap(Path("synthetic.map"), None, idx, bytes(r2.region), {e.tag_id: e for e in idx.entries})  # type: ignore[arg-type]
        m3.pix = m2.pix
        return fx.collect(m3)

    def ev_off(r2, rg, path, ev=0):
        n, eo = rg.reflexive(r2.block("effe", path) + 0x34, 68, 256)
        return eo + ev * 68

    def part_off(r2, rg, path, ev=0, k=0):
        n, po = rg.reflexive(ev_off(r2, rg, path, ev) + 0x2C, 104, 256)
        return po + k * 104

    def emit_off(r2, rg, path, ev=0, k=0):
        n, co = rg.reflexive(ev_off(r2, rg, path, ev) + 0x38, 232, 256)
        return co + k * 232

    FIRE = "weapons\\ar\\fire"
    IMPACT = "weapons\\ar\\impact"
    eb = lambda r2: r2.block("effe", FIRE)  # noqa: E731
    expect(lambda: run(lambda r2, rg: r2.poke(eb(r2) + 0x34, "<II", 500, 0x80000000 + 0x7FFF0000)))    # events outside the region
    expect(lambda: run(lambda r2, rg: r2.poke(eb(r2) + 0x34, "<II", 5000, BASE + 0x1000)))              # too many events
    expect(lambda: run(lambda r2, rg: r2.poke(eb(r2) + 0x28, "<II", 3, 1)))                             # pointer below the base
    pb = lambda r2: r2.block("part", "effects\\dust")  # noqa: E731
    expect(lambda: run(lambda r2, rg: r2.poke(pb(r2) + 0xDA, "<H", 9)))                                  # blend function 9
    expect(lambda: run(lambda r2, rg: r2.poke(pb(r2) + 0xAC, "<H", 3)))                                  # orientation 3
    expect(lambda: run(lambda r2, rg: r2.poke(pb(r2) + 0x98, "<h", -1)))                                 # negative sequence
    expect(lambda: run(lambda r2, rg: r2.poke(r2.block("bitm", "effects\\dust") + 0x54, "<II", 2, 0x7FFFFFFF)))
    expect(lambda: run(lambda r2, rg: r2.poke(r2.block("deca", "effects\\hole") + 0x02, "<H", 7)))     # decal type 7
    expect(lambda: run(lambda r2, rg: r2.poke(part_off(r2, rg, FIRE), "<H", 9)))                         # create in 9
    expect(lambda: run(lambda r2, rg: r2.poke(emit_off(r2, rg, FIRE) + 0x68, "<H", 6)))                  # distribution function 6
    expect(lambda: run(lambda r2, rg: r2.poke(emit_off(r2, rg, FIRE) + 0x6C, "<2h", -1, 3)))             # negative count
    expect(lambda: run(lambda r2, rg: r2.poke(r2.block("pctl", "effects\\boom") + 0x5C, "<II", 9, BASE + 0x1000)))   # 9 types
    # an emitter that names a decal instead of a particle is dropped with a problem note
    def wrong_group(r2, rg):
        r2.set_ref(emit_off(r2, rg, FIRE) + 0x54, "deca", r2.tid("deca", "effects\\hole"))
    s = run(wrong_group)
    assert any("not a particle tag" in p for p in s.problems)
    assert len(next(e for e in s.effects if e.path == FIRE).events[0].emitters) == 0
    assert s.stats["particles with a dangling reference (dropped)"] == 1
    # a part whose reference names a tag id nobody has is dropped
    def dangling(r2, rg):
        r2.set_ref(part_off(r2, rg, IMPACT, 0, 1) + 0x18, "snd!", 0xE0000001)
    s = run(dangling)
    imp = next(e for e in s.effects if e.path == IMPACT)
    assert [p.kind for p in imp.events[0].parts] == [fx.K_DECAL] and s.stats["parts with a dangling reference (dropped)"] == 1
    # a reference whose group differs from the tag it names is reported and nulled where a group is required
    s = run(lambda r2, rg: r2.set_ref(pb(r2) + 0x04, "pphy", r2.tid("bitm", "effects\\dust")))
    assert any("bitmap" in p for p in s.problems) and next(p for p in s.particles if p.path.endswith("dust")).bitmap_tag == NULL
    # NaN float is replaced by 0 and reported
    s = run(lambda r2, rg: r2.poke(r2.block("ligh", "weapons\\ar\\muzzle") + 4, "<I", 0x7FC00000))
    assert any("non-finite" in p for p in s.problems) and s.lights[0].radius == 0.0


def check_textures(m, s):
    tx = fx.build_textures(m, s)
    v = texconv.parse_texs(tx.blob)
    # dust: sheet image 0 (16x16) and 1 (32x16); hole: 2D images 0, 1 -> 4 textures
    assert len(tx.textures) == 4 and sorted(tx.tex_index) == sorted(
        [(b.tag_id, i) for b in s.bitmaps for i in range(b.images)])
    dust = next(b for b in s.bitmaps if b.path.endswith("dust"))
    t0 = v.textures[tx.tex_index[(dust.tag_id, 0)]]
    assert (t0["width"], t0["height"], t0["mips"]) == (16, 16, 1)                   # sprite sheets: one mip
    assert t0["flags"] & texconv.TF_BLEND                                           # the pixels carry alpha
    t1 = v.textures[tx.tex_index[(dust.tag_id, 1)]]
    assert (t1["width"], t1["height"]) == (32, 16)
    hole = next(b for b in s.bitmaps if b.path.endswith("hole"))
    th = v.textures[tx.tex_index[(hole.tag_id, 0)]]
    assert th["mips"] == len(texconv.mip_dims(16, 16)) == 2                          # plain 2D bitmaps keep their mips
    # bindings: keyed by bitmap tag id, first converted image, blend of the first user
    assert [b["shader_id"] for b in v.bindings] == sorted(b.tag_id for b in s.bitmaps)
    bd = {b["shader_id"]: b for b in v.bindings}
    assert bd[dust.tag_id]["texture"] == tx.tex_index[(dust.tag_id, 0)] and bd[dust.tag_id]["blend"] == texconv.BLEND_ADD
    assert bd[hole.tag_id]["blend"] in (texconv.BLEND_MULTIPLY, texconv.BLEND_DOUBLE_MULTIPLY)
    # budget: a tiny budget lowers the size cap
    small = fx.build_textures(m, s, 1)
    assert small.cap < tx.cap or tx.cap == 32
    # a 256x256 sheet is halved once (128), a 64 sheet stays
    assert fx.fx_target(256, 256, 128) == (128, 128) and fx.fx_target(64, 64, 128) == (64, 64)
    assert fx.fx_target(256, 128, 64) == (64, 32) and fx.fx_target(32, 32, 128) == (32, 32) and fx.fx_target(16, 8, 128) == (16, 8)
    return tx


def check_serialisation(s, tx) -> bytes:
    blob = fx.build(s, tx.tex_index)
    assert len(blob) % 16 == 0
    sec = fx.parse_section(blob)
    t = sec.tables
    assert len(t["effects"]) == 3 and len(t["events"]) == 4 and len(t["parts"]) == 7 and len(t["emitters"]) == 2
    assert [len(t[n]) for n in ("particles", "physics", "decals", "lights", "pctl", "bitmaps")] == [2, 1, 2, 1, 1, 2]
    assert len(t["locations"]) == 3 and len(t["sequences"]) == 4 and len(t["sprites"]) == 3 + 1 + 0 + 2
    # sprites carry the converted textures; the first image of a bitmap is the bitmap record's texture
    sp = t["sprites"]
    assert all(x[0] != fx.NONE16 for x in sp)
    # a section built without textures marks every sprite unconverted
    sec0 = fx.parse_section(fx.build(s))
    assert all(x[0] == fx.NONE16 for x in sec0.tables["sprites"])
    # empty set
    empty = fx.parse_section(fx.build(fx.FxSet([], [], [], [], [], [], [], [])))
    assert all(len(v) == 0 for v in empty.tables.values())
    # reference parser rejects every truncation, bad signature and version
    for n in range(0, len(blob), 7):
        expect(lambda n=n: fx.parse_section(blob[:n]))
    expect(lambda: fx.parse_section(b"XXXX" + blob[4:]))
    expect(lambda: fx.parse_section(blob[:4] + struct.pack("<H", 2) + blob[6:]))
    # writer refuses unsorted definitions and sprites of unknown bitmaps
    bad = fx.FxSet(s.effects, list(reversed(s.particles)), s.physics, s.decals, s.lights, s.pctl, s.bitmaps, [])
    expect(lambda: fx.build(bad, tx.tex_index))
    # byte flips anywhere: the reference reader either rejects or returns a section (never another exception)
    rng = random.Random(7)
    for _ in range(400):
        at = rng.randrange(8, len(blob))
        c = bytearray(blob)
        c[at] ^= rng.choice((0xFF, 0x01, 0x80))
        try:
            fx.parse_section(bytes(c))
        except fx.FxError:
            pass
    return blob


def check_real_maps() -> None:
    path = ROOT / "assets" / "decompressed" / "a10.map"
    if not path.exists():
        print("  (a10.map not present: real-map checks skipped)")
        return
    pack, s, tx, m = fx.convert_map(path)
    pk = hp2.parse(pack)
    sec = fx.parse_section(pk.read("FXD0"))
    v = texconv.parse_texs(pk.read("FXTX"))
    t = sec.tables
    assert [len(t[n]) for n in ("effects", "events", "parts", "emitters", "particles", "decals", "lights", "pctl", "physics",
                                "bitmaps", "sprites")] == [225, 406, 350, 1338, 92, 33, 24, 9, 26, 76, 776]
    assert len(v.textures) == 114 and tx.bytes <= fx.FX_DEFAULT_BUDGET and len(v.bindings) == 76
    assert all("location 1 but the effect has 1 locations" in p for p in s.problems)          # the one retail oddity
    assert s.stats["swapped bounds"] > 0 and not s.objects
    ids = {e.tag_id for e in s.effects}
    assert any(e.path.endswith("assault rifle\\effects\\fire bullet") for e in s.effects)
    # every particle's collision / death effect that is an effect is converted
    for p in s.particles:
        for k, tid in ((p.collision_kind, p.collision_effect), (p.death_kind, p.death_effect)):
            assert k != fx.K_EFFECT or tid in ids
    # object attachments (FXAT): the steam jets scenery keeps its particle system, every row points at a converted tag
    rows = fx.build_attachments(s)
    assert rows[:4] == b"FXAT" and struct.unpack_from("<II", rows, 4)[0] == 2
    n = struct.unpack_from("<I", rows, 8)[0]
    assert n > 13 and len(rows) == 16 + 32 * n and struct.unpack_from('<I', rows, 12)[0] != 0xFFFFFFFF
    have = {p.tag_id for p in s.pctl} | ids | {p.tag_id for p in s.lights}
    assert all(struct.unpack_from("<I", rows, 16 + 32 * i + 4)[0] in have for i in range(n))
    assert any(r[1] == fx.ATT_PCTL and r[3] == "smoker" for r in s.attach)
    # every converted image of every sprite has a texture, every bitmap has a binding
    assert all(sp[0] != fx.NONE16 for sp in t["sprites"])
    assert {b["shader_id"] for b in v.bindings} == {b.tag_id for b in s.bitmaps}
    # the whole-map set is a superset and creates objects, whose models are exported
    sa = fx.collect(m, True)
    assert len(sa.effects) == 255 and {e.tag_id for e in s.effects} <= {e.tag_id for e in sa.effects} and len(sa.objects) == 4
    assert fx.referenced_models(path, False) == set() and len(fx.referenced_models(path)) == 3


def main() -> None:
    r, m = build_synthetic()
    check_closure(r, m)
    s = check_collect(r, m)
    check_corrupt_map(r, m)
    tx = check_textures(m, s)
    blob = check_serialisation(s, tx)
    check_real_maps()
    pack = hp2.build("test", [("FXD0", blob), ("FXD1", fx.build(fx.FxSet([], [], [], [], [], [], [], []))), ("FXTX", tx.blob)])
    got = hp2.parse(pack)
    assert fx.parse_section(got.read("FXD0")).tables["effects"][0][0] == min(e.tag_id for e in s.effects)
    assert fx.parse_section(got.read("FXD1")).tables["effects"] == []
    if os.environ.get("FXDEF_FIXTURE"):
        Path(os.environ["FXDEF_FIXTURE"]).write_bytes(pack)
    print("ok")


if __name__ == "__main__":
    main()
