"""ASTB conversion on a synthetic map: model_animations tags with units / weapon classes / weapon types, first person
weapons, vehicles, devices, object overlays, unit damage, variant chains, sound and key events are written as Xbox tag
structs, converted, read back with the reference reader and checked against hand-written expectations. Also: the slot
tables against engine/src/world/ansm.h, error handling for damaged tags, `referenced_anims`, the `animations_needed`
hook of animconv (indices, names and frame counts unchanged, stubs without pose data) and the CLI. Optional checks run
on the real campaign maps in assets/decompressed (skipped when missing): index ranges, name agreement of every slot,
the ANIM section's animation names and loop flags equal the ASTB ones, all ten maps convert.

Set ANSM_FIXTURE=<path> to also write the pack (ASTB + AEXP: the digest of the reference walk, see
ansmconv.digest) for the C reader test (engine/tests/test_ansm.c)."""
import contextlib
import io
import os
import re
import struct
import sys
import tempfile
import time
from pathlib import Path

sys.path[:0] = [str(Path(__file__).resolve().parents[1]), str(Path(__file__).resolve().parent)]
from halo2ps2 import animconv as ac  # noqa: E402
from halo2ps2 import ansmconv as am  # noqa: E402
from halo2ps2 import hp2  # noqa: E402
import test_animconv as tac  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
HEADER = ROOT / "engine" / "src" / "world" / "ansm.h"
MAPS = ("a10", "a30", "a50", "b30", "b40", "c10", "c20", "c40", "d20", "d40")


# ---- Xbox antr writer ---------------------------------------------------------------------------------------------------
class Tag:
    """A tag data block at absolute address `addr`: a 0x80 byte struct followed by whatever `put` adds."""

    def __init__(self, addr: int):
        self.addr = addr
        self.buf = bytearray(0x80)

    def put(self, raw: bytes, align: int = 4) -> int:
        while len(self.buf) % align:
            self.buf.append(0)
        at = self.addr + len(self.buf)
        self.buf += raw
        return at

    def refl(self, off: int, count: int, addr: int) -> None:
        struct.pack_into("<III", self.buf, off, count, addr if count else 0, 0)


def refl_bytes(count: int, addr: int) -> bytes:
    return struct.pack("<III", count, addr if count else 0, 0)


def u16_list(tag: Tag, slots) -> bytes:
    """A reflexive (12 bytes) of i16 animation indices."""
    return refl_bytes(len(slots), tag.put(struct.pack("<%dh" % len(slots), *slots), 2) if slots else 0)


def cstr32(s: str) -> bytes:
    return s.encode().ljust(32, b"\0")


def aim24(yr=0.0, yl=0.0, yrc=0, ylc=0, pd=0.0, pu=0.0, pdc=0, puc=0) -> bytes:
    return struct.pack("<ffhhffhh", yr, yl, yrc, ylc, pd, pu, pdc, puc)


def small_parts(fc: int):
    """Uncompressed animation data of one node (so animconv can convert the graph too)."""
    rot = [tac.axis_angle((0, 0, 1), 0.1 * f) for f in range(fc)]
    return tac.uncompressed_parts(1, fc, [rot], [(0.0, 0.0, 0.5)], [1.0])


def anim_record(a: dict, ptrs) -> bytes:
    parts = small_parts(a["fc"])
    b = bytearray(180)
    b[:32] = a["name"].encode().ljust(32, b"\0")
    struct.pack_into("<hhhhI", b, 0x20, a.get("type", 0), a["fc"], parts["frame_size"], 0, 0xABCD)
    struct.pack_into("<hh", b, 0x2C, 1, a.get("loop", 0))
    struct.pack_into("<hhhHhh", b, 0x34, a.get("key", 0), a.get("key2", 0), a.get("next", -1), a.get("flags", 0),
                     a.get("sound", -1), a.get("sound_frame", -1))
    struct.pack_into("<bb", b, 0x40, a.get("foot_l", 0), a.get("foot_r", 0))
    struct.pack_into("<hf", b, 0x42, a.get("main", a["index"]), a.get("rel", 1.0))
    b[0x48:0x5C] = tac.td(0, 0)
    struct.pack_into("<2I", b, 0x5C, parts["trans_flags"], 0)
    struct.pack_into("<2I", b, 0x6C, parts["rot_flags"], 0)
    struct.pack_into("<2I", b, 0x7C, parts["scale_flags"], 0)
    b[0x8C:0xA0] = tac.td(len(parts["default"]), ptrs["default"])
    b[0xA0:0xB4] = tac.td(len(parts["frame_data"]), ptrs["frame"])
    return bytes(b)


