"""WEAP section on a synthetic map and a synthetic set (no game data needed): the tag readers (weapon with two
triggers, magazines, equipment, firing effects; projectile with material responses; damage effect; equipment;
grenades in the globals; first person animation timing), the plausibility rules, the section writer and its
reference parser, corrupt input.  Set WEAP_FIXTURE=<path> to also write the .hp2 the C reader test
(engine/tests/test_weapdef.c, which hard-codes the numbers of fixture_set()) opens.  Skip-if-missing checks
convert all ten real maps when assets/decompressed/ has them."""
import os
import struct
import sys
from pathlib import Path

sys.path[:0] = [str(Path(__file__).resolve().parents[1])]
from halo2ps2 import hp2, tagindex, texconv, weapconv as wc  # noqa: E402

BASE = tagindex.XBOX_TAG_BASE
ROOT = Path(__file__).resolve().parents[2]
MAPS = "a10 a30 a50 b30 b40 c10 c20 c40 d20 d40".split()

W_RIFLE, W_PLASMA, W_SHOTGUN = 0xE1760010, 0xE1770011, 0xE1780012
P_BULLET, P_BOLT, P_PELLET = 0xE1790020, 0xE17A0021, 0xE17B0022
D_BULLET, D_MELEE, D_EXPLOSION = 0xE17C0030, 0xE17D0031, 0xE17E0032
Q_AMMO, Q_GRENADE = 0xE17F0040, 0xE1800041
C_BOX = 0xE1810050


# ---- the synthetic set (engine/tests/test_weapdef.c hard-codes these numbers) -----------------------
def fixture_set() -> wc.WeapSet:
    s = wc.WeapSet()
    rifle = wc.Weapon(W_RIFLE, "rifle", model_tag=0xE1900001, fp_model_tag=0xE1900002, fp_anim_tag=0xE1910001,
                      hud_tag=0xE1920001, pickup_sound=0xE1930001, melee_damage=D_MELEE, flags=0, label="tr",
                      autoaim_angle=0.1, autoaim_range=25.0, near_reticle=4.0, far_reticle=16.0,
                      intersection_reticle=80.0, hud_message=7, sort_order=1, item_scale=1.5,
                      fp_ticks={"ready": 20, "put_away": 8, "reload_empty": 50, "reload_full": 45, "melee": 18})
    rifle.magazines = [wc.Magazine(rounds_total_initial=90, rounds_total_max=300, rounds_loaded_max=30,
                                   rounds_reloaded=30, reload_time=2.0, equipment=[(Q_AMMO, 60)])]
    rifle.triggers = [wc.Trigger(rps_min=10.0, rps_max=10.0, magazine=0, rounds_per_shot=1, tracer_interval=3,
                                 firing_noise=2, error_min=0.02, error_max=0.06, error_accel_time=1.0,
                                 error_decel_time=0.5, minimum_error=0.01, projectile=P_BULLET,
                                 fx=[wc.FireEffect(1, 1, (0xE1A00001, 0, 0xE1930002), 0b100, (D_BULLET, 0, 0))])]
    plasma = wc.Weapon(W_PLASMA, "plasma", flags=0x800,
                       label="pp", weapon_type=3, zoom_levels=2, zoom_min=2.0, zoom_max=8.0, heat_recovery=0.25,
                       overheated=1.0, heat_loss=0.5, heat_illumination=1.0)
    plasma.triggers = [
        wc.Trigger(magazine=wc.NO_MAGAZINE, charging_time=0.5, charged_time=0.0, projectile=P_BOLT, heat_per_round=0.25,
                   age_per_round=0.01, projectiles_per_shot=1),
        wc.Trigger(rps_min=4.0, rps_max=8.0, accel_time=1.0, decel_time=0.5, magazine=wc.NO_MAGAZINE,
                   heat_per_round=0.25, age_per_round=0.01, projectile=P_BOLT, projectiles_per_shot=1)]
    shotgun = wc.Weapon(W_SHOTGUN, "shotgun", label="sg", weapon_type=1)
    shotgun.magazines = [wc.Magazine(flags=2, rounds_total_initial=24, rounds_total_max=60, rounds_loaded_max=6,
                                     rounds_reloaded=1, reload_time=0.4, chamber_time=0.2)]
    shotgun.triggers = [wc.Trigger(flags=0x8, rps_min=2.0, rps_max=2.0, magazine=0, rounds_per_shot=1,
                                   projectiles_per_shot=12, projectile=P_PELLET, error_min=0.1, error_max=0.1)]
    s.weapons = [rifle, plasma, shotgun]
    s.projectiles = [
        wc.Projectile(P_BULLET, "bullet", model_tag=0, flags=1, initial_velocity=10.0, final_velocity=10.0,
                      air_gravity=1.0, air_range=(5.0, 15.0), water_gravity=0.4, water_range=(0.0, 5.0), max_range=40.0,
                      impact_damage=D_BULLET, object_radius=0.1,
                      materials=[wc.MaterialResponse(response=1, effect=0xE1A00002), wc.MaterialResponse(response=2),
                                 wc.MaterialResponse(flags=1, response=3, effect=0xE1A00003, angle=(0.0, 3.14159),
                                                     initial_friction=0.7, max_distance=100.0)]),
        wc.Projectile(P_BOLT, "bolt", flags=2, initial_velocity=1.5, final_velocity=0.75, guided_angular_velocity=1.5,
                      air_range=(20.0, 50.0), detonation_effect=0xE1A00004, impact_damage=D_EXPLOSION,
                      arming_time=0.5, timer=(1.0, 2.0)),
        wc.Projectile(P_PELLET, "pellet", flags=1, initial_velocity=4.0, final_velocity=4.0, air_range=(1.5, 3.0))]
    s.damages = [
        wc.Damage(D_BULLET, "bullet", category=2, damage_lower=20.0, damage_upper=(25.0, 25.0), damage_flags=2,
                  flash_color=(0.0, 1.0, 0.5, 0.25), low_rumble=(0.5, 0.25, 4), shake_duration=0.05,
                  shake_rotation=0.01, impulse_rotation=0.03, modifiers=tuple(i / 10 for i in range(33))),
        wc.Damage(D_MELEE, "melee", radius=(0.5, 0.5), category=6, side_effect=2, damage_lower=40.0,
                  damage_upper=(50.0, 60.0)),
        wc.Damage(D_EXPLOSION, "explosion", radius=(1.5, 2.5), aoe_core_radius=0.4, category=3, damage_flags=0x20,
                  damage_lower=80.0, damage_upper=(100.0, 120.0), break_forward=(15.0, 3.0, 1.0))]
    s.equipment = [wc.Equipment(Q_AMMO, "ammo", item_scale=1.0, hud_message=9),
                   wc.Equipment(Q_GRENADE, "grenade", powerup_type=6, grenade_type=1, detonation_delay=(0.4, 0.8))]
    s.grenades = [wc.Grenade(4, 0, 0xE1A00005, 0xE1B00001, Q_GRENADE, P_BOLT), wc.Grenade(3, 1, 0, 0, Q_AMMO, P_PELLET)]
    s.collections = [wc.ItemCollection(C_BOX, 5, [(2.0, W_RIFLE), (1.0, Q_AMMO)])]
    return s


