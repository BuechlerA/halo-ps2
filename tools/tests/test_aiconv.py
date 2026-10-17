"""AIDT section on a synthetic map (no game data needed): actor and variant images with flattened references, the actor
palette, encounters with squads / platoons / starting locations / move and firing positions, command lists, conversations,
AI animation and script references, sanitising of dangling indices and stale bytes, the roots exported to the object and
sound packs, the writer and its reference parser, name tables, corrupt maps. Set AI_FIXTURE=<path> to also write the .hp2
the C reader test (engine/tests/test_aidef.c, which hard-codes the numbers of this world) opens. A skip-if-missing check
converts the real campaign maps and checks ranges, units and indices."""
import math
import os
import re
import struct
import sys
from pathlib import Path

sys.path[:0] = [str(Path(__file__).resolve().parents[1]), str(Path(__file__).resolve().parent)]
from halo2ps2 import aiconv as ai, hp2, scenconv, tagindex, texconv  # noqa: E402

BASE = tagindex.XBOX_TAG_BASE
ROOT = Path(__file__).resolve().parents[2]
NAMES = ("scn", "act_a", "act_b", "act_t", "var_a", "var_b", "var_c", "var_s", "unit_a", "unit_b", "wpn_a", "wpn_b",
         "mod_ua", "mod_ub", "mod_wa", "mod_wb", "gr_ua", "gr_ub", "gr_wa", "gr_n1", "gr_n2", "dlg", "s1", "s2", "s3", "s4", "s5",
         "s6")
GROUPS = {"scn": "scnr", "act_a": "actr", "act_b": "actr", "act_t": "actr", "var_a": "actv", "var_b": "actv", "var_c": "actv",
          "var_s": "actv", "unit_a": "bipd", "unit_b": "bipd", "wpn_a": "weap", "wpn_b": "weap", "mod_ua": "mode",
          "mod_ub": "mode", "mod_wa": "mode", "mod_wb": "mode", "gr_ua": "antr", "gr_ub": "antr", "gr_wa": "antr",
          "gr_n1": "antr", "gr_n2": "antr", "dlg": "udlg", "s1": "snd!", "s2": "snd!", "s3": "snd!", "s4": "snd!",
          "s5": "snd!", "s6": "snd!"}
SIZES = {"scn": 0x20000, "dlg": 4112}


def path(n: str) -> str:
    return "tst\\" + n


TAGS = [(GROUPS[n], path(n), SIZES.get(n, 0x1000)) for n in NAMES]
IDX = {n: i for i, n in enumerate(NAMES)}


def TAG(n: str) -> int:
    i = IDX[n]
    return ((0xE174 + i) << 16) | i


class Region:
    """Tag region builder: tags with zero-filled data blocks, then pokes (offsets are region offsets)."""

    def __init__(self, tags):
        self.region = bytearray(tagindex.TagIndex.build(tags, scenario_index=0))
        self.index = tagindex.TagIndex.parse_region(bytes(self.region), 0x1000)
        self.by_path = {e.path: e for e in self.index.entries}
        self.cursor = {}

    def block(self, n):
        return self.by_path[path(n)].data_address - BASE

    def alloc(self, n, data: bytes) -> int:
        e = self.by_path[path(n)]
        cur = (self.cursor.get(n, 0x1000) + 3) & ~3
        off = e.data_address - BASE + cur
        self.region[off:off + len(data)] = data
        self.cursor[n] = cur + len(data)
        return BASE + off

    def poke(self, off, fmt, *vals):
        struct.pack_into(fmt, self.region, off, *vals)

    def ref(self, n):
        group = GROUPS[n] if n else None
        if n is None:
            return struct.pack("<4sIII", b"\xff\xff\xff\xff", 0, 0, 0xFFFFFFFF)
        return struct.pack("<4sIII", group[::-1].encode(), 0, 0, TAG(n))

    def null_ref(self, group):
        return struct.pack("<4sIII", group[::-1].encode(), 0, 0, 0xFFFFFFFF)

    def map(self):
        idx = tagindex.TagIndex.parse_region(bytes(self.region), 0x1000)
        return texconv.MapData(Path("synthetic.map"), None, idx, bytes(self.region),  # type: ignore[arg-type]
                               {e.tag_id: e for e in idx.entries})


def refl(count: int, addr: int) -> bytes:
    return struct.pack("<III", count, addr, 0)


def name32(s: str, junk: str = "") -> bytes:
    """A String32 as the tags have it: text, NUL, then optional stale bytes."""
    b = bytearray(32)
    raw = (s.encode() + b"\0" + junk.encode())[:32]
    b[:len(raw)] = raw
    return bytes(b)


def put(img: bytearray, named, name, value):
    for off, fmt, nm in named:
        if nm == name:
            vals = value if isinstance(value, (tuple, list)) else (value,)
            struct.pack_into("<" + fmt, img, off, *vals)
            return
    raise KeyError(name)


# ---- the synthetic world ------------------------------------------------------------------------------
ACTOR_A = {
    "flags": 0x1234, "more_flags": 0x40, "type": 7, "max_vision_distance": 40.0, "central_vision_angle": 0.8726646,
    "max_vision_angle": 1.2217305, "peripheral_vision_angle": 2.0943952, "peripheral_distance": 6.0,
    "standing_gun_offset": (0.2, -0.1, 0.51), "hearing_distance": 40.0, "notice_projectile_chance": 0.5,
    "notice_vehicle_chance": 0.75, "combat_perception_time": 0.6, "guard_perception_time": 1.4,
    "non_combat_perception_time": 2.0, "inv_combat_perception_time": 1.0 / 18, "inv_guard_perception_time": 1.0 / 42,
    "inv_non_combat_perception_time": 1.0 / 60, "fractions_80": (0.25, 0.25, 0.75, 0.2), "values_90": (0.0, 5.0, 2.0),
    "angle_9c": 0.6981317, "angle_9c_cos": math.cos(0.6981317), "max_aiming_deviation": (1.5707964, 1.5707964),
    "max_looking_deviation": (1.5707964, 1.0471976), "looking_angles": (1.5, 1.4, 1.3, 1.2, 1.1, 1.0, 0.9, 0.8),
    "idle_times": tuple(v for i in range(10) for v in (0.5 + i, 1.5 + i)),
    "cos_aiming_deviation": (math.cos(1.5707964), math.cos(1.5707964)),
    "cos_looking_deviation": (math.cos(1.5707964), math.cos(1.0471976)),
}
ACTOR_B = dict(ACTOR_A, type=0, flags=0x2000, more_flags=0, max_vision_distance=25.0, hearing_distance=30.0,
               standing_gun_offset=(0.2, 0.095, 0.46))
