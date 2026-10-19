"""UNIT section on a synthetic map (no game data needed): globals (player information, control, falling damage,
grenades, difficulty, materials), bipeds with vitals from their collision models, the animation-derived speeds and
hold markers, vehicles with seats, the roots exported to the object pack, the writer and its reference parser,
corrupt maps. Set UNIT_FIXTURE=<path> to also write the .hp2 the C reader test (engine/tests/test_unitdef.c, which
hard-codes the numbers of this world) opens. A skip-if-missing check converts the real campaign maps."""
import math
import os
import struct
import sys
from pathlib import Path

sys.path[:0] = [str(Path(__file__).resolve().parents[1]), str(Path(__file__).resolve().parent)]
from halo2ps2 import hp2, scenconv, tagindex, texconv, unitconv as uc  # noqa: E402

BASE = tagindex.XBOX_TAG_BASE
ROOT = Path(__file__).resolve().parents[2]
GRAVITY = 0.0035651791


class Region:
    """Tag region builder: tags with zero-filled data blocks, then pokes (offsets are region offsets)."""

    def __init__(self, tags):
        self.region = bytearray(tagindex.TagIndex.build(tags, scenario_index=0))
        self.index = tagindex.TagIndex.parse_region(bytes(self.region), 0x1000)
        self.by_key = {(e.group, e.path): e for e in self.index.entries}
        self.cursor = {}

    def e(self, group, path):
        return self.by_key[(group, path)]

    def block(self, group, path):
        return self.e(group, path).data_address - BASE

    def tid(self, group, path):
        return self.e(group, path).tag_id

    def alloc(self, group, path, data: bytes) -> int:
        e = self.e(group, path)
        cur = (self.cursor.get((group, path), 0x1000) + 3) & ~3
        off = e.data_address - BASE + cur
        self.region[off:off + len(data)] = data
        self.cursor[(group, path)] = cur + len(data)
        return BASE + off

    def poke(self, off, fmt, *vals):
        struct.pack_into(fmt, self.region, off, *vals)

    def refl(self, off, count, addr):
        struct.pack_into("<III", self.region, off, count, addr, 0)

    def ref(self, group, path):
        if path is None:
            return struct.pack("<4sIII", group[::-1].encode(), 0, 0, 0xFFFFFFFF)
        return struct.pack("<4sIII", group[::-1].encode(), 0, 0, self.tid(group, path))

    def setref(self, off, group, path):
        self.region[off:off + 16] = self.ref(group, path)

    def map(self):
        idx = tagindex.TagIndex.parse_region(bytes(self.region), 0x1000)
        return texconv.MapData(Path("synthetic.map"), None, idx, bytes(self.region),  # type: ignore[arg-type]
                               {e.tag_id: e for e in idx.entries})


CY, MA, WH, CH = ("characters\\cyborg\\cyborg", "characters\\marine\\marine", "vehicles\\warthog\\warthog",
                  "levels\\t\\chair")
TAGS = [
    ("scnr", "levels\\t\\t", 0x1000), ("matg", "globals\\globals", 0x6000),
    ("bipd", CY, 0x6000), ("bipd", MA, 0x6000), ("vehi", WH, 0x6000), ("vehi", CH, 0x6000),
    ("mode", CY, 0x1000), ("mode", MA, 0x1000), ("mode", WH, 0x1000), ("antr", CY, 0x6000), ("antr", WH, 0x1000),
    ("antr", "levels\\t\\chairgraph", 0x1000),
    ("coll", CY, 0x2000), ("coll", MA, 0x2000), ("coll", WH, 0x2000), ("phys", WH, 0x1000), ("phys", CY, 0x1000),
    ("foot", "globals\\footsteps", 0x1000), ("weap", "weapons\\pistol\\pistol", 0x1000),
    ("weap", "weapons\\assault rifle\\assault rifle", 0x1000), ("weap", "vehicles\\warthog\\chaingun", 0x1000),
    ("udlg", "sound\\dialog\\chief", 0x1000), ("udlg", "sound\\dialog\\marines\\a", 0x1000),
    ("udlg", "sound\\dialog\\marines\\b", 0x1000), ("trak", "camera\\biped", 0x1000), ("trak", "camera\\default", 0x1000),
    ("snd!", "sound\\water_in", 0x1000), ("snd!", "sound\\water_out", 0x1000), ("snd!", "sound\\melee_dirt", 0x1000),
    ("snd!", "sound\\melee_glass", 0x1000), ("snd!", "sound\\glass_medium", 0x1000),
    ("effe", "effects\\coop", 0x1000), ("part", "effects\\glass part", 0x1000),
    ("jpt!", "dmg\\falling", 0x1000), ("jpt!", "dmg\\distance", 0x1000), ("jpt!", "dmg\\venv", 0x1000),
    ("jpt!", "dmg\\vkill", 0x1000), ("jpt!", "dmg\\vcoll", 0x1000), ("jpt!", "dmg\\flame", 0x1000),
    ("jpt!", "dmg\\melee", 0x1000),
    ("grhi", "ui\\frag", 0x1000), ("grhi", "ui\\plasma", 0x1000), ("eqip", "weapons\\frag", 0x1000),
    ("eqip", "weapons\\plasma", 0x1000), ("proj", "weapons\\frag\\proj", 0x1000),
    ("proj", "weapons\\plasma\\proj", 0x1000), ("effe", "effects\\throw", 0x1000),
    ("effe", "effects\\throw2", 0x1000),
]