# ---- a synthetic map region --------------------------------------------------------------------------
class Region:
    """Tag region builder: tags with zero-filled data blocks, then pokes (offsets are region offsets)."""

    def __init__(self, tags):
        self.region = bytearray(tagindex.TagIndex.build(tags, scenario_index=0))
        self.index = tagindex.TagIndex.parse_region(bytes(self.region), 0x1000)
        self.by_path = {(e.group, e.path): e for e in self.index.entries}
        self.cursor = {}

    def block(self, group, path):
        return self.by_path[(group, path)].data_address - BASE

    def tid(self, group, path):
        return self.by_path[(group, path)].tag_id

    def alloc(self, group, path, data: bytes) -> int:
        e = self.by_path[(group, path)]
        cur = (self.cursor.get(e.tag_id, 0xC00) + 3) & ~3
        off = e.data_address - BASE + cur
        self.region[off:off + len(data)] = data
        self.cursor[e.tag_id] = cur + len(data)
        return BASE + off

    def poke(self, off, fmt, *v):
        struct.pack_into(fmt, self.region, off, *v)

    def refl(self, off, count, addr):
        struct.pack_into("<III", self.region, off, count, addr, 0)

    def ref(self, off, group, path):
        tid = 0xFFFFFFFF if path is None else self.tid(group, path)
        self.region[off:off + 16] = struct.pack("<4sIII", group[::-1].encode(), 0, 0, tid)

    def map(self) -> texconv.MapData:
        idx = tagindex.TagIndex.parse_region(bytes(self.region), 0x1000)
        return texconv.MapData(Path("synthetic.map"), None, idx, bytes(self.region),  # type: ignore[arg-type]
                               {e.tag_id: e for e in idx.entries})


TAGS = [
    ("scnr", "levels\\t\\t", 0x1000), ("matg", "globals\\globals", 0x1000),
    ("weap", "weapons\\gun\\gun", 0xC00 + 0x2000), ("proj", "weapons\\gun\\bullet", 0xC00 + 0x1000),
    ("jpt!", "weapons\\gun\\bullet", 0x2A0), ("jpt!", "weapons\\gun\\melee", 0x2A0),
    ("eqip", "powerups\\gun ammo", 0x400), ("eqip", "weapons\\frag", 0x400),
    ("proj", "weapons\\frag", 0x700),
    ("mode", "weapons\\gun\\gun", 0x100), ("mode", "weapons\\gun\\fp\\fp", 0x100), ("antr", "weapons\\gun\\fp\\fp", 0x1000),
    ("effe", "weapons\\gun\\effects\\fire", 0x100), ("effe", "weapons\\frag\\effects\\throw", 0x100),
    ("snd!", "sound\\gun\\fire", 0x100), ("snd!", "sound\\gun\\pickup", 0x100),
    ("wphi", "weapons\\gun\\gun", 0x100), ("grhi", "ui\\hud\\default", 0x100),
]
OBJECT_TYPES = {"weap": 2, "proj": 5, "eqip": 3}