ACTOR_T = dict(ACTOR_A, type=15, flags=0x2000, more_flags=0x62697064, central_vision_angle=2.0, max_vision_angle=2.0,
               peripheral_vision_angle=40.0, peripheral_distance=0.0, max_aiming_deviation=(3.1415927, 1.5707964))
VARIANT_A = {
    "flags": 8, "movement_type": 1, "initial_crouch_chance": 0.7, "crouch_time": (6.0, 8.0), "run_time": (2.0, 4.0),
    "max_firing_distance": 13.0, "rate_of_fire": 10.0, "projectile_error": 0.0261799, "first_burst_delay": (0.25, 1.25),
    "new_target_firing_pattern_time": 5.0, "surprise_delay_time": 0.6, "surprise_fire_wildly_time": 0.3,
    "death_fire_wildly_chance": 0.1, "death_fire_wildly_time": 3.0, "desired_combat_range": (4.0, 8.0),
    "target_tracking": 0.9, "target_leading": 0.5, "weapon_damage_modifier": 0.5, "burst_origin_radius": 0.3,
    "burst_origin_angle": 0.0872665, "burst_return_length": (0.275, 0.5), "burst_return_angle": 0.1745329,
    "burst_duration": (0.75, 1.1), "burst_separation": (0.5, 1.5), "burst_angular_velocity": 0.0698132,
    "special_damage_modifier": 0.4, "special_fire": (1, 2), "special_fire_chance": 0.35, "special_fire_delay": 10.0,
    "grenade_kind": (0, 0, 1, 3), "grenade_enemy_radius": 3.5, "grenade_velocity": 7.0 / 30, "grenade_ranges": (3.0, 14.0),
    "grenade_collateral_radius": 2.5, "grenade_chance": 0.7, "grenade_check_time": 0.5, "encounter_grenade_timeout": 3.0,
    "grenade_count": (1, 2), "dont_drop_grenades_chance": 0.7, "drop_weapon_loaded": (0.7, 0.9), "drop_weapon_ammo": (90, 140),
    "body_vitality": 100.0, "shield_vitality": 45.0, "forced_shader_permutation": 2,
}
VARIANT_B = dict(VARIANT_A, flags=0, movement_type=0, body_vitality=25.0, shield_vitality=0.0, rate_of_fire=5.0)
VARIANT_C = dict(VARIANT_A, flags=0, grenade_kind=(1, 2, 0, 0), body_vitality=225.0, shield_vitality=300.0)


def build_actor(r: Region, n: str, fields: dict) -> None:
    b = r.block(n)
    img = bytearray(ai.ACTOR_SRC)
    for k, v in fields.items():
        put(img, ai.ACTOR_NAMED, k, v)
    struct.pack_into("<2H", img, 0x268, 4, 3)                    # unnamed words: kept raw
    struct.pack_into("<If", img, 0x2F8, 3, 0.6)
    struct.pack_into("<2H", img, 0x354, 6, 7)
    for off, grp in ai.ACTOR_REFS.items():
        img[off:off + 16] = r.null_ref(next(iter(grp)))
    r.region[b:b + ai.ACTOR_SRC] = img


def build_variant(r: Region, n: str, fields: dict, actor, unit, major, weapon, stale=False, changes=0) -> None:
    b = r.block(n)
    img = bytearray(ai.VARIANT_SRC)
    for k, v in fields.items():
        put(img, ai.VARIANT_NAMED, k, v)
    for off, grp in ai.VARIANT_REFS.items():
        img[off:off + 16] = r.null_ref(grp[0])
    for off, tag in ((0x04, actor), (0x14, unit), (0x24, major), (0x64, weapon)):
        if tag:
            img[off:off + 16] = r.ref(tag)
    if stale:
        img[0x180:0x190] = b"rpjoD\x18\x01\xb4\x00\x00\x00\x00\xff\xff\xff\xff"
    r.region[b:b + ai.VARIANT_SRC] = img
    if changes:
        r.poke(b + ai.VARIANT_CHANGE_COLORS, "<III", changes, BASE + 0x1234, 0)


def build_units(r: Region) -> None:
    for n, model, graph, dlg in (("unit_a", "mod_ua", "gr_ua", True), ("unit_b", "mod_ub", "gr_ub", False)):
        b = r.block(n)
        r.region[b + 0x28:b + 0x38] = r.ref(model)
        r.region[b + 0x38:b + 0x48] = r.ref(graph)
        if dlg:
            el = bytearray(24)
            struct.pack_into("<H", el, 0, 100)
            el[8:24] = r.ref("dlg")
            r.region[b + 0x2B4:b + 0x2B4 + 12] = refl(1, r.alloc(n, bytes(el)))
    for n, model, graph in (("wpn_a", "mod_wa", "gr_wa"), ("wpn_b", "mod_wb", None)):
        b = r.block(n)
        r.region[b + 0x28:b + 0x38] = r.ref(model)
        r.region[b + 0x38:b + 0x48] = r.ref(graph) if graph else r.null_ref("antr")
    d = r.block("dlg")
    for k, s in enumerate(("s4", "s5", None, "s5")):
        r.region[d + 0x10 + 16 * k:d + 0x20 + 16 * k] = r.ref(s) if s else r.null_ref("snd!")


def encounter_bytes(name, flags, team, w28, manual, pre, squads, platoons, firings, pstarts) -> bytes:
    b = bytearray(176)
    b[:32] = name32(name, "stale")
    struct.pack_into("<IHH", b, 0x20, flags, team, 1)
    struct.pack_into("<Hh", b, 0x28, w28, 0)
    struct.pack_into("<h", b, 0x2A, manual)
    struct.pack_into("<h", b, 0x7E, pre)
    for off, (c, a) in zip((0x80, 0x8C, 0x98, 0xA4), (squads, platoons, firings, pstarts)):
        b[off:off + 12] = refl(c, a)
    return bytes(b)