# expected numbers of the synthetic world (engine/tests/test_unitdef.c uses the same ones)
EXPECT = {"gravity": GRAVITY}


def put_f(r, b, off, *vals):
    r.poke(b + off, f"<{len(vals)}f", *vals)


def shield_coll(r, path, flags, body, shield, mat, stun, rech, body_mat=1):
    cb = r.block("coll", path)
    r.poke(cb, "<I", flags)
    r.poke(cb + 4, "<H", body_mat)
    put_f(r, cb, 0x08, body)
    put_f(r, cb, 0xCC, shield)
    r.poke(cb + 0xD2, "<H", mat)
    put_f(r, cb, 0x10C, stun, rech)
    put_f(r, cb, 0x1C0, 1.0 / (rech * 30) if rech else 1.0)


def write_graph(r, path, hold, aux):
    """Cyborg-like graph: one `stand` unit with a `rifle` weapon block (hold / aux markers) and move animations
    carrying root motion (frame info type 1: dx dy per frame)."""
    e = r.e("antr", path)
    b = e.data_address - BASE
    anims = [
        ("stand rifle move-front", 20, lambda f: (0.075, 0.0)),
        ("stand rifle move-back", 20, lambda f: (-0.067, 0.0)),
        ("stand rifle move-left", 20, lambda f: (0.0, 0.067)),
        ("stand rifle move-right", 20, lambda f: (0.0, -0.067)),
        ("crouch rifle move-front", 30, lambda f: (0.03, 0.0)),
        ("stand pistol move-front%1", 20, lambda f: (0.5, 0.0)),        # not preferred over `rifle`
        ("stand rifle idle", 10, None),
    ]
    blob = bytearray()
    for name, fc, fn in anims:
        a = bytearray(180)
        a[:len(name)] = name.encode()
        struct.pack_into("<hhh", a, 0x22, fc, 0, 1 if fn else 0)
        if fn:
            data = b"".join(struct.pack("<2f", *fn(f)) for f in range(fc))
            struct.pack_into("<5I", a, 0x48, len(data), 0, 0, r.alloc("antr", path, data), 0xFFFFFFFF)
        blob += a
    r.refl(b + 0x74, len(anims), r.alloc("antr", path, bytes(blob)))
    w = bytearray(188)
    w[:5] = b"rifle"
    w[0x20:0x20 + len(aux)] = aux.encode()
    w[0x40:0x40 + len(hold)] = hold.encode()
    wb = r.alloc("antr", path, bytes(w))
    u = bytearray(100)
    u[:5] = b"stand"
    struct.pack_into("<III", u, 0x58, 1, wb, 0)
    r.refl(b + 0x0C, 1, r.alloc("antr", path, bytes(u)))


def write_unit(r, group, path, otype, model, anim, coll, phys=None):
    b = r.block(group, path)
    r.poke(b, "<H", otype)
    r.poke(b + 2, "<H", 2)
    put_f(r, b, 4, 0.45)
    put_f(r, b, 8, -0.02, 0.0, 0.1)
    put_f(r, b, 0x20, 0.7)
    r.setref(b + 0x28, "mode", model)
    r.setref(b + 0x38, "antr", anim)
    r.setref(b + 0x70, "coll", coll)
    r.setref(b + 0x80, "phys", phys)
    put_f(r, b, 0x104, 0.45)
    r.poke(b + 0x17C, "<I", 0x00448008)
    r.poke(b + 0x180, "<H", 1)
    put_f(r, b, 0x1A0, 1.2217)
    track = r.alloc(group, path, r.ref("trak", "camera\\biped"))
    r.refl(b + 0x1F4, 1, track)
    return b