def build_map() -> tuple[Region, texconv.MapData]:
    r = Region(TAGS)
    G = lambda g, p: r.block(g, p)  # noqa: E731
    for (g, p), t in (((e.group, e.path), e) for e in r.index.entries):
        if g in OBJECT_TYPES:
            r.poke(t.data_address - BASE, "<H", OBJECT_TYPES[g])
    # ---- the weapon
    w = G("weap", "weapons\\gun\\gun")
    r.ref(w + 0x28, "mode", "weapons\\gun\\gun")
    r.ref(w + 0x45C, "mode", "weapons\\gun\\fp\\fp")
    r.ref(w + 0x46C, "antr", "weapons\\gun\\fp\\fp")
    r.ref(w + 0x480, "wphi", "weapons\\gun\\gun")
    r.ref(w + 0x490, "snd!", "sound\\gun\\pickup")
    r.ref(w + 0x394, "jpt!", "weapons\\gun\\melee")
    r.poke(w + 0x308, "<I", 0x20)
    r.region[w + 0x30C:w + 0x30C + 12] = b"gn\0ld rifle\0"            # a label with stale bytes behind the NUL
    r.poke(w + 0x34C, "<6f", 0.25, 1.0, 1.0, 0.0, 0.3, 1.0)
    r.poke(w + 0x3DA, "<H", 2)
    r.poke(w + 0x3DC, "<2f", 2.0, 8.0)
    r.poke(w + 0x3E4, "<5f", 0.1, 25.0, 0.2, 25.0, 0.0)
    r.poke(w + 0x4E2, "<H", 4)
    mag = bytearray(wc.M_SIZE)
    struct.pack_into("<I4H", mag, 0, 2, 0, 90, 300, 30)
    struct.pack_into("<f", mag, 0x14, 2.5)
    struct.pack_into("<H", mag, 0x18, 30)
    struct.pack_into("<f", mag, 0x1C, 0.5)
    mag[0x38:0x48] = struct.pack("<4sIII", b"effe"[::-1], 0, 0, r.tid("effe", "weapons\\gun\\effects\\fire"))
    mag[0x48:0x58] = struct.pack("<4sIII", b"effe"[::-1], 0, 0, 0xFFFFFFFF)
    eq = bytearray(wc.ME_SIZE)
    struct.pack_into("<H", eq, 0, 60)
    eq[0x0C:0x1C] = struct.pack("<4sIII", b"piqe", 0, 0, r.tid("eqip", "powerups\\gun ammo"))
    struct.pack_into("<III", mag, 0x64, 1, r.alloc("weap", "weapons\\gun\\gun", bytes(eq)), 0)
    r.refl(w + 0x4F0, 1, r.alloc("weap", "weapons\\gun\\gun", bytes(mag)))
    fx = bytearray(wc.FX_SIZE)
    struct.pack_into("<2H", fx, 0, 1, 3)
    fx[0x24:0x34] = struct.pack("<4sIII", b"effe"[::-1], 0, 0, r.tid("effe", "weapons\\gun\\effects\\fire"))
    fx[0x34:0x44] = struct.pack("<4sIII", b"effe"[::-1], 0, 0, 0xFFFFFFFF)
    fx[0x44:0x54] = struct.pack("<4sIII", b"!dns", 0, 0, r.tid("snd!", "sound\\gun\\fire"))          # empty: a sound
    fx[0x54:0x64] = struct.pack("<4sIII", b"!tpj", 0, 0, r.tid("jpt!", "weapons\\gun\\bullet"))
    fx[0x64:0x74] = struct.pack("<4sIII", b"!tpj", 0, 0, 0xFFFFFFFF)
    fx[0x74:0x84] = struct.pack("<4sIII", b"!tpj", 0, 0, 0xFFFFFFFF)
    trg = bytearray(wc.T_SIZE)
    struct.pack_into("<I5f", trg, 0, 0x8, 3.5, 4.5, 1.0, 0.25, 0.0)
    struct.pack_into("<4H", trg, 0x20, 0, 1, 0, 3)
    struct.pack_into("<H", trg, 0x2E, 2)
    struct.pack_into("<2f", trg, 0x38, 0.6, 1.0)
    struct.pack_into("<H", trg, 0x6E, 1)
    struct.pack_into("<f", trg, 0x78, 0.0035)
    struct.pack_into("<2f", trg, 0x7C, 0.0035, 0.007)
    trg[0x94:0xA4] = struct.pack("<4sIII", b"jorp", 0, 0, r.tid("proj", "weapons\\gun\\bullet"))
    struct.pack_into("<2f", trg, 0xB8, 0.08, 0.005)
    struct.pack_into("<III", trg, 0x108, 1, r.alloc("weap", "weapons\\gun\\gun", bytes(fx)), 0)
    r.refl(w + 0x4FC, 1, r.alloc("weap", "weapons\\gun\\gun", bytes(trg)))
    # ---- the projectile with 3 material responses
    p = G("proj", "weapons\\gun\\bullet")
    r.poke(p + 0x17C, "<I", 1)
    r.poke(p + 0x180, "<2H", 1, 4)
    r.poke(p + 0x19C, "<4f", 0.0, 0.1, 0.0, 2.25)
    r.poke(p + 0x1BC, "<4f", 0.5, 0.5, 0.0667, 40.0)
    r.poke(p + 0x1CC, "<7f", 1.0, 5.0, 15.0, 0.4, 0.0, 5.0, 10.8)
    r.poke(p + 0x1E8, "<2f", 10.8, 0.0)
    r.ref(p + 0x224, "jpt!", "weapons\\gun\\bullet")
    r.ref(p + 0x204, "snd!", "sound\\gun\\fire")
    mats = bytearray(3 * wc.MR_SIZE)
    for i, (resp, flags) in enumerate(((1, 0), (2, 0), (3, 1))):
        struct.pack_into("<2H", mats, i * 160, flags, resp)
        mats[i * 160 + 4:i * 160 + 20] = struct.pack("<4sIII", b"effe"[::-1], 0, 0, r.tid("effe", "weapons\\gun\\effects\\fire"))
        mats[i * 160 + 0x3C:i * 160 + 0x4C] = struct.pack("<4sIII", b"effe"[::-1], 0, 0, 0xFFFFFFFF)
        mats[i * 160 + 0x68:i * 160 + 0x78] = struct.pack("<4sIII", b"effe"[::-1], 0, 0, 0xFFFFFFFF)
        struct.pack_into("<4f", mats, i * 160 + 0x90, 0.7, 100.0, 0.9, 0.95)
        struct.pack_into("<2f", mats, i * 160 + 0x2C, 0.0, 3.14159)
    r.refl(p + 0x240, 3, r.alloc("proj", "weapons\\gun\\bullet", bytes(mats)))
    # ---- damage effects
    j = G("jpt!", "weapons\\gun\\bullet")
    r.poke(j + 0x24, "<2H", 1, 1)
    r.poke(j + 0x34, "<f", 0.4)
    r.poke(j + 0x4C, "<4f", 0.1, 1.0, 1.0, 1.0)
    r.poke(j + 0x5C, "<2fH", 1.0, 0.3, 4)
    r.poke(j + 0xCC, "<f", 0.2)
    r.poke(j + 0xD8, "<f", 0.0035)
    r.poke(j + 0x1C4, "<2H", 0, 2)
    r.poke(j + 0x1C8, "<I", 2)
    r.poke(j + 0x1D0, "<3f", 25.0, 25.0, 25.0)
    r.poke(j + 0x21C, "<33f", *([1.0] * 33))
    j2 = G("jpt!", "weapons\\gun\\melee")
    r.poke(j2, "<2f", 0.5, 0.5)
    r.poke(j2 + 0x1C4, "<2H", 2, 6)
    r.poke(j2 + 0x1D0, "<3f", 40.0, 50.0, 60.0)
    # ---- equipment and globals
    q = G("eqip", "weapons\\frag")
    r.poke(q + 0x308, "<2Hf", 6, 0, 0.0)
    r.poke(q + 0x2E0, "<2f", 0.4, 0.8)
    r.ref(q + 0x2F8, "effe", "weapons\\frag\\effects\\throw")
    q2 = G("eqip", "powerups\\gun ammo")
    r.poke(q2 + 0x308, "<2Hf", 0, 0, 0.0)
    r.ref(q2 + 0x310, "snd!", "sound\\gun\\pickup")
    pf = G("proj", "weapons\\frag")
    r.poke(pf + 0x1E4, "<2f", 0.0, 0.0)
    gl = G("matg", "globals\\globals")
    gr = bytearray(wc.G_SIZE)
    struct.pack_into("<2H", gr, 0, 4, 0)
    gr[4:20] = struct.pack("<4sIII", b"effe"[::-1], 0, 0, r.tid("effe", "weapons\\frag\\effects\\throw"))
    gr[0x14:0x24] = struct.pack("<4sIII", b"ihrg", 0, 0, r.tid("grhi", "ui\\hud\\default"))
    gr[0x24:0x34] = struct.pack("<4sIII", b"piqe", 0, 0, r.tid("eqip", "weapons\\frag"))
    gr[0x34:0x44] = struct.pack("<4sIII", b"jorp", 0, 0, r.tid("proj", "weapons\\frag"))
    r.refl(gl + wc.GLOBALS_GRENADES, 1, r.alloc("matg", "globals\\globals", bytes(gr)))
    # ---- the first person graph: animations ready / put-away / reload / melee in the slot order of the doc
    fpa = G("antr", "weapons\\gun\\fp\\fp")
    names = ["first-person idle", "first-person ready", "first-person put-away", "first-person reload-full",
             "first-person melee", "first-person throw-grenade"]
    frames = [60, 29, 11, 87, 37, 38]
    an = b"".join(struct.pack("<32sHH", n.encode(), 0, f).ljust(wc.ANIM_REC, b"\0") for n, f in zip(names, frames))
    r.refl(fpa + wc.ANIMS, len(names), r.alloc("antr", "weapons\\gun\\fp\\fp", an))
    slots = [0xFFFF] * 17
    slots[0], slots[10], slots[11], slots[7], slots[8], slots[13], slots[16] = 0, 1, 2, 3, 3, 4, 5
    el = bytearray(wc.FP_STRIDE)
    struct.pack_into("<III", el, wc.FP_LIST, 17, r.alloc("antr", "weapons\\gun\\fp\\fp", struct.pack("<17H", *slots)), 0)
    r.refl(fpa + wc.FP_WEAPONS, 1, r.alloc("antr", "weapons\\gun\\fp\\fp", bytes(el)))
    return r, r.map()