def antr_tag(addr: int, anims: list[dict], objects=(), units=(), wobjs=(), vehicles=(), devices=(), fps=(), damage=(),
             sounds=()) -> bytes:
    """units: [(label, aim, slots, ik, [(class, grip, hold, aim, slots, ik, [(type label, slots)])])];
    vehicles: [(yaw tuple, slots, [(mass point, anim, ext, comp)])]; devices / wobjs / fps: [slots]."""
    t = Tag(addr)
    recs = []
    for i, a in enumerate(anims):
        a["index"] = i
        parts = small_parts(a["fc"])
        ptrs = {"default": t.put(parts["default"]) if parts["default"] else 0,
                "frame": t.put(parts["frame_data"])}
        recs.append(anim_record(a, ptrs))
    t.refl(ac.ANTR_ANIM_AT, len(recs), t.put(b"".join(recs)))
    t.refl(am.ANTR_OBJECTS, len(objects), t.put(b"".join(struct.pack("<hHH14x", *o) for o in objects)))
    t.refl(am.ANTR_SOUNDS, len(sounds), t.put(b"".join(tac.fourcc_ref("snd!", s) + bytes(4) for s in sounds)))
    t.refl(am.ANTR_DAMAGE, len(damage), t.put(struct.pack("<%dh" % len(damage), *damage), 2) if damage else 0)
    urecs = []
    for label, uaim, uslots, uik, classes in units:
        crecs = []
        for cname, grip, hold, caim, cslots, cik, types in classes:
            trecs = [cstr32(tl) + bytes(16) + u16_list(t, ts) for tl, ts in types]
            ikb = b"".join(cstr32(m) + cstr32(a_) for m, a_ in cik)
            crecs.append(cstr32(cname) + cstr32(grip) + cstr32(hold) + caim + bytes(32) + u16_list(t, cslots) +
                         refl_bytes(len(cik), t.put(ikb) if cik else 0) +
                         refl_bytes(len(trecs), t.put(b"".join(trecs)) if trecs else 0))
            assert len(crecs[-1]) == 188
        uikb = b"".join(cstr32(m) + cstr32(a_) for m, a_ in uik)
        urecs.append(cstr32(label) + uaim + bytes(8) + u16_list(t, uslots) +
                     refl_bytes(len(uik), t.put(uikb) if uik else 0) +
                     refl_bytes(len(crecs), t.put(b"".join(crecs)) if crecs else 0))
        assert len(urecs[-1]) == 100
    t.refl(am.ANTR_UNITS, len(urecs), t.put(b"".join(urecs)))
    vrecs = []
    for yaw, vslots, susp in vehicles:
        sb = b"".join(struct.pack("<HHff8x", *s) for s in susp)
        vrecs.append(struct.pack("<ffhh", *yaw) + bytes(80) + u16_list(t, vslots) +
                     refl_bytes(len(susp), t.put(sb) if susp else 0))
        assert len(vrecs[-1]) == 116
    t.refl(am.ANTR_VEHICLES, len(vrecs), t.put(b"".join(vrecs)))
    t.refl(am.ANTR_DEVICES, len(devices), t.put(b"".join(bytes(84) + u16_list(t, s) for s in devices)))
    t.refl(am.ANTR_WEAPONS, len(wobjs), t.put(b"".join(bytes(16) + u16_list(t, s) for s in wobjs)))
    t.refl(am.ANTR_FP, len(fps), t.put(b"".join(bytes(16) + u16_list(t, s) for s in fps)))
    return bytes(t.buf)


def weapon_tag(anim_id: int, fp_id: int) -> bytes:
    out = bytearray(0x500)
    out[ac.OBJ_ANIM_AT:ac.OBJ_ANIM_AT + 16] = tac.fourcc_ref("antr", anim_id) if anim_id else bytes(16)
    out[am.WEAPON_FP_ANIM:am.WEAPON_FP_ANIM + 16] = tac.fourcc_ref("antr", fp_id) if fp_id else bytes(16)
    return bytes(out)


# ---- the synthetic world ---------------------------------------------------------------------------------------------------
SND_A, SND_B = 0xE0010001, 0xE0020002


def A(name, fc=10, **kw):
    return dict(name=name, fc=fc, **kw)


BIPED_ANIMS = [
    A("stand rifle idle", 40),                                              # 0
    A("stand rifle move-front", 24, sound=0, sound_frame=0, foot_l=3, foot_r=15),   # 1
    A("stand rifle ar fire-1", 6, key=2),                                   # 2
    A("stand rifle ar reload-1", 50, sound=1, sound_frame=4),              # 3
    A("stand rifle ar melee", 18, key=6, key2=11),                          # 4
    A("stand rifle aim-still", 9, type=1),                                  # 5  (3 x 3 overlay)
    A("stand rifle throw-grenade", 30, key=14),                             # 6
    A("alert unarmed idle%1", 55, next=8, rel=0.33, main=7),                # 7
    A("alert unarmed idle%2", 100, next=9, rel=0.67, main=7),               # 8
    A("alert unarmed idle%3", 90, next=-1, rel=1.0, main=7),                # 9
    A("stand airborne-dead", 12),                                           # 10
    A("s-ping front gut", 9, type=1),                                       # 11
    A("s-ping left chest", 9, type=1),                                      # 12
    A("crouch rifle idle", 40),                                             # 13
    A("pilot_fidget01", 60),                                                # 14 free-form: a script plays it by name
    A("stand rifle dive-left", 20),                                         # 15 dead: named like an action, no slot
    A("stand rifle ready", 20, sound=0, sound_frame=5),                     # 16
    A("stand rifle sg chamber-1", 12),                                      # 17
    A("stand rifle land-hard", 12),                                         # 18 reached through an alias slot
]
BIPED_DAMAGE = [-1] * 176
for _i in range(0, 11):
    BIPED_DAMAGE[_i] = 11                    # shield ping, front: every region plays 's-ping front gut'