def build_synthetic():
    r = Region(TAGS)
    gb = r.block("matg", "globals\\globals")
    G = ("matg", "globals\\globals")

    def alloc(data):
        return r.alloc(*G, data)

    # sound refs, camera
    r.refl(gb + uc.G_SOUNDS, 2, alloc(r.ref("snd!", "sound\\water_in") + r.ref("snd!", "sound\\water_out")))
    r.refl(gb + uc.G_CAMERA, 1, alloc(r.ref("trak", "camera\\default")))
    # player information: element 0xD0 bytes (reference at 0, floats, coop effect at 0xB8)
    pi = bytearray(0xE0)
    pi[:16] = r.ref("bipd", CY)
    pi[0xB8:0xC8] = r.ref("effe", "effects\\coop")
    for at, v in ((0x2C, 0.512), (0x30, 1.25), (0x34, 2.25), (0x38, 2.0), (0x3C, 2.0), (0x40, 0.32), (0x44, 0.9),
                  (0x48, 0.65), (0x4C, 0.6), (0x50, 0.16), (0x54, 0.035), (0x6C, 0.05), (0x80, 0.8), (0x84, 0.8),
                  (0x88, 1.0), (0x8C, 0.0), (0x90, 1.25), (0x9C, 10.0), (0xA0, 15.0), (0xA4, 0.25)):
        struct.pack_into("<f", pi, at, v)
    r.refl(gb + uc.G_PLAYER, 1, alloc(bytes(pi)))
    # player control (128 B) + look function
    pc = bytearray(128)
    for at, v in ((0, 0.6), (4, 0.7), (8, 0.5), (0x40, 1.0), (0x44, 3.0), (0x48, 0.85), (0x4C, 60.0), (0x50, 120.0),
                  (0x54, 0.5), (0x70, 0.7853982)):
        struct.pack_into("<f", pc, at, v)
    struct.pack_into("<HH", pc, 0x6C, 7, 15)
    look = struct.pack("<6f", 0.0, 0.05, 0.1, 0.25, 0.58, 1.0)
    struct.pack_into("<III", pc, 0x74, 6, alloc(look), 0)
    r.refl(gb + uc.G_CONTROL, 1, alloc(bytes(pc)))
    # falling damage (152 B)
    fd = bytearray(152)
    struct.pack_into("<2f", fd, 8, 3.0, 6.0)
    struct.pack_into("<f", fd, 0x28, 16.5)
    for at, p in ((0x10, "falling"), (0x2C, "distance"), (0x3C, "venv"), (0x4C, "vkill"), (0x5C, "vcoll"),
                  (0x6C, "flame")):
        fd[at:at + 16] = r.ref("jpt!", "dmg\\" + p)
    struct.pack_into("<3f", fd, 0x8C, math.sqrt(2 * GRAVITY * 16.5), math.sqrt(2 * GRAVITY * 3.0),
                     math.sqrt(2 * GRAVITY * 6.0))
    r.refl(gb + uc.G_FALLING, 1, alloc(bytes(fd)))
    # grenades (68 B x 2)
    gr = b""
    for i, (nm, hud, eq, pr, fx) in enumerate((("frag", "ui\\frag", "weapons\\frag", "weapons\\frag\\proj", "effects\\throw"),
                                              ("plasma", "ui\\plasma", "weapons\\plasma", "weapons\\plasma\\proj",
                                               "effects\\throw2"))):
        e = bytearray(68)
        struct.pack_into("<HH", e, 0, 4, 0)
        e[4:20] = r.ref("effe", fx)
        e[0x14:0x24] = r.ref("grhi", hud)
        e[0x24:0x34] = r.ref("eqip", eq)
        e[0x34:0x44] = r.ref("proj", pr)
        gr += e
    r.refl(gb + uc.G_GRENADES, 2, alloc(gr))
    # difficulty: group g, level l = 100 g + l (distinct numbers) / 100
    diff = bytearray(644)
    for g in range(31):
        for lv in range(4):
            struct.pack_into("<f", diff, 16 * g + 4 * lv, g + lv / 10.0)
    r.refl(gb + uc.G_DIFFICULTY, 1, alloc(bytes(diff)))
    # materials: 3 (884 B): dirt, glass (with particle effect + sweetener), ice (param_a)
    mats = bytearray(884 * 3)
    for i in range(3):
        el = 884 * i
        mats[el + 0x2E4:el + 0x2F4] = r.ref("effe", None)
        mats[el + 0x2F4:el + 0x304] = r.ref("snd!", None)
        mats[el + 0x364:el + 0x374] = r.ref("snd!", "sound\\melee_dirt" if i != 1 else "sound\\melee_glass")
    mats[884 + 0x2F4:884 + 0x304] = r.ref("snd!", "sound\\glass_medium")
    struct.pack_into("<f", mats, 884 + 0x2D4, 10.0)
    struct.pack_into("<f", mats, 884 * 2 + 0x94, 0.125)
    part = bytearray(0x84)
    part[:16] = r.ref("part", "effects\\glass part")
    struct.pack_into("<II", mats, 884 + 0x31C, 1, alloc(bytes(part)))
    r.refl(gb + uc.G_MATERIALS, 3, alloc(bytes(mats)))

    # coll models
    shield_coll(r, CY, 7, 75.0, 75.0, 22, 6.0, 4.0)
    shield_coll(r, MA, 3, 50.0, 50.0, 23, 8.0, 1.0)
    shield_coll(r, WH, 0, 0.0, 0.0, 0, 0.0, 0.0, 0xFFFF)

    # cyborg biped
    b = write_unit(r, "bipd", CY, 0, "mode" and CY, CY, CY, CY)
    r.setref(b + 0x28, "mode", CY)
    r.setref(b + 0x70, "coll", CY)
    r.setref(b + 0x80, "phys", CY)
    wl = b"".join(r.ref("weap", p).ljust(uc.WEAPON_REC, b"\0") for p in
                  ("weapons\\assault rifle\\assault rifle", "weapons\\pistol\\pistol"))
    r.refl(b + 0x2D8, 2, r.alloc("bipd", CY, wl))
    dl = bytearray(24)
    dl[8:24] = r.ref("udlg", "sound\\dialog\\chief")
    r.refl(b + 0x2B4, 1, r.alloc("bipd", CY, bytes(dl)))
    r.setref(b + 0x288, "jpt!", "dmg\\melee")
    put_f(r, b, 0x264, 0.0, 0.0, 0.0, 0.0, 0.0)
    put_f(r, b, 0x2C0, 10.0)
    r.poke(b + 0x2C4, "<HH", 0, 4)
    put_f(r, b, 0x2F0, 5.8939)
    r.poke(b + 0x2F4, "<I", 0x202)
    put_f(r, b, 0x2F8, 1.3963)
    put_f(r, b, 0x358, 0.7854, 0.34907, 0.7854, 1.25, 0.34907, 0.7854, 0.65)
    r.setref(b + 0x38C, "foot", "globals\\footsteps")
    put_f(r, b, 0x3B4, 0.07)
    put_f(r, b, 0x3D4, 0.6, 1.0, 1.5, 5.0, 8.0, 0.0)
    put_f(r, b, 0x400, 0.62, 0.35, 0.2)
    put_f(r, b, 0x424, 0.7, 0.5, 0.2)
    r.poke(b + 0x4E4, "<HH", 0, 12)
    contact = bytearray(128)
    contact[0x20:0x29] = b"left foot"
    contact[0x40 + 0x20:0x40 + 0x2A] = b"right foot"
    r.refl(b + 0x4E8, 2, r.alloc("bipd", CY, bytes(contact)))
    write_graph(r, CY, "right hand", "left hand gun grip")

    # marine biped (AI): no weapons, no camera track
    m = r.block("bipd", MA)
    r.poke(m, "<H", 0)
    put_f(r, m, 4, 0.4)
    r.setref(m + 0x28, "mode", MA)
    r.setref(m + 0x38, "antr", CY)             # shares the cyborg graph in this world
    r.setref(m + 0x70, "coll", MA)
    r.setref(m + 0x80, "phys", None)
    put_f(r, m, 0x1A0, 1.2217)
    put_f(r, m, 0x400, 0.62, 0.44, 0.2)
    put_f(r, m, 0x424, 0.64, 0.5, 0.18)
    r.poke(m + 0x4E4, "<HH", 0, 12)
    dl = bytearray(24 * 2)
    dl[8:24] = r.ref("udlg", "sound\\dialog\\marines\\a")
    struct.pack_into("<H", dl, 24, 100)
    dl[24 + 8:48] = r.ref("udlg", "sound\\dialog\\marines\\b")
    r.refl(m + 0x2B4, 2, r.alloc("bipd", MA, bytes(dl)))

    # warthog: jeep, 2 seats, one weapon
    w = write_unit(r, "vehi", WH, 1, WH, WH, WH, WH)
    r.poke(w + 0x2F0, "<I", 0xCD1)
    r.poke(w + 0x2F4, "<H", 1)
    put_f(r, w, 0x2F8, 0.255, 0.1, 0.00275, 0.011, 30.0, -30.0, 1.0, 180.0)
    r.refl(w + 0x2D8, 1, r.alloc("vehi", WH, r.ref("weap", "vehicles\\warthog\\chaingun").ljust(uc.WEAPON_REC, b"\0")))
    seats = bytearray(uc.SEAT_REC * 2)
    for i, (flags, label, marker, cam, lo, hi) in enumerate(((0x14, "W-driver", "driver", "camera driver", -0.5236, 0.2618),
                                                           (0x198, "W-gunner", "gunner", "camera gunner", -0.349, 0.6109))):
        s = uc.SEAT_REC * i
        struct.pack_into("<I", seats, s, flags)
        seats[s + 4:s + 4 + len(label)] = label.encode()
        seats[s + 0x24:s + 0x24 + len(marker)] = marker.encode()
        seats[s + 0x84:s + 0x84 + len(cam)] = cam.encode()
        struct.pack_into("<2f", seats, s + 0xC8, lo, hi)
    r.refl(w + 0x2E4, 2, r.alloc("vehi", WH, bytes(seats)))
    write_graph(r, WH, "", "")

    # chair: a turret-type `vehicle` (device) with one seat and no speed
    c = write_unit(r, "vehi", CH, 1, WH, "levels\\t\\chairgraph", WH)
    r.setref(c + 0x28, "mode", None)
    r.setref(c + 0x38, "antr", None)
    r.setref(c + 0x70, "coll", None)
    r.poke(c + 0x2F4, "<H", 6)
    seat = bytearray(uc.SEAT_REC)
    seat[4:17] = b"pchair-driver"
    seat[0x24:0x28] = b"seat"
    r.refl(c + 0x2E4, 1, r.alloc("vehi", CH, bytes(seat)))
    return r, r.map()