def check_collect() -> wc.WeapSet:
    r, m = build_map()
    s = wc.collect_map(m)
    assert s.problems == [], s.problems
    assert [x.name for x in s.weapons] == ["gun"] and len(s.projectiles) == 2 and len(s.damages) == 2
    assert [x.name for x in s.equipment] == ["frag", "gun ammo"] or [x.name for x in s.equipment] == ["gun ammo", "frag"]
    w = s.weapons[0]
    assert w.label == "gn" and w.flags == 0x20 and w.weapon_type == 4 and w.zoom_levels == 2 and (w.zoom_min, w.zoom_max) == (2.0, 8.0)
    assert w.model_tag == r.tid("mode", "weapons\\gun\\gun") and w.fp_model_tag == r.tid("mode", "weapons\\gun\\fp\\fp")
    assert w.fp_anim_tag == r.tid("antr", "weapons\\gun\\fp\\fp") and w.hud_tag == r.tid("wphi", "weapons\\gun\\gun")
    assert w.pickup_sound == r.tid("snd!", "sound\\gun\\pickup") and w.melee_damage == r.tid("jpt!", "weapons\\gun\\melee")
    assert abs(w.heat_loss - 0.3) < 1e-6 and abs(w.autoaim_range - 25.0) < 1e-6
    assert w.fp_ticks == {"ready": 29, "put_away": 11, "reload_empty": 87, "reload_full": 87, "melee": 37, "throw": 38}
    mg = w.magazines[0]
    assert (mg.flags, mg.rounds_total_initial, mg.rounds_total_max, mg.rounds_loaded_max, mg.rounds_reloaded) == (2, 90, 300, 30, 30)
    assert abs(mg.reload_time - 2.5) < 1e-6 and abs(mg.chamber_time - 0.5) < 1e-6 and mg.equipment == [(r.tid("eqip", "powerups\\gun ammo"), 60)]
    t = w.triggers[0]
    assert t.flags == 8 and (t.rps_min, t.rps_max, t.accel_time, t.decel_time) == (3.5, 4.5, 1.0, 0.25)
    assert (t.magazine, t.rounds_per_shot, t.tracer_interval, t.firing_noise, t.projectiles_per_shot) == (0, 1, 3, 2, 1)
    assert abs(t.error_min - 0.0035) < 1e-7 and abs(t.error_max - 0.007) < 1e-7 and abs(t.heat_per_round - 0.08) < 1e-7
    assert t.projectile == r.tid("proj", "weapons\\gun\\bullet") and len(t.fx) == 1
    f = t.fx[0]
    assert (f.shots_lo, f.shots_hi) == (1, 3) and f.sound_mask == 0b100 and f.effects[1] == 0
    assert f.effects[0] == r.tid("effe", "weapons\\gun\\effects\\fire") and f.effects[2] == r.tid("snd!", "sound\\gun\\fire")
    assert f.damages == (r.tid("jpt!", "weapons\\gun\\bullet"), 0, 0)
    p = next(x for x in s.projectiles if x.name == "bullet")
    assert p.flags == 1 and (p.timer_starts, p.impact_noise) == (1, 4) and abs(p.collision_radius - 0.1) < 1e-6
    assert abs(p.danger_radius - 2.25) < 1e-6 and abs(p.max_range - 40.0) < 1e-6 and abs(p.initial_velocity - 10.8) < 1e-5
    assert p.air_range == (5.0, 15.0) and abs(p.water_gravity - 0.4) < 1e-6 and p.water_range == (0.0, 5.0)
    assert [m_.response for m_ in p.materials] == [1, 2, 3] and p.materials[2].flags == 1
    assert abs(p.materials[0].initial_friction - 0.7) < 1e-6 and p.materials[1].angle[1] > 3.14
    assert p.impact_damage == r.tid("jpt!", "weapons\\gun\\bullet") and p.flyby_sound == r.tid("snd!", "sound\\gun\\fire")
    d = next(x for x in s.damages if x.name == "bullet")
    assert (d.flash_type, d.flash_priority) == (1, 1) and abs(d.flash_duration - 0.4) < 1e-6 and all(abs(a - b) < 1e-6 for a, b in zip(d.flash_color, (0.1, 1.0, 1.0, 1.0)))
    assert d.low_rumble[2] == 4 and abs(d.shake_duration - 0.2) < 1e-6 and (d.side_effect, d.category) == (0, 2)
    assert d.damage_flags == 2 and (d.damage_lower, d.damage_upper) == (25.0, (25.0, 25.0)) and d.modifiers == (1.0,) * 33
    me = next(x for x in s.damages if x.name == "melee")
    assert me.radius == (0.5, 0.5) and (me.side_effect, me.category) == (2, 6) and me.damage_upper == (50.0, 60.0)
    fr = next(x for x in s.equipment if x.name == "frag")
    assert fr.powerup_type == 6 and all(abs(a - b) < 1e-6 for a, b in zip(fr.detonation_delay, (0.4, 0.8))) and fr.detonation_effect
    assert len(s.grenades) == 1 and s.grenades[0].max_count == 4 and s.grenades[0].equipment == fr.tag_id
    assert s.grenades[0].throw_effect == r.tid("effe", "weapons\\frag\\effects\\throw")
    # models / animations the objpack needs
    return s