def squad_bytes(name, atype, plat, states, flags, leader, maneuver, delay, counts, upgrade, moves, starts, tail=(3, 5, 7)) -> bytes:
    b = bytearray(232)
    b[:32] = name32(name, "junk")
    struct.pack_into("<hhHHI", b, 0x20, atype, plat, states[0], states[1], flags)
    struct.pack_into("<H", b, 0x2C, leader)
    struct.pack_into("<h", b, 0x4E, maneuver)
    struct.pack_into("<f", b, 0x50, delay)
    struct.pack_into("<7I", b, 0x54, 0x03FFFFFF, 0x03FFFFFF, 1, 2, 3, 4, 0x100)
    struct.pack_into("<2H", b, 0x7C, *counts)
    struct.pack_into("<H", b, 0x80, upgrade)
    struct.pack_into("<3H", b, 0x82, *tail)
    b[0xC4:0xD0] = refl(*moves)
    b[0xD0:0xDC] = refl(*starts)
    return bytes(b)


def start_bytes(pos, facing, cluster, w12, cmd, w14=0, w16=0, w18=-1) -> bytes:
    return struct.pack("<4f6h", *pos, facing, cluster, w12, w14, w16, w18, cmd)


def move_bytes(pos, facing, weight, t0, t1, seq, cluster, surface) -> bytes:
    b = bytearray(80)
    struct.pack_into("<4f", b, 0, *pos, facing)
    struct.pack_into("<3f", b, 0x10, weight, t0, t1)
    struct.pack_into("<2h", b, 0x1C, -1, seq)
    struct.pack_into("<h", b, 0x28, cluster)
    struct.pack_into("<i", b, 0x4C, surface)
    return bytes(b)


def firing_bytes(pos, group, cluster, surface) -> bytes:
    return struct.pack("<3fHhIi", *pos, group, cluster, 0, surface)


def platoon_bytes(name, w20, w30, w32, w3c, w3e) -> bytes:
    b = bytearray(172)
    b[:32] = name32(name, "old")
    struct.pack_into("<I", b, 0x20, w20)
    struct.pack_into("<Hh", b, 0x30, w30, w32)
    struct.pack_into("<Hh", b, 0x3C, w3c, w3e)
    b[0x50:0x58] = b"stale!!\0"            # stale text in the unused tail: ignored
    return bytes(b)


def command_bytes(typ, mod, p1, p2, point1=-1, point2=-1, anim=-1, script=-1, rec=-1, cmd=-1, obj=-1) -> bytes:
    return struct.pack("<2H2f7h6x", typ, mod, p1, p2, point1, point2, anim, script, rec, cmd, obj)


def cmdlist_bytes(name, flags, bsp, commands, points, pre=-1) -> bytes:
    b = bytearray(96)
    b[:32] = name32(name)
    struct.pack_into("<I", b, 0x20, flags)
    struct.pack_into("<2h", b, 0x2C, bsp, pre)
    b[0x30:0x3C] = refl(*commands)
    b[0x3C:0x48] = refl(*points)
    return bytes(b)


def conv_bytes(name, flags, trig, run, parts, lines) -> bytes:
    b = bytearray(116)
    b[:32] = name32(name)
    struct.pack_into("<Iff", b, 0x20, flags, trig, run)
    b[0x50:0x5C] = refl(*parts)
    b[0x5C:0x68] = refl(*lines)
    return bytes(b)


def part_bytes(sel, atype, use, new, variants, enc_name, enc_index, w00=0, w02=0) -> bytes:
    b = bytearray(84)
    struct.pack_into("<6H", b, 0, w00, w02, sel, atype, use & 0xFFFF, new & 0xFFFF)
    struct.pack_into("<6h", b, 0x18, *variants)
    b[0x24:0x44] = name32(enc_name)
    struct.pack_into("<i", b, 0x44, enc_index)
    return bytes(b)


def line_bytes(r, flags, part, addr, addr_part, delay, sounds) -> bytes:
    b = bytearray(124)
    struct.pack_into("<HhHh", b, 0, flags, part, addr, addr_part)
    struct.pack_into("<f", b, 0x0C, delay)
    for k in range(6):
        s = sounds[k] if k < len(sounds) else None
        b[0x1C + 16 * k:0x2C + 16 * k] = r.ref(s) if s else r.null_ref("snd!")
    return bytes(b)