def near(a, b, tol=1e-5):
    return abs(a - b) <= tol


def check_collect():
    r, m = build_synthetic()
    us = uc.collect_map(m)
    assert not us.problems, us.problems
    g = us.globals.vals
    assert g["default_biped_tag"] == r.tid("bipd", CY) and g["coop_effect_tag"] == r.tid("effe", "effects\\coop")
    assert g["camera_track_tag"] == r.tid("trak", "camera\\default")
    assert g["water_enter_sound"] == r.tid("snd!", "sound\\water_in") and g["water_exit_sound"] == r.tid("snd!", "sound\\water_out")
    assert near(g["walk_speed"], 1.25) and near(g["run_forward"], 2.25) and near(g["run_backward"], 2.0)
    assert near(g["sneak_forward"], 0.9) and near(g["airborne_accel"], 0.035) and near(g["pi_unk_2c"], 0.512)
    assert near(g["stun_max_time"], 1.25) and near(g["fp_idle_max"], 15.0) and near(g["fp_skip_fraction"], 0.25)
    assert near(g["look_pitch_rate_deg"], 60.0) and near(g["look_yaw_rate_deg"], 120.0) and near(g["magnetism_adhesion"], 0.7)
    assert g["min_weapon_swap_ticks"] == 7 and g["min_autolevel_ticks"] == 15 and near(g["look_function"][4], 0.58)
    assert near(g["gravity"], GRAVITY, 1e-6) and near(g["max_fall_distance"], 16.5) and near(g["harmful_fall_max"], 6.0)
    assert g["damage_tags"][0] == r.tid("jpt!", "dmg\\falling") and g["damage_tags"][5] == r.tid("jpt!", "dmg\\flame")
    assert near(g["difficulty"][4 * 7 + 2], 7.2) and near(g["difficulty"][30 * 4 + 3], 30.3)
    assert len(us.materials) == 3
    mt = [x.vals for x in us.materials]
    assert mt[0]["melee_sound"] == r.tid("snd!", "sound\\melee_dirt") and mt[0]["particle_count"] == 0
    assert mt[1]["melee_sound"] == r.tid("snd!", "sound\\melee_glass") and mt[1]["impact_sound"] == r.tid("snd!", "sound\\glass_medium")
    assert mt[1]["particle_tag"] == r.tid("part", "effects\\glass part") and mt[1]["particle_count"] == 1 and near(mt[1]["param_b"], 10.0)
    assert near(mt[2]["param_a"], 0.125) and mt[0]["effect"] == 0

    assert [b.vals["tag_id"] for b in us.bipeds] == sorted(b.vals["tag_id"] for b in us.bipeds)
    cy = us.biped_by_tag(r.tid("bipd", CY)).vals
    assert cy["name"] == "cyborg" and cy["model_tag"] == r.tid("mode", CY) and cy["anim_tag"] == r.tid("antr", CY)
    assert cy["collision_tag"] == r.tid("coll", CY) and cy["physics_tag"] == r.tid("phys", CY)
    assert cy["camera_track_tag"] == r.tid("trak", "camera\\biped") and cy["footsteps_tag"] == r.tid("foot", "globals\\footsteps")
    assert cy["melee_damage_tag"] == r.tid("jpt!", "dmg\\melee")
    assert cy["weapon_count"] == 2 and cy["weapon_tags"][0] == r.tid("weap", "weapons\\assault rifle\\assault rifle")
    assert cy["weapon_tags"][1] == r.tid("weap", "weapons\\pistol\\pistol") and cy["weapon_tags"][2] == 0
    assert cy["dialogue_count"] == 1 and cy["dialogue"][1] == r.tid("udlg", "sound\\dialog\\chief")
    assert near(cy["radius"], 0.2) and near(cy["stand_height"], 0.7) and near(cy["crouch_height"], 0.5)
    assert near(cy["stand_camera"], 0.62) and near(cy["crouch_camera"], 0.35) and near(cy["crouch_time"], 0.2)
    assert near(cy["jump_velocity"], 0.07) and near(cy["accel_scale"], 0.7) and near(cy["camera_fov"], 1.2217)
    assert near(cy["bounding_offset"][0], -0.02) and near(cy["bounding_offset"][2], 0.1)
    assert near(cy["slope"][0], 0.7854) and near(cy["slope"][3], 1.25) and near(cy["landing"][3], 5.0)
    assert near(cy["turn_speed"], 5.8939) and cy["biped_flags"] == 0x202 and cy["head_node"] == 12 and cy["pelvis_node"] == 0
    assert near(cy["health"], 75.0) and near(cy["shield"], 75.0) and near(cy["shield_recharge_time"], 4.0)
    assert near(cy["shield_stun_time"], 6.0) and near(cy["shield_recharge_rate"], 1.0 / 120.0, 1e-6)
    assert cy["shield_material"] == 22 and cy["coll_flags"] == 7 and cy["body_material"] == 1
    assert near(cy["grenade_velocity"], 10.0) and cy["grenade_type"] == 0 and cy["grenade_count"] == 4
    assert cy["foot_markers"][:9] == b"left foot" and cy["foot_markers"][32:42] == b"right foot"
    assert cy["hold_marker"] == "right hand" and cy["aux_marker"] == "left hand gun grip"
    sp = cy["anim_speed"]
    # 20 frames of 0.075 = 1.5 wu in 20 / 30 s = 2.25 wu/s; back 0.067 x 20 / (20/30); crouch 0.03 x 30 = 0.9; the
    # `rifle` weapon wins over `pistol` for the front speed
    assert near(sp[0], 2.25, 1e-4) and near(sp[1], 2.01, 1e-4) and near(sp[2], 2.01, 1e-4) and near(sp[3], 0.9, 1e-4)
    ma = us.biped_by_tag(r.tid("bipd", MA)).vals
    assert ma["health"] == 50.0 and ma["shield_recharge_time"] == 1.0 and ma["weapon_count"] == 0
    assert ma["dialogue_count"] == 2 and ma["dialogue"][0] == 0 and ma["dialogue"][2] == 100 and ma["dialogue"][4] == 0
    assert ma["physics_tag"] == 0 and ma["camera_track_tag"] == 0 and ma["anim_tag"] == r.tid("antr", CY)

    assert len(us.vehicles) == 2 and len(us.seats) == 3
    wh = next(v.vals for v in us.vehicles if v.vals["tag_id"] == r.tid("vehi", WH))
    assert wh["type"] == 1 and near(wh["max_forward"], 0.255) and near(wh["max_reverse"], 0.1) and near(wh["accel"], 0.00275)
    assert near(wh["decel"], 0.011) and near(wh["max_left_turn"], 30.0) and near(wh["max_right_turn"], -30.0)
    assert near(wh["wheel_circumference"], 1.0) and near(wh["turn_rate"], 180.0) and wh["vehicle_flags"] == 0xCD1
    assert wh["weapon_count"] == 1 and wh["weapon_tags"][0] == r.tid("weap", "vehicles\\warthog\\chaingun")
    assert wh["seat_count"] == 2 and wh["model_tag"] == r.tid("mode", WH) and wh["physics_tag"] == r.tid("phys", WH)
    seats = us.seats[wh["first_seat"]:wh["first_seat"] + 2]
    assert [s.vals["label"] for s in seats] == ["W-driver", "W-gunner"] and seats[1].vals["marker"] == "gunner"
    assert seats[0].vals["camera_marker"] == "camera driver" and near(seats[0].vals["pitch_min"], -0.5236)
    assert near(seats[1].vals["pitch_max"], 0.6109) and seats[1].vals["flags"] == 0x198
    ch = next(v.vals for v in us.vehicles if v.vals["tag_id"] == r.tid("vehi", CH))
    assert ch["type"] == 6 and ch["seat_count"] == 1 and ch["model_tag"] == 0
    assert uc.is_driveable(next(v for v in us.vehicles if v.vals["tag_id"] == wh["tag_id"]))
    assert not uc.is_driveable(next(v for v in us.vehicles if v.vals["tag_id"] == ch["tag_id"]))
    return r, m, us