BIPED_DAMAGE[11 + 1] = 12                    # shield ping, left, chest
BIPED_DAMAGE[44 * 3 + 10] = 11               # health kill, front, right foot (reuses a neighbour)


def slots_of(**kw) -> list[int]:
    """Build a class slot list from name -> animation index."""
    out: list[int] = []
    for name, a in kw.items():
        slot = am.slot_by_name(am.L_CLASS, name.replace("_", "-"))
        assert slot >= 0, name
        out += [-1] * (slot + 1 - len(out))
        out[slot] = a
    return out


def type_slots(**kw) -> list[int]:
    out: list[int] = []
    for name, a in kw.items():
        slot = am.slot_by_name(am.L_TYPE, name.replace("_", "-"))
        assert slot >= 0, name
        out += [-1] * (slot + 1 - len(out))
        out[slot] = a
    return out


BIPED_UNITS = [
    ("stand", aim24(1.5, 1.5, 1, 1, 0.7, 0.7, 1, 1), [10], [("right hand", "grip")],
     [("rifle", "", "right hand", aim24(1.2, 1.2, 1, 1, 0.5, 0.5, 1, 1),
       slots_of(idle=0, move_front=1, aim_still=5, throw_grenade=6, ready=16, land_soft=18, **{"land_hard": 18}),
       [("left hand", "gun left hand")],
       [("ar", type_slots(reload_1=3, fire_1=2, melee=4)), ("sg", type_slots(chamber_1=17)), ("pb", [])]),
      ("unarmed", "", "", aim24(), slots_of(idle=7), [], [("", [])])]),
    ("crouch", aim24(), [], [], [("rifle", "", "", aim24(), slots_of(idle=13), [], [("ar", type_slots(fire_1=2))])]),
]
FP_ANIMS = [A("first-person idle", 40), A("first-person fire-1", 5), A("first-person ready", 30),
            A("first-person reload-full", 60), A("first-person overheated", 70)]
FP_SLOTS = [[0, -1, 1, -1, -1, -1, -1, -1, 3, 4, 2]]
VEH_ANIMS = [A("vehicle steering", 3), A("vehicle ground-speed", 3), A("suspension left front", 5), A("firing", 4)]
VEH = [((0.6, 0.6, 1, 1), [0, -1, -1, -1, -1, 1], [(11, 2, -0.3, -0.1), (12, 2, -0.3, -0.12)])]
DEV_ANIMS = [A("device position", 3)]
WOBJ_ANIMS = [A("fire-1", 4), A("reload-2", 30)]
SCEN_ANIMS = [A("idle", 8)]
CINE_ANIMS = [A("cutscene walk", 20), A("stand rifle idle", 30)]
TAGS = dict(biped=2, fp=4, veh=6, dev=8, wobj=10, scen=12, cine=14, cine_obj=15)


def tid(i: int) -> int:
    return tac.tag_id(i)


def build_world(path: Path) -> None:
    specs = [
        ("scnr", "levels/test/test", lambda a: bytes(0x40)),
        ("mode", "characters/test/test", lambda a: tac.model_block(a, ["root"], [(0, 0, 0, 1)], [(0, 0, 0)], 0xABCD)),  # 1
        ("antr", "characters/test/biped", lambda a: antr_tag(a, BIPED_ANIMS, units=BIPED_UNITS, damage=BIPED_DAMAGE,
                                                              sounds=[SND_A, SND_B], objects=[(14, 1, 0)])),         # 2
        ("bipd", "characters/test/biped", lambda a: tac.object_block(tid(1), tid(2))),                                 # 3
        ("antr", "weapons/test/fp", lambda a: antr_tag(a, FP_ANIMS, fps=FP_SLOTS)),                                    # 4
        ("weap", "weapons/test/weapon", lambda a: weapon_tag(tid(10), tid(4))),                                        # 5
        ("antr", "vehicles/test/veh", lambda a: antr_tag(a, VEH_ANIMS, vehicles=VEH, objects=[(3, 3, 0)])),          # 6
        ("vehi", "vehicles/test/veh", lambda a: tac.object_block(tid(1), tid(6))),                                     # 7
        ("antr", "levels/test/door", lambda a: antr_tag(a, DEV_ANIMS, devices=[[0, -1]])),                            # 8
        ("mach", "levels/test/door", lambda a: tac.object_block(tid(1), tid(8))),                                      # 9
        ("antr", "weapons/test/weapon", lambda a: antr_tag(a, WOBJ_ANIMS, wobjs=[[-1] * 4 + [1, -1, -1, -1, -1, 0]])),  # 10
        ("antr", "scenery/test/prop", lambda a: antr_tag(a, SCEN_ANIMS)),                                              # 11
        ("scen", "scenery/test/prop", lambda a: tac.object_block(tid(1), tid(11))),                                    # 12
        ("antr", "cinematics/test/cine", lambda a: antr_tag(a, CINE_ANIMS, units=[("stand", aim24(), [], [], [
            ("rifle", "", "", aim24(), slots_of(idle=1), [], [])])])),                                                 # 13
    ]
    # tag ids are index based (tac.tag_id): the antr tags sit at indices 2, 4, 6, 8, 10, 11, 13
    tac.build_map(path, specs)