def build_synthetic(variations: dict | None = None) -> Region:
    """The world. ``variations`` lets the corruption tests change single values: names -> replacement."""
    v = variations or {}
    r = Region(TAGS)
    build_actor(r, "act_a", ACTOR_A)
    build_actor(r, "act_b", ACTOR_B)
    build_actor(r, "act_t", ACTOR_T)
    build_variant(r, "var_a", VARIANT_A, "act_a", "unit_a", "var_b", "wpn_a", changes=1)
    build_variant(r, "var_b", VARIANT_B, "act_a", "unit_a", None, "wpn_b")
    build_variant(r, "var_c", VARIANT_C, None, None, None, None)
    build_variant(r, "var_s", VARIANT_B, "act_t", "unit_b", None, "wpn_b", stale=True)
    build_units(r)
    sb = r.block("scn")

    def A(data):
        return r.alloc("scn", data)

    # palette: variant b, a, c (index 1 is variant_a)
    pal = b"".join(r.ref(n) for n in ("var_b", "var_a", "var_c"))
    r.region[sb + ai.SCN_PALETTE:sb + ai.SCN_PALETTE + 12] = refl(3, A(pal))
    # AI animation references (name, antr reference) and script references
    anims = b"".join(name32(nm) + r.ref(g) + bytes(12) for nm, g in (("anim_one", "gr_n1"), ("anim_two", "gr_n2")))
    r.region[sb + ai.SCN_ANIMS:sb + ai.SCN_ANIMS + 12] = refl(2, A(anims))
    r.region[sb + ai.SCN_SCRIPTS:sb + ai.SCN_SCRIPTS + 12] = refl(1, A(name32("script_one") + bytes(8)))
    if "recordings" in v:
        r.region[sb + ai.SCN_RECORDINGS:sb + ai.SCN_RECORDINGS + 12] = refl(v["recordings"], A(bytes(64)))

    # command lists: "cmd_b" (4 commands, 2 points), "CMD_A" (1 command), "cmd_c" (none), "CMD_B" (a repeated name)
    cmds_b = b"".join((command_bytes(5, 0, 0.0, 0.0),
                       command_bytes(13, 0, 1.0, 0.0, anim=1),
                       command_bytes(20, 1, 0.6, 2.6, point1=0, point2=1, cmd=1),
                       command_bytes(12, 0, 0.0, 0.0, script=0, obj=3)))
    pts_b = b"".join((struct.pack("<3fiI", 1.0, 2.0, 3.0, 7, 0), struct.pack("<3fiI", -4.0, 5.5, 6.0, -1, 0)))
    c0 = (4, A(cmds_b)), (2, A(pts_b))
    c1 = (1, A(command_bytes(3, 0, 5.0, 90.0))), (0, 0)
    lists = [cmdlist_bytes("cmd_b", 0x20, 0, *c0, pre=1), cmdlist_bytes("CMD_A", 0, -1, *c1),
             cmdlist_bytes("cmd_c", 0x24, 2, (0, 0), (0, 0), pre=3), cmdlist_bytes("CMD_B", 0, -1, (0, 0), (0, 0))]    # the last repeats a name
    r.region[sb + ai.SCN_COMMAND_LISTS:sb + ai.SCN_COMMAND_LISTS + 12] = refl(4, A(b"".join(lists)))

    # encounters: "enc_b" (rich), "Enc_A" (one squad), "enc_c" (empty)
    starts_s0 = b"".join((start_bytes((1.0, 2.0, 3.0), 0.5, 4, 0, v.get("start_cmd", 0)),
                          start_bytes((4.0, 5.0, 6.0), -3.1, 5, 3, -1, w14=9, w16=7, w18=2),
                          start_bytes((-7.0, 8.0, 9.5), 3.0, 4, 1, 2)))
    moves_s0 = b"".join((move_bytes((10.0, 11.0, 12.0), 1.5, 0.001, 0.0, 0.0, 0, 3, -1),
                         move_bytes((13.0, 14.0, 15.0), -1.5, 2.0, 1.0, 4.0, 3, 4, 77)))
    s0 = squad_bytes("sq_one", v.get("actor_type", 1), 0, (10, 9), 32, 0, v.get("maneuver", 1), 2.5, (2, 4), 1,
                     (2, A(moves_s0)), (3, A(starts_s0)))
    s1 = squad_bytes("sq_two", -1, -1, (9, 9), 0, 4, -1, 0.0, (0, 0), 0, (0, 0), (0, 0))
    squads_e0 = A(s0 + s1)
    plat_e0 = A(platoon_bytes("plt_one", 6, 0, -1, 0, -1))
    firing_e0 = A(b"".join((firing_bytes((0.5, 1.5, 2.5), 0, 4, 48), firing_bytes((3.5, 4.5, 5.5), 1, 5, -1),
                            firing_bytes((6.5, 7.5, 8.5), 25, -1, 12345))))
    pstart_e0 = A(struct.pack("<4f6H", 9.0, 8.0, 7.0, 1.25, 2, 1, 0, 0, 0, 0) + bytes(24))
    e0 = encounter_bytes("enc_b", 0x61, 0, 2, 1, 1, (2, squads_e0), (1, plat_e0), (3, firing_e0), (1, pstart_e0))
    starts_s2 = start_bytes((20.0, 21.0, 22.0), 0.0, 1, 0, -1)
    s2 = squad_bytes("SQ_ONE", 0, -1, (2, 3), 4, 0, -1, 0.0, (1, 1), 0, (0, 0), (1, A(starts_s2)))
    e1 = encounter_bytes("Enc_A", 0x1, 4, 0, 0, 0, (1, A(s2)), (0, 0), (0, 0), (0, 0))
    e2 = encounter_bytes("enc_c", 0x1, 0, 0, 0, -1, (0, 0), (0, 0), (0, 0), (0, 0))
    r.region[sb + ai.SCN_ENCOUNTERS:sb + ai.SCN_ENCOUNTERS + 12] = refl(3, A(e0 + e1 + e2))

    # conversations: "conv_b" (2 participants, 2 lines), "Conv_A" (1 participant, no line)
    parts_b = b"".join((part_bytes(0, 7, -1, -1, (-1,) * 6, "enc_b", v.get("part_enc", 0)),
                        part_bytes(2, 14, 12, 13, (100, 101, -1, -1, -1, -1), "", -1, w02=1)))
    lines_b = b"".join((line_bytes(r, 0, 0, 1, -1, 1.5, ["s1", "s2"]), line_bytes(r, 4, v.get("line_part", 1), 0, 0, 0.0, ["s3"])))
    parts_a = part_bytes(0, 8, -1, -1, (-1,) * 6, "enc_c", 2)
    convs = conv_bytes("conv_b", 0xF2, 4.0, 4.5, (2, A(parts_b)), (2, A(lines_b))) + conv_bytes(
        "Conv_A", 0, 0.0, 0.0, (1, A(parts_a)), (0, 0))
    r.region[sb + ai.SCN_CONVERSATIONS:sb + ai.SCN_CONVERSATIONS + 12] = refl(2, A(convs))
    return r


def collect(r: Region) -> ai.AiSet:
    return ai.collect_map(r.map())


# ---- checks ---------------------------------------------------------------------------------------------
def near(a, b, tol=1e-5):
    if isinstance(a, tuple):
        return all(near(x, y, tol) for x, y in zip(a, b)) and len(a) == len(b)
    return abs(a - b) <= tol


def check_images(s: ai.AiSet) -> None:
    assert [t for t, _ in s.actors] == sorted([TAG("act_a"), TAG("act_b"), TAG("act_t")])
    assert [t for t, _ in s.variants] == sorted([TAG("var_a"), TAG("var_b"), TAG("var_c"), TAG("var_s")])
    assert all(len(img) == ai.ACTOR_REC for _, img in s.actors) and all(len(img) == ai.VARIANT_REC for _, img in s.variants)
    a = dict(s.actors)
    da = ai.decode_actor(a[TAG("act_a")])
    for k, v in ACTOR_A.items():
        assert near(da[k], v), (k, da[k], v)
    assert da["weapon_tag"] == 0 and da["projectile_tag"] == 0 and da["actor_tag"] == 0       # null references flatten to 0
    img = a[TAG("act_a")]
    assert struct.unpack_from("<2H", img, 0x268) == (4, 3) and struct.unpack_from("<I", img, 0x2F8)[0] == 3
    assert img[0x13C:0x148] == bytes(12)                    # group / pointer / length are flattened away
    dt = ai.decode_actor(a[TAG("act_t")])
    assert dt["type"] == 15 and dt["peripheral_vision_angle"] == 40.0 and dt["more_flags"] == 0x62697064
    v = dict(s.variants)
    va = ai.decode_variant(v[TAG("var_a")])
    for k, val in VARIANT_A.items():
        assert near(va[k], val), (k, va[k], val)
    assert va["actor_tag"] == TAG("act_a") and va["unit_tag"] == TAG("unit_a") and va["major_variant_tag"] == TAG("var_b")
    assert va["weapon_tag"] == TAG("wpn_a") and va["equipment_tag"] == 0 and va["change_color_count"] == 1
    assert v[TAG("var_a")][ai.VARIANT_CHANGE_COLORS + 4:ai.VARIANT_CHANGE_COLORS + 12] == bytes(8)    # pointer dropped
    vc = ai.decode_variant(v[TAG("var_c")])
    assert vc["actor_tag"] == 0 and vc["unit_tag"] == 0 and vc["grenade_kind"] == (1, 2, 0, 0)
    vs = ai.decode_variant(v[TAG("var_s")])
    assert vs["grenade_kind"] == (0, 0, 0, 0) and v[TAG("var_s")][0x180:0x190] == bytes(16)
    assert any("stale bytes" in p for p in s.problems), s.problems
    assert [t for t in s.palette] == [TAG("var_b"), TAG("var_a"), TAG("var_c")]