def check_roots(r, us):
    models = uc.models_of(us, True)
    assert models == {r.tid("mode", CY), r.tid("mode", MA), r.tid("mode", WH)}
    assert uc.models_of(us, False) == {r.tid("mode", CY), r.tid("mode", MA)}
    assert uc.anims_of(us, True) == {r.tid("antr", CY), r.tid("antr", WH)}
    assert uc.anims_of(us, False) == {r.tid("antr", CY)}


def check_serialisation(r, us):
    blob = uc.build(us)
    assert len(blob) % 16 == 0 and blob[:4] == b"UNIT"
    sec = uc.parse_section(blob)
    assert sec.header["biped_count"] == 2 and sec.header["vehicle_count"] == 2 and sec.header["seat_count"] == 3
    assert sec.header["player_biped"] == r.tid("bipd", CY)
    cy_i = [i for i, b in enumerate(us.bipeds) if b.vals["tag_id"] == r.tid("bipd", CY)][0]
    assert sec.header["player_biped_index"] == cy_i + 1
    g = sec.globals
    assert near(g["gravity"], GRAVITY, 1e-6) and g["default_biped_tag"] == r.tid("bipd", CY)
    cy = sec.bipeds[cy_i]
    assert uc.cstring(cy["name"]) == "cyborg" and uc.cstring(cy["hold_marker"]) == "right hand" and near(cy["radius"], 0.2)
    assert near(cy["anim_speed"][0], 2.25, 1e-4) and len(sec.materials) == 3 and len(sec.seats) == 3
    gr = us.globals.vals["grenades"]
    assert struct.unpack_from("<HHIIII", gr, 0)[0] == 4 and struct.unpack_from("<HHIIII", gr, 32)[2] == r.tid("effe", "effects\\throw2")
    # offsets of the records: the C reader hard-codes them
    assert uc.GLOBALS_REC == 832 and uc.BIPED_REC == 576 and uc.VEHICLE_REC == 256 and uc.SEAT_REC_OUT == 128 and uc.MATERIAL_REC_OUT == 48
    off = {n: o for n, _, o in uc.BIPED_L}
    assert off["bounding_radius"] == 0x70 and off["shield_recharge_rate"] == 0x14C and off["name"] == 0x170 and off["foot_markers"] == 0x1F0
    assert off["unit_flags"] == 0x150 and off["object_flags"] == 0x15C and off["weapon_count"] == 0x16C
    off = {n: o for n, _, o in uc.VEHICLE_L}
    assert off["max_forward"] == 0x40 and off["unit_flags"] == 0x6C and off["type"] == 0x78 and off["name"] == 0x86
    off = {n: o for n, _, o in uc.GLOBALS_L}
    assert off["gravity"] == 0xC0 and off["difficulty"] == 0x130 and off["grenades"] == 0xF0 and off["look_function"] == 0x9C
    return blob