ANTR_IDS = {"biped": tid(2), "fp": tid(4), "veh": tid(6), "dev": tid(8), "wobj": tid(10), "scen": tid(11), "cine": tid(13)}


# ---- tests --------------------------------------------------------------------------------------------------------------------
def test_slot_tables() -> None:
    assert [len(t) for t in am.SLOT_NAMES] == [9, 51, 30, 28, 10, 8, 2]
    for level, names in enumerate(am.SLOT_NAMES):
        named = [n for n in names if n]
        assert len(named) == len(set(named)), f"duplicate slot name at level {level}"
        for slot, n in enumerate(names):
            assert am.slot_name(level, slot) == n and (not n or am.slot_by_name(level, n) == slot)
    assert am.slot_by_name(am.L_CLASS, "nope") == -1 and am.slot_name(am.L_CLASS, 99) == ""
    assert am.action(am.L_FP, 8) == 0x308
    # engine/src/world/ansm.h defines the same tables through X-macros
    text = HEADER.read_text()
    macros = {"TYPE": am.L_TYPE, "CLASS": am.L_CLASS, "UNIT": am.L_UNIT, "FP": am.L_FP, "WOBJ": am.L_WOBJ,
              "VEHICLE": am.L_VEHICLE, "DEVICE": am.L_DEVICE}
    for name, level in macros.items():
        m = re.search(r"#define ANSM_%s_SLOTS\(X\)(.*?)\n(?:#define|\n)" % name, text, re.S)
        assert m, name
        entries = re.findall(r'X\((\w+), (\d+), "([^"]*)"\)', m.group(1))
        got = {int(slot): label for _, slot, label in entries}
        want = {i: n for i, n in enumerate(am.SLOT_NAMES[level]) if n}
        assert got == want, f"{name}: header and ansmconv.py slot names differ: {set(got.items()) ^ set(want.items())}"