def check_scenario(s: ai.AiSet) -> None:
    E = [e.vals for e in s.encounters]
    assert [e["name"] for e in E] == ["enc_b", "Enc_A", "enc_c"]                  # stale bytes after the NUL are dropped
    e0 = E[0]
    assert e0["flags"] == 0x61 and e0["team"] == 0 and e0["search_behavior"] == 1 and e0["w28"] == 2
    assert e0["manual_bsp"] == 1 and e0["precomputed_bsp"] == 1
    assert (e0["first_squad"], e0["squad_count"], e0["first_platoon"], e0["platoon_count"]) == (0, 2, 0, 1)
    assert (e0["first_firing"], e0["firing_count"], e0["first_pstart"], e0["pstart_count"]) == (0, 3, 0, 1)
    assert E[1]["team"] == 4 and (E[1]["first_squad"], E[1]["squad_count"]) == (2, 1) and E[1]["first_firing"] == 3
    assert E[2]["precomputed_bsp"] == -1 and E[2]["squad_count"] == 0 and E[2]["first_squad"] == 3
    Q = [q.vals for q in s.squads]
    assert [q["name"] for q in Q] == ["sq_one", "sq_two", "SQ_ONE"]
    q0 = Q[0]
    assert (q0["actor_type"], q0["platoon"], q0["initial_state"], q0["return_state"], q0["flags"]) == (1, 0, 10, 9, 32)
    assert q0["maneuver_squad"] == 1 and near(q0["delay_time"], 2.5) and (q0["normal_count"], q0["insane_count"]) == (2, 4)
    assert q0["major_upgrade"] == 1 and (q0["w82"], q0["w84"], q0["w86"]) == (3, 5, 7)
    assert q0["masks"] == (0x03FFFFFF, 0x03FFFFFF, 1, 2, 3, 4, 0x100)
    assert (q0["first_start"], q0["start_count"], q0["first_move"], q0["move_count"]) == (0, 3, 0, 2)
    assert Q[1]["actor_type"] == -1 and Q[1]["platoon"] == -1 and Q[1]["unique_leader"] == 4 and Q[1]["start_count"] == 0
    assert Q[1]["first_start"] == 3 and Q[2]["first_start"] == 3 and Q[2]["start_count"] == 1
    S = [x.vals for x in s.starts]
    assert len(S) == 4 and S[0]["pos"] == (1.0, 2.0, 3.0) and near(S[0]["facing"], 0.5) and S[0]["cluster"] == 4
    assert S[0]["command_list"] == 0 and S[1]["command_list"] == -1 and S[2]["command_list"] == 2
    assert (S[1]["w12"], S[1]["w14"], S[1]["w16"], S[1]["w18"]) == (3, 9, 7, 2)
    M = [x.vals for x in s.moves]
    assert len(M) == 2 and M[0]["animation"] == -1 and M[1]["surface"] == 77 and M[1]["sequence"] == 3 and M[1]["cluster"] == 4
    assert near(M[0]["weight"], 0.001) and near(M[1]["time_max"], 4.0) and near(M[1]["facing"], -1.5)
    F = [x.vals for x in s.firings]
    assert len(F) == 3 and F[0]["surface"] == 48 and F[1]["surface"] == -1 and F[2]["group"] == 25 and F[2]["cluster"] == -1
    assert F[2]["surface"] == 12345 and F[1]["pos"] == (3.5, 4.5, 5.5)
    P = [x.vals for x in s.platoons]
    assert len(P) == 1 and P[0]["name"] == "plt_one" and P[0]["w20"] == 6 and P[0]["w32"] == -1 and P[0]["w3e"] == -1
    T = [x.vals for x in s.pstarts]
    assert len(T) == 1 and T[0]["pos"] == (9.0, 8.0, 7.0) and near(T[0]["facing"], 1.25) and T[0]["team"] == 2 and T[0]["bsp"] == 1
    L = [x.vals for x in s.cmdlists]
    assert [c["name"] for c in L] == ["cmd_b", "CMD_A", "cmd_c", "CMD_B"] and L[0]["flags"] == 0x20 and L[2]["manual_bsp"] == 2
    assert (L[0]["precomputed_bsp"], L[1]["precomputed_bsp"], L[2]["precomputed_bsp"]) == (1, -1, 3)
    assert (L[0]["first_command"], L[0]["command_count"], L[0]["first_point"], L[0]["point_count"]) == (0, 4, 0, 2)
    assert (L[1]["first_command"], L[1]["command_count"], L[1]["first_point"]) == (4, 1, 2) and L[2]["first_command"] == 5
    C = [x.vals for x in s.commands]
    assert len(C) == 5 and (C[1]["type"], C[1]["animation"]) == (13, 1) and C[2]["type"] == 20 and C[2]["command"] == 1
    assert (C[2]["point1"], C[2]["point2"], C[2]["modifier"]) == (0, 1, 1) and near(C[2]["param1"], 0.6) and near(C[2]["param2"], 2.6)
    assert C[3]["script"] == 0 and C[3]["object_name"] == 3 and C[3]["recording"] == -1 and C[4]["type"] == 3
    assert near(C[4]["param2"], 90.0)
    Pt = [x.vals for x in s.points]
    assert Pt[0]["pos"] == (1.0, 2.0, 3.0) and Pt[0]["surface"] == 7 and Pt[1]["surface"] == -1
    V = [x.vals for x in s.convs]
    assert [c["name"] for c in V] == ["conv_b", "Conv_A"] and V[0]["flags"] == 0xF2 and near(V[0]["trigger_distance"], 4.0)
    assert near(V[0]["run_to_player_distance"], 4.5)
    assert (V[0]["first_participant"], V[0]["participant_count"], V[0]["first_line"], V[0]["line_count"]) == (0, 2, 0, 2)
    assert (V[1]["first_participant"], V[1]["participant_count"], V[1]["line_count"]) == (2, 1, 0)
    R = [x.vals for x in s.parts]
    assert len(R) == 3 and R[0]["actor_type"] == 7 and R[0]["encounter_index"] == 0 and R[0]["encounter_name"] == "enc_b"
    assert R[1]["actor_type"] == 14 and R[1]["variants"] == (100, 101, -1, -1, -1, -1) and R[1]["encounter_index"] == -1
    assert R[1]["use_object"] == 12 and R[1]["new_name_object"] == 13 and R[1]["w02"] == 1 and R[2]["encounter_index"] == 2
    Ln = [x.vals for x in s.lines]
    assert len(Ln) == 2 and Ln[0]["sounds"] == (TAG("s1"), TAG("s2"), 0, 0, 0, 0) and Ln[0]["addressee"] == 1
    assert Ln[0]["participant"] == 0 and Ln[0]["addressee_participant"] == -1 and near(Ln[0]["delay"], 1.5)
    assert Ln[1]["flags"] == 4 and Ln[1]["participant"] == 1 and Ln[1]["sounds"][0] == TAG("s3")
    An = [x.vals for x in s.anims]
    assert [a["name"] for a in An] == ["anim_one", "anim_two"] and An[1]["graph_tag"] == TAG("gr_n2")
    assert [x.vals["name"] for x in s.scripts] == ["script_one"]
    assert s.sound_tags == {TAG("s1"), TAG("s2"), TAG("s3")}