def check_problems():
    r, m = build_synthetic()
    # falling velocities that do not give one gravity are reported
    fb = uc.Reader(m)
    gb = r.block("matg", "globals\\globals")
    n, o = fb.refl(gb + uc.G_FALLING, 152)
    r.poke(o + 0x90, "<f", 0.5)
    us = uc.collect_map(r.map())
    assert any("gravity" in p for p in us.problems), us.problems
    # a cached recharge rate that disagrees with the recharge time is reported
    r, m = build_synthetic()
    r.poke(r.block("coll", CY) + 0x1C0, "<f", 0.5)
    us = uc.collect_map(r.map())
    assert any("recharge" in p for p in us.problems), us.problems
    # an unresolvable reference becomes tag 0 plus a problem
    r, m = build_synthetic()
    r.poke(r.block("bipd", MA) + 0x38 + 12, "<I", 0x12345678)
    us = uc.collect_map(r.map())
    assert us.biped_by_tag(r.tid("bipd", MA)).vals["anim_tag"] == 0 and us.problems


def expect_error(r, what):
    try:
        uc.collect_map(r.map())
    except uc.UnitError:
        return
    raise AssertionError(f"{what}: no UnitError")


def check_corrupt():
    r, _ = build_synthetic()
    r.refl(r.block("bipd", CY) + 0x2D8, 5000, BASE + 0x100)         # weapon list outside the limit
    expect_error(r, "huge reflexive")
    r, _ = build_synthetic()
    r.refl(r.block("bipd", CY) + 0x2D8, 2, BASE + len(r.region) + 64)
    expect_error(r, "reflexive past the region")
    r, _ = build_synthetic()
    r.poke(r.block("bipd", MA), "<H", 1)
    expect_error(r, "object type mismatch")
    r, _ = build_synthetic()
    r.poke(r.block("bipd", CY) + 0x400, "<I", 0x7FC00000)
    expect_error(r, "NaN camera height")
    r, _ = build_synthetic()
    r.poke(r.block("vehi", WH) + 0x2F4, "<H", 9)
    expect_error(r, "vehicle type 9")
    r, _ = build_synthetic()
    gb = r.block("matg", "globals\\globals")
    r.refl(gb + uc.G_PLAYER, 0, 0)
    expect_error(r, "no player information")
    r, _ = build_synthetic()
    gb = r.block("matg", "globals\\globals")
    r.refl(gb + uc.G_FALLING, 1, BASE + 0x7FFFFF0)
    expect_error(r, "falling damage outside the region")
    # two globals tags
    tags = TAGS + [("matg", "globals\\globals2", 0x1000)]
    r = Region(tags)
    expect_error(r, "two globals")