def test_build_and_read(tmp: Path):
    mp = tmp / "world.map"
    build_world(mp)
    problems: list[str] = []
    data = am.build_section(mp, everything=True, problems=problems)
    assert problems == [], problems
    sec = am.parse_section(data)
    assert [g.tag_id for g in sec.graphs] == sorted(ANTR_IDS.values())
    g = sec.find_graph(ANTR_IDS["biped"])
    assert [a.name for a in g.anims] == [a["name"] for a in BIPED_ANIMS]
    # animation info
    a = g.anims
    assert (a[0].frame_count, a[0].ticks, a[0].fps, a[0].loops, a[0].type) == (40, 40, 30, True, 0)
    assert a[1].loops and not a[2].loops and a[5].type == 1 and not a[5].loops           # aim-still: overlay
    assert (a[1].sound, a[1].sound_frame, a[1].foot_left, a[1].foot_right) == (0, 0, 3, 15)
    assert (a[3].sound, a[3].sound_frame) == (1, 4) and (a[2].sound, a[2].sound_frame) == (-1, -1)
    assert (a[4].key, a[4].key2, a[6].key) == (6, 11, 14)
    assert [x.next for x in a[7:10]] == [8, 9, -1] and [x.main for x in a[7:10]] == [7, 7, 7]
    assert [round(x.weight, 2) for x in a[7:10]] == [0.33, 0.67, 1.0]
    assert g.sounds == [SND_A, SND_B]
    # name lookup: exact, variant head, absent
    assert g.find_anim("stand rifle ar reload-1") == 3 and g.find_anim("alert unarmed idle") == 7
    assert g.find_anim("alert unarmed idle%2") == 8 and g.find_anim("stand rifle") == -1 and g.find_anim("zzz") == -1
    assert g.find_anim("pilot_fidget01") == 14 and g.find_anim("") == -1
    # variants by cumulative weight
    assert [g.pick_variant(8, r) for r in (0.0, 0.32, 0.34, 0.66, 0.68, 0.99)] == [7, 7, 8, 8, 9, 9]
    assert g.pick_variant(0, 0.5) == 0
    # unit / class / type tables
    assert [u.name for u in g.units] == ["stand", "crouch"]
    assert g.find_unit("crouch") == 1 and g.find_unit("nope") == -1
    assert g.find_class(0, "rifle") == 0 and g.find_class(0, "unarmed") == 1 and g.find_class(0, "pistol") == -1
    assert g.find_type(0, 0, "ar") == 0 and g.find_type(0, 0, "sg") == 1 and g.find_type(0, 0, "xx") == -1
    assert g.find_type(0, 1, "") == 0
    assert g.units[0].aim[:4] == (1.5, 1.5, 1, 1) and g.units[0].classes[0].aim[0] == 1.2000000476837158
    assert g.units[0].ik == [("right hand", "grip")] and g.units[0].classes[0].ik == [("left hand", "gun left hand")]
    assert g.units[0].classes[0].hold == "right hand" and g.units[0].classes[0].grip == ""
    L = g.lookup
    assert L("stand", "rifle", "ar", am.action(am.L_TYPE, 0)) == 3          # reload-1
    assert L("stand", "rifle", "ar", am.action(am.L_TYPE, 4)) == 2          # fire-1
    assert L("stand", "rifle", "ar", am.action(am.L_TYPE, 8)) == 4          # melee
    assert L("stand", "rifle", "sg", am.action(am.L_TYPE, 2)) == 17         # chamber-1
    assert L("stand", "rifle", "ar", am.action(am.L_TYPE, 1)) == -1         # reload-2: none
    assert L("stand", "rifle", "zz", am.action(am.L_TYPE, 4)) == -1         # unknown type
    assert L("stand", "rifle", "", am.action(am.L_CLASS, am.slot_by_name(am.L_CLASS, "idle"))) == 0
    assert L("stand", "rifle", "", am.action(am.L_CLASS, am.slot_by_name(am.L_CLASS, "move-front"))) == 1
    assert L("stand", "unarmed", "", am.action(am.L_CLASS, 0)) == 7
    assert L("crouch", "rifle", "", am.action(am.L_CLASS, 0)) == 13
    assert L("stand", "rifle", "", am.action(am.L_CLASS, am.slot_by_name(am.L_CLASS, "land-hard"))) == 18
    assert L("stand", "", "", am.action(am.L_UNIT, 0)) == 10                # airborne-dead
    assert L("stand", "", "", am.action(am.L_UNIT, 9)) == -1 and L("nope", "", "", am.action(am.L_UNIT, 0)) == -1
    assert L("stand", "rifle", "ar", 0x7F00) == -1                          # bad level
    assert g.find_class_for_type(0, "ar") == 0 and g.find_class_for_type(0, "pb") == -1 and \
        g.find_class_for_type(0, "") == -1
    # damage
    assert g.damage_anim(0, 0, 5) == 11 and g.damage_anim(0, 1, 1) == 12 and g.damage_anim(3, 0, 10) == 11
    assert g.damage_anim(0, 0, 11) == -1 and g.damage_anim(4, 0, 0) == -1 and g.damage_anim(1, 0, 0) == -1
    assert g.objects == [(14, 1, 0)]
    # first person / weapon object / vehicle / device
    fp = sec.find_graph(ANTR_IDS["fp"])
    assert fp.lookup(None, None, None, am.action(am.L_FP, 8)) == 3 and fp.lookup(None, None, None, am.action(am.L_FP, 2)) == 1
    assert fp.lookup(None, None, None, am.action(am.L_FP, 27)) == -1
    wo = sec.find_graph(ANTR_IDS["wobj"])
    assert wo.lookup(None, None, None, am.action(am.L_WOBJ, 9)) == 0 and wo.lookup(None, None, None, am.action(am.L_WOBJ, 4)) == 1
    v = sec.find_graph(ANTR_IDS["veh"])
    assert v.vehicles[0].yaw == (0.6000000238418579, 0.6000000238418579, 1, 1)
    assert v.lookup(None, None, None, am.action(am.L_VEHICLE, 5)) == 1 and v.vehicles[0].suspension[0][:2] == (11, 2)
    assert v.objects == [(3, 3, 0)]
    d = sec.find_graph(ANTR_IDS["dev"])
    assert d.lookup(None, None, None, am.action(am.L_DEVICE, 0)) == 0 and d.lookup(None, None, None, am.action(am.L_DEVICE, 1)) == -1
    assert sec.find_graph(0x12345) is None
    # the digest reads every table
    assert am.digest(sec) == am.digest(am.parse_section(am.build_section(mp, everything=True)))
    # default graph set: gameplay graphs only
    sel = am.parse_section(am.build_section(mp))
    assert {x.tag_id for x in sel.graphs} == {ANTR_IDS[k] for k in ("biped", "fp", "veh", "dev", "wobj", "scen")}
    assert am.referenced_anims(mp) == {x.tag_id for x in sel.graphs}
    assert am.referenced_sounds(mp) == set()          # SND_A / SND_B are not tags of this map
    one = am.parse_section(am.build_section(mp, tags=[ANTR_IDS["cine"]]))
    assert [x.tag_id for x in one.graphs] == [ANTR_IDS["cine"]]
    return mp, data, sec


def test_writer_properties(data: bytes, sec) -> None:
    # sorted, deduplicated, every offset aligned; the pack round trip
    body = struct.unpack_from("<I", data, 16)[0]
    assert struct.unpack_from("<I", data, 20)[0] == len(data) and len(data) % 16 == 0 and body % 16 == 0
    pack = hp2.build("test", [("ASTB", data)])
    assert am.parse_section(hp2.parse(pack).read("ASTB")).graphs[0].tag_id == sec.graphs[0].tag_id
    # the section is deterministic and a graph-less section is valid
    assert am.build_section_from_graphs([]) == am.build_section_from_graphs([])
    assert am.parse_section(am.build_section_from_graphs([])).graphs == []