def check_roots(r: Region) -> None:
    m = r.map()
    rd = ai.Reader(m)
    _, variants = ai.read_actors(rd)
    a = ai.unit_assets(rd, variants)
    assert a["models"] == {TAG("mod_ua"), TAG("mod_ub"), TAG("mod_wa"), TAG("mod_wb")}
    assert a["anims"] == {TAG("gr_ua"), TAG("gr_ub"), TAG("gr_wa")}
    assert a["dialogue"] == {TAG("dlg")}
    assert ai.dialogue_sounds(rd, a["dialogue"]) == {TAG("s4"), TAG("s5")}


def check_serialisation(s: ai.AiSet) -> bytes:
    blob = ai.build(s)
    assert len(blob) % 16 == 0 and blob[:4] == b"AIDT"
    sec = ai.parse_section(blob)
    assert sec["header"]["size"] == len(blob)
    assert len(sec["actors"]) == 3 and len(sec["variants"]) == 4 and sec["actor_ids"] == sorted(sec["actor_ids"])
    assert sec["variant_ids"] == sorted(sec["variant_ids"])
    assert sec["palette"] == [{"variant_tag": t, "variant_index": sec["variant_ids"].index(t)}
                              for t in (TAG("var_b"), TAG("var_a"), TAG("var_c"))]
    assert near(sec["actors"][0]["max_vision_distance"], 40.0) or near(sec["actors"][0]["max_vision_distance"], 25.0)
    for name, lst in (("encounters", s.encounters), ("squads", s.squads), ("starts", s.starts), ("moves", s.moves),
                      ("firings", s.firings), ("platoons", s.platoons), ("pstarts", s.pstarts), ("cmdlists", s.cmdlists),
                      ("commands", s.commands), ("points", s.points), ("convs", s.convs), ("parts", s.parts),
                      ("lines", s.lines), ("anims", s.anims), ("scripts", s.scripts)):
        assert len(sec[name]) == len(lst), name
        for got, want in zip(sec[name], lst):
            for k, v in want.vals.items():
                g = got[k]
                if isinstance(v, str):
                    g = ai.unitconv.cstring(g)
                assert g == v or near(g, v), (name, k, g, v)
    # name tables: sorted case-insensitively, stable
    assert sec["enc_names"] == [1, 0, 2] and sec["cmd_names"] == [1, 0, 3, 2] and sec["conv_names"] == [1, 0]
    # records have the sizes the C reader hard-codes
    assert [(n, rec, mx) for n, rec, mx in ai.TABLES][:6] == [
        ("actors", 1272, 255), ("variants", 568, 1024), ("actor_ids", 4, 255), ("variant_ids", 4, 1024), ("palette", 8, 255),
        ("encounters", 80, 1024)]
    # the offsets of the layouts that aidef.c hard-codes
    off = {n: {f: o for f, _, o in L} for n, L in (("enc", ai.ENCOUNTER_L), ("squad", ai.SQUAD_L), ("start", ai.START_L),
                                                    ("move", ai.MOVE_L), ("firing", ai.FIRING_L), ("platoon", ai.PLATOON_L),
                                                    ("pstart", ai.PSTART_L), ("cmdlist", ai.CMDLIST_L),
                                                    ("command", ai.COMMAND_L), ("point", ai.POINT_L), ("conv", ai.CONV_L),
                                                    ("part", ai.PART_L), ("line", ai.LINE_L), ("anim", ai.ANIM_L))}
    assert off["enc"]["flags"] == 32 and off["enc"]["first_squad"] == 48 and off["enc"]["pstart_count"] == 76
    assert off["squad"]["actor_type"] == 32 and off["squad"]["masks"] == 52 and off["squad"]["first_start"] == 92
    assert off["squad"]["move_count"] == 104 and off["start"]["command_list"] == 26 and off["move"]["surface"] == 36
    assert off["firing"]["surface"] == 16 and off["platoon"]["w3e"] == 42 and off["pstart"]["types"] == 20
    assert off["cmdlist"]["precomputed_bsp"] == 38 and off["cmdlist"]["point_count"] == 52 and off["command"]["object_name"] == 24 and off["point"]["surface"] == 12
    assert off["conv"]["line_count"] == 56 and off["part"]["encounter_index"] == 24 and off["part"]["encounter_name"] == 28
    assert off["line"]["delay"] == 8 and off["line"]["sounds"] == 12 and off["anim"]["graph_tag"] == 32
    return blob