def check_plausibility() -> None:
    r, m = build_map()
    s = wc.collect_map(m)
    n0 = len(s.problems)
    s.weapons[0].magazines[0].rounds_loaded_max = 400                  # more loaded than the total maximum
    s.weapons[0].triggers[0].rps_max = 90.0
    s.weapons[0].triggers[0].magazine = 3
    s.projectiles[0].timer = (2.0, 1.0)
    s.damages[0].category = 40
    wc.check_set(s)
    assert len(s.problems) - n0 >= 5, s.problems
    # wrong group in a reference: recorded, not fatal
    r2, m2 = build_map()
    w = r2.block("weap", "weapons\\gun\\gun")
    r2.ref(w + 0x480, "wphi", "weapons\\gun\\gun")
    r2.region[w + 0x480:w + 0x484] = b"edom"                           # a model where the HUD interface belongs
    s2 = wc.collect_map(r2.map())
    assert any("0x480" in p for p in s2.problems) and s2.weapons[0].hud_tag == 0
    # an item collection with a layout that does not fit is refused, not guessed
    expect_fail(lambda: wc.read_collection(*collection_map()))


def collection_map():
    r = Region([("scnr", "levels\\t\\t", 0x100), ("itmc", "weapons\\all", 0x400)])
    e = r.by_path[("itmc", "weapons\\all")]
    r.refl(e.data_address - BASE, 1, r.alloc("itmc", "weapons\\all", bytes(0x54)))
    m = r.map()
    return wc.scenconv.Region(m), e, []