def test_errors(tmp: Path) -> None:
    def build_with(anims, **kw):
        specs = [("scnr", "levels/test/test", lambda a: bytes(0x40)),
                 ("antr", "characters/test/bad", lambda a: antr_tag(a, anims, **kw))]
        p = tmp / "bad.map"
        tac.build_map(p, specs)
        return p

    anims = [A("stand rifle idle"), A("stand rifle move-front")]
    units = lambda slots, cs: [("stand", aim24(), [], [], [("rifle", "", "", aim24(), cs, [], [("ar", slots)])])]   # noqa: E731
    # an index outside the animation list is dropped and reported, not written
    probs: list[str] = []
    p = build_with(anims, units=units([0, 7], [1, 5]))
    sec = am.parse_section(am.build_section(p, everything=True, problems=probs))
    c = sec.graphs[0].units[0].classes[0]
    assert c.slots == [1] and c.types[0].slots == [0] and len(probs) == 2 and "outside 0..1" in probs[0], probs
    # a sound index outside the sound references
    probs = []
    p = build_with([A("a", sound=3, sound_frame=2), A("b")], sounds=[SND_A])
    sec = am.parse_section(am.build_section(p, everything=True, problems=probs))
    assert sec.graphs[0].anims[0].sound == -1 and sec.graphs[0].anims[0].sound_frame == -1 and "sound index 3" in probs[0]
    # a variant link outside the list
    probs = []
    p = build_with([A("a", next=9), A("b")])
    sec = am.parse_section(am.build_section(p, everything=True, problems=probs))
    assert sec.graphs[0].anims[0].next == -1 and any("next" in x for x in probs)
    # a main animation link that is -1 means the animation is its own head
    p = build_with([A("a", main=-1), A("b")])
    assert am.parse_section(am.build_section(p, everything=True)).graphs[0].anims[0].main == 0
    # structural damage raises
    for bad in (lambda: build_with([A("bad\x01name")]), lambda: build_with([A("a", fc=0)]),
                lambda: build_with([A("a", type=7)]), lambda: build_with([A("a")], units=[("st\x02", aim24(), [], [], [])])):
        try:
            am.build_section(bad(), everything=True)
        except ac.ConvertError:
            pass
        else:
            raise AssertionError("expected ConvertError")
    # a reflexive that points outside the tag region
    p = build_with(anims)
    t = ac.MapTags(p)
    t.region = bytearray(t.region)
    e = next(e for e in t.idx.entries if e.group == "antr")
    o = t.off(e.data_address)
    # damage reflexive count 3, address far away
    struct.pack_into("<II", t.region, o + am.ANTR_DAMAGE, 3, 0x70000000)
    try:
        am.read_graph(t, e)
    except ac.ConvertError:
        pass
    else:
        raise AssertionError("expected ConvertError for a wild pointer")
    # unknown group / tag
    try:
        am.load_graphs(ac.MapTags(p), tags=[0x1234])
    except ac.ConvertError:
        pass
    else:
        raise AssertionError("expected ConvertError for an unknown tag")
    # duplicate graph ids
    t = ac.MapTags(p)
    g = am.read_graph(t, next(e for e in t.idx.entries if e.group == "antr"))
    try:
        am.build_section_from_graphs([g, g])
    except ac.ConvertError:
        pass
    else:
        raise AssertionError("expected ConvertError for duplicate graphs")