def check_real_maps():
    names = ["a10", "a30", "a50", "b30", "b40", "c10", "c20", "c40", "d20", "d40"]
    found = [n for n in names if (ROOT / "assets" / "decompressed" / f"{n}.map").exists()]
    if not found:
        print("  (no decompressed maps present: real-map checks skipped)")
        return
    seen_player = False
    nodes_checked = 0
    for n in found:
        path = ROOT / "assets" / "decompressed" / f"{n}.map"
        m = texconv.MapData.open(path)
        us = uc.collect_map(m)
        assert not us.problems, (n, us.problems)
        g = us.globals.vals
        assert abs(g["gravity"] - GRAVITY) < 2e-6, (n, g["gravity"])
        assert (g["run_forward"], g["run_backward"], g["run_sideways"]) == (2.25, 2.0, 2.0)
        assert len(us.materials) == 32 and len(us.bipeds) >= 8
        ids = [b.vals["tag_id"] for b in us.bipeds]
        assert ids == sorted(ids) and len(set(ids)) == len(ids)
        player = us.biped_by_tag(g["default_biped_tag"])
        assert player is not None and player.vals["name"] == "cyborg"
        pv = player.vals
        # the player biped: radius 0.2, standing height 0.7, 75 / 75 vitals, jump 0.07 wu/tick
        assert abs(pv["radius"] - 0.2) < 1e-6 and abs(pv["stand_height"] - 0.7) < 1e-6 and abs(pv["jump_velocity"] - 0.07) < 1e-6
        assert pv["health"] == 75.0 and pv["shield"] == 75.0 and abs(pv["shield_recharge_time"] - 4.0) < 1e-6
        # the run animation's root motion is the globals' run speed (the proof of the speed unit)
        assert abs(pv["anim_speed"][0] - g["run_forward"]) < 0.01, pv["anim_speed"]
        assert abs(pv["anim_speed"][3] - g["sneak_forward"]) < 0.01
        rg = scenconv.Region(m)
        for b in us.bipeds:
            v = b.vals
            if v["model_tag"] and v["head_node"] != 0xFFFF:       # the pelvis / head node indices name the model's nodes
                nodes = scenconv.model_nodes(rg, rg.block(m.by_id[v["model_tag"]]))
                assert "pelvis" in nodes[v["pelvis_node"]] and "head" in nodes[v["head_node"]], (n, v["name"])
                nodes_checked += 1
            assert 0.0 <= v["radius"] <= 1.0 and 0.0 <= v["stand_height"] <= 2.0 and 0.0 <= v["health"] <= 1000.0, (n, v["name"])
            assert 0.0 <= v["jump_velocity"] <= 1.0 and 0.0 <= v["stand_camera"] <= 2.0, (n, v["name"])
            assert v["model_tag"] in m.by_id or v["model_tag"] == 0
            if v["shield_recharge_time"] > 0:
                assert abs(v["shield_recharge_rate"] * v["shield_recharge_time"] * 30 - 1.0) < 1e-3
        for v in us.vehicles:
            d = v.vals
            assert 0.0 <= d["max_forward"] <= 4.0 and d["type"] <= 6 and d["first_seat"] + d["seat_count"] <= len(us.seats)
        sec = uc.parse_section(uc.build(us))
        assert len(sec.bipeds) == len(us.bipeds) and sec.header["player_biped_index"] > 0
        seen_player = True
        models = uc.referenced_models(path)
        anims = uc.referenced_anims(path)
        assert models and anims and all(m.by_id[t].group == "mode" for t in models) and all(m.by_id[t].group == "antr" for t in anims)
    assert seen_player and (len(found) < 10 or nodes_checked >= 100), nodes_checked
    print(f"  real maps converted: {', '.join(found)}")


def main():
    r, m, us = check_collect()
    check_roots(r, us)
    blob = check_serialisation(r, us)
    check_problems()
    check_corrupt()
    check_real_maps()
    # a pack with the world's UNIT and an empty second section (no bipeds, vehicles, materials; neutral globals)
    empty_g = uc.Rec(uc.GLOBALS_L, uc.GLOBALS_REC)
    empty_g.set("gravity", 0.0035651791)
    empty = uc.UnitSet(empty_g, [], [], [], [])
    pack = hp2.build("test", [("UNIT", blob), ("UNI1", uc.build(empty))])
    got = hp2.parse(pack)
    assert uc.parse_section(got.read("UNI1")).bipeds == [] and uc.parse_section(got.read("UNIT")).header["biped_count"] == 2
    if os.environ.get("UNIT_FIXTURE"):
        Path(os.environ["UNIT_FIXTURE"]).write_bytes(pack)
    print("ok")


if __name__ == "__main__":
    main()