def check_c_tables_match() -> None:
    """engine/src/ai/aidef.c hard-codes the record sizes and limits of ai.TABLES: compare the source text."""
    src = (ROOT / "engine/src/ai/aidef.c").read_text()
    body = src[src.index("TAB[AIDEF_TABLES] = {"):]
    body = body[:body.index("};")]
    pairs = [(int(a), int(b)) for a, b in re.findall(r"\{\s*(\d+),\s*(\d+)\s*\}", body)]
    assert pairs == [(rec, mx) for _, rec, mx in ai.TABLES], pairs
    hdr = (ROOT / "engine/src/ai/aidef.h").read_text()
    enum = hdr[hdr.index("AIDEF_T_ACTORS = 0"):hdr.index("} aidef_table;")]
    names = re.findall(r"AIDEF_T_([A-Z_]+)", enum)
    assert [n.lower() for n in names] == [n for n, _, _ in ai.TABLES], names
    assert hdr.count("AIDEF_ATOM_") >= len(ai.ATOM_TYPES) and hdr.count("AIDEF_STATE_") >= len(ai.ACTOR_STATES)
    for nm in ai.ACTOR_TYPES[:3] + ai.ATOM_TYPES[-2:]:
        assert nm in src, nm


def check_problems() -> None:
    # dangling indices become -1 and are reported
    s = collect(build_synthetic({"actor_type": 9, "maneuver": 7, "start_cmd": 50, "part_enc": 40, "line_part": 5}))
    assert s.squads[0].vals["actor_type"] == -1 and s.squads[0].vals["maneuver_squad"] == -1
    assert s.starts[0].vals["command_list"] == -1 and s.parts[0].vals["encounter_index"] == -1
    assert s.lines[1].vals["participant"] == -1
    txt = "\n".join(s.problems)
    for key in ("actor type: index 9", "maneuver squad: index 7", "start command list: index 50", "encounter index 40",
                "participant 5"):
        assert key in txt, (key, txt)
    # the clean world reports only the stale-bytes variant
    assert [p for p in collect(build_synthetic()).problems if "stale bytes" not in p] == []


def expect_error(fn, what: str) -> None:
    try:
        fn()
    except ai.AiError:
        return
    raise AssertionError(f"{what}: no AiError")


def check_corrupt() -> None:
    expect_error(lambda: collect(build_synthetic({"recordings": 2})), "recording references")
    r = build_synthetic()
    sb = r.block("scn")
    r.poke(sb + ai.SCN_ENCOUNTERS, "<II", 3, 0xFFFFFFF0)          # encounter block outside the tag region
    expect_error(lambda: collect(r), "encounters outside")
    r = build_synthetic()
    r.poke(r.block("act_a") + 0x14, "<H", 16)                            # actor type 16
    expect_error(lambda: collect(r), "actor type")
    r = build_synthetic()
    r.poke(r.block("scn") + ai.SCN_PALETTE, "<II", 3, 0xFFFFFFF0)
    expect_error(lambda: collect(r), "palette outside")
    # a palette entry of the wrong group is dropped; a NaN position becomes 0 and is reported
    r = build_synthetic()
    pal_addr = struct.unpack_from("<I", r.region, r.block("scn") + ai.SCN_PALETTE + 4)[0] - BASE
    r.region[pal_addr:pal_addr + 16] = struct.pack("<4sIII", b"pbac", 0, 0, TAG("act_a"))
    s = collect(r)
    assert s.palette[0] == 0 and any("not resolved" in p for p in s.problems)
    r = build_synthetic()
    enc = struct.unpack_from("<I", r.region, r.block("scn") + ai.SCN_ENCOUNTERS + 4)[0] - BASE
    sq = struct.unpack_from("<I", r.region, enc + 0x84)[0] - BASE
    st = struct.unpack_from("<I", r.region, sq + 0xD4)[0] - BASE
    struct.pack_into("<I", r.region, st, 0x7FC00000)
    s = collect(r)
    assert s.starts[0].vals["pos"][0] == 0.0 and any("non-finite" in p for p in s.problems)
    # a float word of the actor that is not finite is reported
    r = build_synthetic()
    struct.pack_into("<I", r.region, r.block("act_a") + 0x300, 0x7F800000)
    assert any("non-finite float word" in p for p in collect(r).problems)
    # build() refuses unsorted ids and oversize tables
    s = collect(build_synthetic())
    s.actors = s.actors[::-1]
    expect_error(lambda: ai.build(s), "unsorted actors")
    # parse_section refuses damaged sections
    blob = ai.build(collect(build_synthetic()))
    for bad in (blob[:100], b"XXXX" + blob[4:], blob + bytes(16)):
        expect_error(lambda: ai.parse_section(bad), "damaged section")