def expect_fail(fn, exc=wc.WeapError) -> None:
    try:
        fn()
    except exc:
        return
    raise SystemExit("bad input accepted")


def check_corrupt_map() -> None:
    r, m = build_map()
    w = r.block("weap", "weapons\\gun\\gun")
    # a reflexive that points outside the tag region
    r.poke(w + 0x4F0, "<II", 1, 0x10)
    expect_fail(lambda: wc.collect_map(r.map()))
    r, m = build_map()
    r.poke(r.block("weap", "weapons\\gun\\gun") + 0x4F0, "<I", 9)              # more magazines than the format allows
    expect_fail(lambda: wc.collect_map(r.map()))
    r, m = build_map()
    r.poke(r.block("proj", "weapons\\gun\\bullet") + 0x1E4, "<I", 0x7F800000)  # infinity
    expect_fail(lambda: wc.collect_map(r.map()))
    r, m = build_map()
    r.poke(r.block("proj", "weapons\\gun\\bullet") + 0x240, "<I", 40)          # 40 material responses
    expect_fail(lambda: wc.collect_map(r.map()))


def check_serialisation(s: wc.WeapSet) -> bytes:
    blob = wc.build_section_from(s)
    assert blob[:4] == b"WEAP" and len(blob) % 16 == 0
    tabs = wc.parse_section(blob)
    assert {k: len(v) for k, v in tabs.items()} == {
        "weapons": 3, "magazines": 2, "triggers": 4, "fx": 1, "projectiles": 3, "materials": 3, "damages": 3,
        "equipment": 2, "grenades": 2, "collections": 1, "permutations": 2}
    w0 = tabs["weapons"][0]
    assert struct.unpack_from("<I", w0)[0] == W_RIFLE and struct.unpack_from("<8H", w0, 0xB0) == (20, 8, 50, 45, 18, 0, 0, 0)
    assert struct.unpack_from("<HHHH", w0, 0xA8) == (0, 1, 0, 1) and w0[0xC0:0xC2] == b"tr" and w0[0xD0:0xD5] == b"rifle"
    w1 = tabs["weapons"][1]
    assert struct.unpack_from("<HHHH", w1, 0xA8) == (1, 0, 1, 2)                 # first magazine, 0 magazines, triggers 1..2
    assert struct.unpack_from("<f", w1, 0x5C)[0] == 1.0 and struct.unpack_from("<H", w1, 0x4E)[0] == 2
    mg = tabs["magazines"][0]
    assert struct.unpack_from("<I6H", mg) == (0, 0, 90, 300, 30, 30, 1) and struct.unpack_from("<f", mg, 0x10)[0] == 2.0
    assert struct.unpack_from("<I", mg, 0x20)[0] == Q_AMMO and struct.unpack_from("<H", mg, 0x28)[0] == 60
    t0 = tabs["triggers"][0]
    assert struct.unpack_from("<f", t0, 4)[0] == 10.0 and struct.unpack_from("<8H", t0, 0x18)[:4] == (0, 1, 0, 3)
    assert struct.unpack_from("<I", t0, 0x6C)[0] == P_BULLET and struct.unpack_from("<2H", t0, 0x74) == (0, 1)
    assert struct.unpack_from("<H", tabs["triggers"][1], 0x18)[0] == wc.NO_MAGAZINE
    fx = tabs["fx"][0]
    assert struct.unpack_from("<2H", fx) == (1, 1) and fx[28] == 4 and struct.unpack_from("<I", fx, 16)[0] == D_BULLET
    p0 = tabs["projectiles"][0]
    assert struct.unpack_from("<I", p0)[0] == P_BULLET and struct.unpack_from("<f", p0, 0x48)[0] == 10.0
    assert struct.unpack_from("<2H", p0, 0x70) == (0, 3) and struct.unpack_from("<I", p0, 0x6C)[0] == D_BULLET
    assert struct.unpack_from("<2H", tabs["projectiles"][1], 0x70) == (3, 0)
    m2 = tabs["materials"][2]
    assert struct.unpack_from("<2H", m2) == (1, 3)
    assert abs(struct.unpack_from("<f", m2, 0x30)[0] - 0.7) < 1e-6 and struct.unpack_from("<f", m2, 0x34)[0] == 100.0
    assert abs(struct.unpack_from("<f", m2, 0x14)[0] - 3.14159) < 1e-6
    d2 = tabs["damages"][2]
    assert struct.unpack_from("<f", d2, 4)[0] == 1.5 and struct.unpack_from("<2H", d2, 0xA0) == (0, 3)
    assert all(abs(a - b) < 1e-6 for a, b in zip(struct.unpack_from("<4f", d2, 0xA8), (0.4, 80.0, 100.0, 120.0))) and struct.unpack_from("<I", d2, 0xA4)[0] == 0x20
    d0 = tabs["damages"][0]
    assert abs(struct.unpack_from("<f", d0, 0xD0 + 4 * 7)[0] - 0.7) < 1e-6 and d0[0x154:0x160].startswith(b"bullet")
    q = tabs["equipment"][1]
    assert struct.unpack_from("<2H", q, 0x10) == (6, 1)
    g1 = tabs["grenades"][1]
    assert struct.unpack_from("<2H", g1) == (3, 1) and struct.unpack_from("<I", g1, 12)[0] == Q_AMMO
    assert struct.unpack_from("<4I", tabs["collections"][0]) == (C_BOX, 0, 2, 5)
    assert struct.unpack_from("<fI", tabs["permutations"][1]) == (1.0, Q_AMMO)
    # all tables 16-aligned, deterministic
    assert wc.build_section_from(s) == blob
    for k in ("magazines", "triggers"):
        assert all(len(x) == wc.RECORD[k] for x in tabs[k])
    # reference parser rejects bad input
    expect_fail(lambda: wc.parse_section(blob[:-16]))
    expect_fail(lambda: wc.parse_section(b"XXXX" + blob[4:]))
    bad = bytearray(blob)
    struct.pack_into("<I", bad, wc.OFFSETS_AT, 7)                          # unaligned table
    expect_fail(lambda: wc.parse_section(bytes(bad)))
    # sets that exceed a u16 index are refused
    big = wc.WeapSet(damages=[wc.Damage(i, "x") for i in range(1, 65538)])
    expect_fail(lambda: wc.build_section_from(big))
    return blob