def test_corrupt_python_reader(data: bytes) -> None:
    """The reference reader rejects (never crashes on) damaged sections: the C test does the byte-wise version."""
    import random
    rng = random.Random(3)
    for n in range(0, len(data), max(1, len(data) // 200)):
        try:
            am.parse_section(data[:n])
        except ac.ConvertError:
            continue
        raise AssertionError(f"accepted a section truncated to {n} bytes")
    ok = 0
    for _ in range(400):
        pos = rng.randrange(len(data))
        raw = bytearray(data)
        raw[pos] ^= 1 << rng.randrange(8)
        try:
            am.parse_section(bytes(raw))
            ok += 1
        except (ac.ConvertError, struct.error, IndexError, UnicodeDecodeError):
            pass
    assert ok < 400


def test_animations_needed(tmp: Path, mp: Path) -> None:
    """animconv's hook: indices, names, frame counts and loop flags of a stubbed conversion equal the full one."""
    full = ac.build_section(mp)
    refs = sorted(am.referenced_anims(mp))
    need = am.needed_animations(mp)
    assert ANTR_IDS["scen"] not in need                       # no state tables: converted in full
    assert need[ANTR_IDS["biped"]] == set(range(len(BIPED_ANIMS))) - {15}, need[ANTR_IDS["biped"]]
    assert am.dead_anims(am.load_graphs(mp, tags=[ANTR_IDS["biped"]])[0]) == {15}
    part = ac.build_section(mp, refs, animations_needed=need)
    full_sel = ac.build_section(mp, refs)
    s1, s2 = ac.parse_section(full_sel), ac.parse_section(part)
    assert [(g.tag_id, g.anim_count) for g in s1.graphs] == [(g.tag_id, g.anim_count) for g in s2.graphs]
    assert [(a.name, a.frame_count, a.type, a.loop_frame, a.flags & 3) for a in s1.anims] == \
        [(a.name, a.frame_count, a.type, a.loop_frame, a.flags & 3) for a in s2.anims]
    stubs = [a.name for a in s2.anims if a.flags & ac.ANIM_STUB]
    assert stubs == ["stand rifle dive-left"] and not any(a.flags & ac.ANIM_STUB for a in s1.anims)
    bi = s2.graph_index(ANTR_IDS["biped"])
    stub = next(a for a in s2.graph_anims(bi) if a.flags & ac.ANIM_STUB)
    assert stub.data_bytes == 4 and stub.root_off == 0 and stub.root_type == 0
    pose = s2.sample(bi, s2.graph_anims(bi).index(stub), 5.0)
    rest = s2.rest_pose(bi)
    assert pose[0][0] == tuple(rest.rot[0])
    # untouched animations sample identically
    for ai in (0, 1, 3):
        assert s1.sample(s1.graph_index(ANTR_IDS["biped"]), ai, 7.5) == s2.sample(bi, ai, 7.5)
    # a graph not in the dict, and the default (None), convert everything
    assert not any(a.flags & ac.ANIM_STUB for a in ac.parse_section(ac.build_section(mp, refs, animations_needed={})).anims)
    for bad in ({ANTR_IDS["biped"]: [99]}, {ANTR_IDS["biped"]: [-1]}):
        try:
            ac.build_section(mp, refs, animations_needed=bad)
        except ac.ConvertError:
            pass
        else:
            raise AssertionError("expected ConvertError for an index outside the graph")


def test_cli(tmp: Path, mp: Path) -> None:
    out = tmp / "world.ansm.hp2"
    log, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(log), contextlib.redirect_stderr(err):
        assert am.main([str(mp), "--all", "-o", str(out), "--info", "--verify", "--dump", "characters/test/biped"]) == 0
        assert am.main([str(out), "--info"]) == 0
        assert am.main([str(out), "--digest"]) == 0
        assert am.main([str(mp), "--info", "--tags", "weapons/test/fp"]) == 0
    text = log.getvalue()
    assert "7 graphs" in text and "0 structural problems" in text and "unit 'stand'" in text and "type 'ar'" in text
    assert re.search(r"^[0-9a-f]{8}$", text, re.M)
    assert "wrote" in text and am.parse_section(hp2.parse(out.read_bytes()).read("ASTB")).graphs
    # verify's survey on the synthetic graph: every referenced slot is named, the aliased one counts as an alias
    sv = am.survey_graph(am.load_graphs(mp, tags=[ANTR_IDS["biped"]])[0])
    assert (sv.refs[am.L_CLASS], sv.refs[am.L_TYPE], sv.refs[am.L_UNIT]) == (9, 5, 1)
    assert not sv.other and not sv.unnamed and sv.alias[am.L_CLASS] == 1      # land-soft slot -> a 'land-hard' animation
    assert (sv.damage_total, sv.damage_cat_ok, sv.damage_dir_ok) == (13, 12, 12)
    assert (sv.aim_ok, sv.aim_bad) == (1, 0)                                  # the 3 x 3 overlay of the rifle block
    with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
        try:
            am.main([str(mp)])
        except SystemExit as e:
            assert e.code == 2
        else:
            raise AssertionError("expected an argparse error")


# ---- real maps -----------------------------------------------------------------------------------------------------------
def real_map(name: str) -> Path | None:
    p = ROOT / "assets" / "decompressed" / f"{name}.map"
    return p if p.exists() else None


def check_real_maps() -> None:
    present = [m for m in MAPS if real_map(m)]
    if not present:
        print("skip: no real maps in assets/decompressed")
        return
    totals = {"graphs": 0, "anims": 0}
    for name in present:
        mp = real_map(name)
        t0 = time.time()
        tags = ac.MapTags(mp)
        gs = am.load_graphs(tags, everything=True)
        data = am.build_section_from_graphs(gs)
        sec = am.parse_section(data)
        assert am.parse_section(am.build_section(tags)) is not None
        # structural invariants of every graph, no problems dropped silently
        for g in gs:
            assert am.check_graph(g) == [], (name, g.path, am.check_graph(g)[:3])
        sv = am.Survey()
        for g in gs:
            am.survey_graph(g, sv)
        assert not sv.other, (name, sv.other.most_common(5))
        assert not sv.unnamed, (name, sv.unnamed)
        assert sv.max_slot[am.L_CLASS] < 51 and sv.max_slot[am.L_UNIT] < 30 and sv.max_slot[am.L_TYPE] < 9
        # ASTB names equal the ANIM converter's source names at every index (same graph, same order)
        for g, sg in zip(gs, sec.graphs):
            src = ac.read_graph(tags, tags.byid[g.tag_id])
            assert [a.name for a in src.anims] == [a.name for a in sg.anims], (name, g.path)
            assert [ac.loop_flags(a) & 1 for a in src.anims] == [x.flags & 1 for x in sg.anims], (name, g.path)
            assert [a.frame_count for a in src.anims] == [x.frame_count for x in sg.anims]
            assert [(a.next_anim) for a in src.anims] == [x.next for x in sg.anims]
        assert len(am.referenced_anims(tags)) <= len(gs)
        totals["graphs"] += len(gs)
        totals["anims"] += len(sec.graphs and [a for g in sec.graphs for a in g.anims])
        print(f"  {name}: {len(gs)} graphs, {sum(len(g.anims) for g in gs)} animations, {len(data)} bytes, {time.time() - t0:.2f} s")
    # the real a10: plausible state tables
    if real_map("a10"):
        t = ac.MapTags(real_map("a10"))
        sec = am.parse_section(am.build_section(t))
        paths = {e.tag_id: e.path for e in t.idx.entries if e.group == "antr"}
        by_path = {paths[g.tag_id]: g for g in sec.graphs}
        cy = by_path["characters\\cyborg\\cyborg"]
        u = cy.find_unit("stand")
        assert u >= 0 and cy.find_unit("crouch") >= 0 and cy.find_unit("W-driver") >= 0
        L = cy.lookup
        names = {k: (cy.anims[i].name if i >= 0 else None) for k, i in {
            "idle": L("stand", "rifle", "", am.action(am.L_CLASS, 0)),
            "move": L("stand", "rifle", "", am.action(am.L_CLASS, 8)),
            "jump": L("stand", "rifle", "", am.action(am.L_CLASS, 16)),
            "land": L("stand", "rifle", "", am.action(am.L_CLASS, 18)),
            "throw": L("stand", "rifle", "", am.action(am.L_CLASS, 20)),
            "aim": L("stand", "rifle", "", am.action(am.L_CLASS, 25)),
            "fire": L("stand", "rifle", "ar", am.action(am.L_TYPE, 4)),
            "reload": L("stand", "rifle", "ar", am.action(am.L_TYPE, 0)),
            "melee": L("stand", "rifle", "ar", am.action(am.L_TYPE, 8)),
            "crouch": L("crouch", "pistol", "", am.action(am.L_CLASS, 0)),
        }.items()}
        assert names == {"idle": "stand rifle idle", "move": "stand rifle move-front", "jump": "stand rifle airborne",
                         "land": "stand rifle land-hard", "throw": "stand rifle throw-grenade",
                         "aim": "stand rifle aim-still", "fire": "stand rifle ar fire-1",
                         "reload": "stand rifle ar reload-1", "melee": "stand rifle ar melee",
                         "crouch": "crouch pistol idle"}, names
        cls = cy.units[u].classes
        assert [c.name for c in cls][:5] == ["rifle", "pistol", "missile", "support", "unarmed"]
        assert cy.find_class_for_type(u, "pp") == cy.find_class(u, "pistol") and cy.find_class_for_type(u, "ar") == 0
        rl = cy.find_class_for_type(u, "rl")
        assert cls[rl].name == "missile"
        # the player's first person graph: ready / reload / fire / melee / throw by name
        fp = by_path["weapons\\pistol\\fp\\fp"]
        fnames = {k: fp.anims[fp.fps[0][s]].name for k, s in (("idle", 0), ("fire", 2), ("reload-empty", 7), ("reload-full", 8),
                                                              ("ready", 10), ("melee", 13), ("throw", 16))}
        assert fnames == {"idle": "first-person idle", "fire": "first-person fire-1", "reload-empty": "first-person reload-empty",
                          "reload-full": "first-person reload-full", "ready": "first-person ready",
                          "melee": "first-person melee", "throw": "first-person throw-grenade"}, fnames
        # the warthog: steering and ground speed, four suspension springs; variants have cumulative weights
        wh = by_path["vehicles\\warthog\\warthog"]
        assert wh.anims[wh.vehicles[0].slots[0]].name == "vehicle steering" and len(wh.vehicles[0].suspension) == 4
        idle = cy.find_anim("alert unarmed idle")
        assert idle >= 0 and cy.anims[idle].name == "alert unarmed idle%1" and cy.anims[idle].main == idle
        chain = [idle]
        while cy.anims[chain[-1]].next >= 0:
            chain.append(cy.anims[chain[-1]].next)
        assert len(chain) == 3 and abs(cy.anims[chain[-1]].weight - 1.0) < 1e-4
        # ANIM section: same indices for the cyborg graph (convert only that graph)
        cyid = next(i for i, p in paths.items() if p == "characters\\cyborg\\cyborg")
        asec = ac.parse_section(ac.build_section(real_map("a10"), [cyid]))
        aa = asec.graph_anims(0)
        assert [a.name for a in aa] == [a.name for a in cy.anims] and [a.flags & 1 for a in aa] == [a.flags & 1 for a in cy.anims]
        # the hook converts the cyborg graph with all the same indices
        need = am.needed_animations(t, [cyid])
        part = ac.parse_section(ac.build_section(real_map("a10"), [cyid], animations_needed=need))
        assert [(a.name, a.frame_count) for a in part.anims] == [(a.name, a.frame_count) for a in asec.anims]
        assert sum(1 for a in part.anims if a.flags & ac.ANIM_STUB) == len(am.dead_anims(am.load_graphs(t, [cyid])[0]))
    print(f"real maps ok ({len(present)} maps, {totals['graphs']} graphs, {totals['anims']} animations)")


def main() -> None:
    test_slot_tables()
    with tempfile.TemporaryDirectory() as td_:
        tmp = Path(td_)
        mp, data, sec = test_build_and_read(tmp)
        test_writer_properties(data, sec)
        test_errors(tmp)
        test_corrupt_python_reader(data)
        mp = tmp / "world.map"
        build_world(mp)
        test_animations_needed(tmp, mp)
        test_cli(tmp, mp)
        if os.environ.get("ANSM_FIXTURE"):
            Path(os.environ["ANSM_FIXTURE"]).write_bytes(
                hp2.build("test", [("ASTB", data), ("AEXP", struct.pack("<III", am.digest(sec), len(sec.graphs),
                                                                       sum(len(g.anims) for g in sec.graphs)))]))
    check_real_maps()
    print("ok")


if __name__ == "__main__":
    main()