# ---- real maps -------------------------------------------------------------------------------------------
def check_real_maps() -> None:
    maps = ROOT / "assets" / "decompressed"
    found = []
    totals = {"nonfloat": 0}
    for n in ("a10", "a30", "a50", "b30", "b40", "c10", "c20", "c40", "d20", "d40"):
        path_ = maps / f"{n}.map"
        if not path_.exists():
            continue
        found.append(n)
        m = texconv.MapData.open(path_)
        s = ai.collect_map(m)
        sec = ai.parse_section(ai.build(s))
        assert s.encounters and s.squads and s.actors and s.variants and s.palette, n
        # actors: types, perception, cached values (the proofs of the units)
        for a in sec["actors"]:
            assert 0 <= a["type"] <= 15, (n, a["type"])
            assert 0.0 < a["max_vision_distance"] <= 100.0 and 0.0 < a["hearing_distance"] <= 100.0
            assert 0.0 <= a["peripheral_distance"] <= 100.0
            assert 0.0 < a["central_vision_angle"] <= a["max_vision_angle"] + 1e-5
            if a["central_vision_angle"] < 1.99:                 # two actors (a mounted weapon and an immobile infantry one) use wide-open sentinels
                assert a["max_vision_angle"] <= a["peripheral_vision_angle"] <= math.pi + 1e-5, (n, a["peripheral_vision_angle"])
            assert 0.0 <= a["notice_projectile_chance"] <= 1.0 and 0.0 <= a["notice_vehicle_chance"] <= 1.0
            for t, inv in (("combat", "inv_combat"), ("guard", "inv_guard"), ("non_combat", "inv_non_combat")):
                tt, ii = a[t + "_perception_time"], a[inv + "_perception_time"]
                assert tt >= 0.0 and abs(ii - (1.0 / (tt * 30.0) if tt > 0 else 0.0)) < 1e-6, (n, t, tt, ii)
            for i in range(2):
                assert 0.0 <= a["max_aiming_deviation"][i] <= math.pi + 1e-5 and 0.0 <= a["max_looking_deviation"][i] <= math.pi
                assert abs(math.cos(a["max_aiming_deviation"][i]) - a["cos_aiming_deviation"][i]) < 1e-6
                assert abs(math.cos(a["max_looking_deviation"][i]) - a["cos_looking_deviation"][i]) < 1e-6
            assert all(0.0 <= x <= math.pi / 2 + 1e-5 for x in a["looking_angles"])
            it = a["idle_times"]
            assert all(0.0 <= it[2 * i] <= it[2 * i + 1] <= 60.0 for i in range(10)), (n, it)
            assert all(0.0 <= x <= 1.0 for x in a["fractions_80"])
            if a["angle_9c"]:
                assert abs(math.cos(a["angle_9c"]) - a["angle_9c_cos"]) < 1e-6
        vtags = {tid for tid, _ in s.variants}
        atags = {tid for tid, _ in s.actors}
        for v in sec["variants"]:
            assert v["actor_tag"] == 0 or v["actor_tag"] in atags, n
            assert v["major_variant_tag"] == 0 or v["major_variant_tag"] in vtags
            for key, tag in (("unit_tag", None), ("weapon_tag", "weap")):
                if v[key]:
                    assert m.by_id[v[key]].group in (("bipd",) if key == "unit_tag" else ("weap",))
            assert 0.0 <= v["max_firing_distance"] <= 150.0 and 0.0 <= v["rate_of_fire"] <= 30.0
            assert 0.0 <= v["projectile_error"] <= math.pi / 4 and 0.0 <= v["death_fire_wildly_chance"] <= 1.0
            for key in ("target_tracking", "target_leading", "grenade_chance", "special_fire_chance",
                        "dont_drop_grenades_chance", "initial_crouch_chance"):
                assert 0.0 <= v[key] <= 1.0, (n, key, v[key])
            for key in ("crouch_time", "run_time", "first_burst_delay", "desired_combat_range", "burst_return_length",
                        "burst_duration", "burst_separation", "grenade_ranges", "drop_weapon_loaded"):
                lo, hi = v[key]
                assert 0.0 <= lo <= hi, (n, key, lo, hi)
            assert v["desired_combat_range"][1] <= 100.0 and v["grenade_ranges"][1] <= 100.0
            assert 0.0 <= v["burst_origin_angle"] <= math.pi and 0.0 <= v["burst_return_angle"] <= math.pi
            assert 0.0 <= v["body_vitality"] <= 1000.0 and 0.0 <= v["shield_vitality"] <= 1000.0
            assert v["grenade_kind"][0] <= 1 and v["grenade_count"][0] <= v["grenade_count"][1] <= 10
            assert v["drop_weapon_ammo"][0] <= v["drop_weapon_ammo"][1]
            assert 0.0 <= v["grenade_velocity"] <= 1.0                 # wu per tick
            assert v["movement_type"] <= 2 and v["special_fire"][0] <= 2 and v["special_fire"][1] <= 3
        # the palette entries are variants of the map
        assert all(t in vtags for t in s.palette), n
        # indices and ranges
        enc = sec["encounters"]
        assert sum(e["squad_count"] for e in enc) == len(s.squads)
        for e in enc:
            assert -1 <= e["precomputed_bsp"] < 32 and 0 <= e["manual_bsp"] < 32 and e["team"] < len(ai.TEAMS)
            assert not e["flags"] & 2, (n, e["flags"])                 # "respawn enabled" is never set
        nclist = len(s.cmdlists)
        for q in sec["squads"]:
            assert -1 <= q["actor_type"] < len(s.palette) and q["initial_state"] <= 11 and q["return_state"] <= 11
            assert q["normal_count"] <= 100 and q["insane_count"] <= 100 and q["unique_leader"] <= 4 and q["major_upgrade"] <= 4
            assert q["delay_time"] >= 0.0 and q["delay_time"] <= 1000.0
        for st in sec["starts"]:
            assert -math.pi - 1e-5 <= st["facing"] <= math.pi + 1e-5 and -1 <= st["command_list"] < nclist
            assert all(abs(c) < 10000.0 for c in st["pos"])
        for mv in sec["moves"]:
            assert -math.pi - 1e-5 <= mv["facing"] <= math.pi + 1e-5 and mv["time_min"] <= mv["time_max"] and mv["surface"] >= -1
        for f in sec["firings"]:
            assert f["surface"] >= -1 and all(abs(c) < 10000.0 for c in f["pos"])
        anim_n, script_n, rec_n = len(s.anims), len(s.scripts), 0
        for c in sec["commands"]:
            assert c["type"] <= 27 and c["type"] != 14                 # no recording atoms: there are no recordings
            if c["type"] == 13:
                assert -1 <= c["animation"] < anim_n, (n, c["animation"])
            if c["type"] == 12:
                assert -1 <= c["script"] < script_n
        for cl in sec["cmdlists"]:
            assert cl["first_command"] + cl["command_count"] <= len(sec["commands"])
            for k in range(cl["command_count"]):
                cm = sec["commands"][cl["first_command"] + k]
                if cm["type"] == 20:
                    assert cm["command"] < cl["command_count"] or cm["command"] == -1, (n, cl["name"])
        for cv in sec["convs"]:
            for k in range(cv["participant_count"]):
                p = sec["parts"][cv["first_participant"] + k]
                if p["encounter_index"] >= 0:
                    assert ai.unitconv.cstring(sec["encounters"][p["encounter_index"]]["name"]) == \
                        ai.unitconv.cstring(p["encounter_name"]).strip(), n
        for ln in sec["lines"]:
            for t in ln["sounds"]:
                assert t == 0 or m.by_id[t].group == "snd!"
        # roots
        models = ai.referenced_models(path_)
        anims = ai.referenced_anims(path_)
        sounds = ai.referenced_sounds(path_)
        assert models and anims and all(m.by_id[t].group == "mode" for t in models)
        assert all(m.by_id[t].group == "antr" for t in anims) and all(m.by_id[t].group == "snd!" for t in sounds)
    print(f"  real maps converted: {', '.join(found)}")


def main():
    r = build_synthetic()
    s = collect(r)
    check_images(s)
    check_scenario(s)
    check_roots(r)
    blob = check_serialisation(s)
    check_c_tables_match()
    check_problems()
    check_corrupt()
    check_real_maps()
    # a pack with the world's AIDT and an empty second section
    empty = ai.AiSet([], [], [])
    pack = hp2.build("test", [("AIDT", blob), ("AID1", ai.build(empty))])
    got = hp2.parse(pack)
    assert len(ai.parse_section(got.read("AID1"))["encounters"]) == 0 and len(ai.parse_section(got.read("AIDT"))["squads"]) == 3
    if os.environ.get("AI_FIXTURE"):
        Path(os.environ["AI_FIXTURE"]).write_bytes(pack)
    print("ok")


if __name__ == "__main__":
    main()