def check_describe(s: wc.WeapSet) -> None:
    text = wc.describe(s)
    assert "rifle" in text and "plasma" in text and text.count("\n") == 3


# ---- real maps (skipped without game data) --------------------------------------------------------
def check_real_maps() -> None:
    paths = [ROOT / "assets" / "decompressed" / f"{n}.map" for n in MAPS]
    have = [p for p in paths if p.exists()]
    if not have:
        print("  (no decompressed maps present: real-map checks skipped)")
        return
    for path in have:
        m = texconv.MapData.open(path)
        s = wc.collect_map(m)
        assert s.problems == [], (path.name, s.problems)
        groups = {g: sum(1 for e in m.index.entries if e.group == g) for g in ("weap", "proj", "jpt!", "eqip", "itmc")}
        assert (len(s.weapons), len(s.projectiles), len(s.damages), len(s.equipment), len(s.collections)) == \
            (groups["weap"], groups["proj"], groups["jpt!"], groups["eqip"], groups["itmc"]), path.name
        assert len(s.grenades) == 2 and [g.max_count for g in s.grenades] == [4, 4], path.name
        byname = {w.name: w for w in s.weapons}
        if "assault rifle" in byname:
            w = byname["assault rifle"]
            t, mg = w.triggers[0], w.magazines[0]
            assert (t.rps_min, t.rps_max) == (15.0, 15.0) and (mg.rounds_loaded_max, mg.rounds_total_max) == (60, 600)
            assert w.label == "ar" and w.fp_ticks["ready"] == 29 and w.flags == 0
            p = next(x for x in s.projectiles if x.tag_id == t.projectile)
            assert abs(p.initial_velocity * 30 - 324.0) < 0.01 and p.air_range == (5.0, 15.0)
            d = next(x for x in s.damages if x.tag_id == p.impact_damage)
            assert d.damage_upper == (10.0, 10.0) and d.category == 2
        if "pistol" in byname:
            w = byname["pistol"]
            # the reload time of the tag equals the first person reload animation (65 frames at 30 Hz): verified relation
            assert abs(w.magazines[0].reload_time * 30 - w.fp_ticks["reload_full"]) < 1.0
            assert w.zoom_levels == 1 and w.triggers[0].error_min < w.triggers[0].error_max
        if "sniper rifle" in byname:
            w = byname["sniper rifle"]
            assert w.zoom_levels == 2 and (w.zoom_min, w.zoom_max) == (2.0, 8.0) and w.flags == 0x4020
            p = next(x for x in s.projectiles if x.tag_id == w.triggers[0].projectile)
            assert abs(p.initial_velocity * 30 - 1000.0) < 0.01
        # every weapon fires something, every ammunition pickup of a magazine is an equipment tag of the map
        assert all(len(w.triggers) in (0, 1, 2) for w in s.weapons)
        eqs = {q.tag_id for q in s.equipment}
        assert all(tid in eqs for w in s.weapons for mg in w.magazines for tid, _ in mg.equipment)
        # grenades: frag then plasma, the equipment of each carries that grenade type
        eq = {q.tag_id: q for q in s.equipment}
        assert [eq[g.equipment].grenade_type for g in s.grenades] == [0, 1]
        # object pack roots
        rm, ra = wc.referenced_models(path), wc.referenced_anims(path)
        assert rm and ra and all(m.by_id[t].group == "mode" for t in rm) and all(m.by_id[t].group == "antr" for t in ra)
        assert {w.fp_model_tag for w in s.weapons if w.fp_model_tag} <= rm
        blob = wc.build_section_from(s)
        tabs = wc.parse_section(blob)
        assert len(tabs["weapons"]) == len(s.weapons) and len(tabs["materials"]) == sum(len(p.materials) for p in s.projectiles)
        assert wc.build_section_from(wc.collect_map(m)) == blob
        pk = hp2.parse(hp2.build(path.stem, [("WEAP", blob)]))
        assert pk.read("WEAP") == blob
        print(f"  {path.stem}: {len(s.weapons)} weapons, {len(s.projectiles)} projectiles, {len(s.damages)} damage effects, "
              f"{len(blob)} bytes")


def main() -> None:
    s = check_collect()
    check_plausibility()
    check_corrupt_map()
    fs = fixture_set()
    assert wc.check_set(fs) is None and fs.problems == [], fs.problems
    blob = check_serialisation(fs)
    check_describe(fs)
    check_real_maps()
    pack = hp2.build("test", [("WEAP", blob)])
    assert hp2.parse(pack).read("WEAP") == blob
    if os.environ.get("WEAP_FIXTURE"):
        Path(os.environ["WEAP_FIXTURE"]).write_bytes(pack)
    print("ok")


if __name__ == "__main__":
    main()
