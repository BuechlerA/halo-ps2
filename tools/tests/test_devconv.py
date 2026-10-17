"""DEVS section on a synthetic map (no game data needed): device tags (machine, control, light fixture), placements with
groups and flags, the SCEN placement order, object names (carried / dangling), trigger volumes, starting locations,
cutscene flags and cameras, sound scenery, decals, BSP switches, the writer and its reference parser, corrupt maps,
and the independent volume reference.  Set DEVS_FIXTURE=<path> to also write the .hp2 that the C reader tests
(engine/tests/test_devdef.c and test_devices.c, which hard-code the numbers of this world) open.  Skip-if-missing
checks convert every real campaign map found under assets/decompressed/ and check the layout invariants."""
import math
import os
import random
import struct
import sys
from pathlib import Path

sys.path[:0] = [str(Path(__file__).resolve().parents[1]), str(Path(__file__).resolve().parent)]
from halo2ps2 import devconv as dc, hp2, scenconv as sc, tagindex, texconv  # noqa: E402
import test_scenconv as tsc  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
BASE = tagindex.XBOX_TAG_BASE
NAN = struct.unpack("<f", struct.pack("<I", 0x7FC00000))[0]

TAGS = [
    ("scnr", "levels\\t\\t", 0x6000),
    ("scen", "scenery\\crate\\crate", 0x2000),
    ("mach", "levels\\t\\devices\\door", 0x2000), ("mach", "levels\\t\\devices\\platform", 0x2000),
    ("mach", "levels\\t\\devices\\gear", 0x2000),
    ("ctrl", "levels\\t\\devices\\toggle", 0x2000), ("ctrl", "levels\\t\\devices\\button on", 0x2000),
    ("ctrl", "levels\\t\\devices\\call", 0x2000),
    ("lifi", "levels\\t\\devices\\lamp", 0x2000),
    ("ssce", "sound\\hum", 0x2000), ("lsnd", "sound\\hum_loop", 0x2000),
    ("snd!", "sound\\open", 0x2000), ("snd!", "sound\\close", 0x2000), ("snd!", "sound\\on", 0x2000),
    ("effe", "effects\\opened", 0x2000), ("effe", "effects\\deny", 0x2000),
    ("mode", "levels\\t\\devices\\door", 0x2000), ("mode", "levels\\t\\devices\\gear", 0x2000),
    ("antr", "levels\\t\\devices\\door", 0x2000), ("antr", "levels\\t\\devices\\gear", 0x2000),
    ("deca", "decals\\scorch", 0x2000), ("deca", "decals\\bullet", 0x2000),
]


def entry(r: tsc.Region, group: str, path: str) -> tagindex.TagEntry:
    return tsc.entry(r, group, path)


def ref(r: tsc.Region, group: str, path: str | None) -> bytes:
    """A tag reference; null references keep their group and carry tag id 0xFFFFFFFF, as on the retail maps."""
    if path is None:
        return struct.pack("<4sIII", group[::-1].encode(), 0, 0, 0xFFFFFFFF)
    return struct.pack("<4sIII", group[::-1].encode(), 0, 0, entry(r, group, path).tag_id)


def put_ref(r: tsc.Region, off: int, group: str, path: str | None) -> None:
    r.region[off:off + 16] = ref(r, group, path)


def write_device(r: tsc.Region, path: str, kind: str, *, model=None, anim=None, flags=0, times=(0.0,) * 6,
                 fn=(0, 0, 0, 0), fx=(None,) * 6, delay=0.0, delay_fx=None, radius=0.0, mtype=0, mflags=0,
                 open_time=0.0, coll=0, elev=0, ctype=0, trig=0, call=0.0, cfx=(None,) * 3, open_ticks=None) -> None:
    group = {"machine": "mach", "control": "ctrl", "light": "lifi"}[kind]
    b = entry(r, group, path).data_address - BASE
    objtype = {"machine": 7, "control": 8, "light": 9}[kind]
    r.poke(b, "<H", objtype)
    r.poke(b + dc.OBJ_MODEL, "<4sIII", b"edom", 0, 0, 0xFFFFFFFF)
    r.poke(b + dc.OBJ_ANIM, "<4sIII", b"rtna", 0, 0, 0xFFFFFFFF)
    if model:
        put_ref(r, b + dc.OBJ_MODEL, "mode", model)
    if anim:
        put_ref(r, b + dc.OBJ_ANIM, "antr", anim)
    r.poke(b + dc.DEV_FLAGS, "<I", flags)
    r.poke(b + dc.DEV_TIMES, "<6f", *times)
    r.poke(b + dc.DEV_FN, "<4H", *fn)
    snd = {"open": "sound\\open", "close": "sound\\close", "on": "sound\\on"}
    for i in range(6):
        v = fx[i]
        if v is None:
            put_ref(r, b + dc.DEV_FX + 16 * i, "effe", None)
        elif v in snd:
            put_ref(r, b + dc.DEV_FX + 16 * i, "snd!", snd[v])
        else:
            put_ref(r, b + dc.DEV_FX + 16 * i, "effe", v)
    r.poke(b + dc.DEV_DELAY, "<f", delay)
    put_ref(r, b + dc.DEV_DELAY_FX, "effe", delay_fx)
    r.poke(b + dc.DEV_RADIUS, "<f", radius)
    if kind == "machine":
        r.poke(b + dc.MACH_TYPE, "<HH", mtype, mflags)
        r.poke(b + dc.MACH_OPEN, "<f", open_time)
        r.poke(b + dc.MACH_COLL, "<HH", coll, elev)
        r.poke(b + dc.MACH_TICKS, "<I", round(open_time * 30) if open_ticks is None else open_ticks)
    elif kind == "control":
        r.poke(b + dc.CTRL_TYPE, "<HHf", ctype, trig, call)
        for i, v in enumerate(cfx):
            put_ref(r, b + dc.CTRL_FX + 16 * i, "effe", v)


def write_antr(r: tsc.Region, path: str, anims: list[tuple[str, int]], slots: list[int]) -> None:
    e = entry(r, "antr", path)
    b = e.data_address - BASE
    an = b"".join(struct.pack("<32sHH", n.encode(), 1, frames).ljust(sc.ANIM_REC, b"\0") for n, frames in anims)
    r.refl(b + dc.ANTR_ANIMS, len(anims), r.alloc_at(e, an))
    dev = bytearray(dc.ANTR_DEVICE_REC)
    struct.pack_into("<III", dev, dc.DEVICE_SLOTS, len(slots), r.alloc_at(e, struct.pack(f"<{len(slots)}h", *slots)), 0)
    r.refl(b + dc.ANTR_DEVICES, 1, r.alloc_at(e, bytes(dev)))


def rows_block(r: tsc.Region, sb: int, off: int, stride: int, rows: list[bytes]) -> None:
    r.refl(sb + off, len(rows), r.alloc("levels\\t\\t", b"".join(rows)) if rows else 0)


def placement(stride: int, ptype: int, name: int, flags: int, pos, yaw=0.0) -> bytearray:
    e = bytearray(stride)
    struct.pack_into("<HHHH3f3f", e, 0, ptype, name, flags, 0, *pos, yaw, 0.0, 0.0)
    return e


def device_row(stride: int, ptype: int, name: int, flags: int, pos, yaw=0.0, pg=-1, qg=-1, dflags=0, w30=0, w34=0,
               manual=0) -> bytes:
    e = placement(stride, ptype, name, flags, pos, yaw)
    struct.pack_into("<hhI", e, 0x28, pg, qg, dflags)
    if stride == 88:                                            # light fixture: manual BSP flags, colour, intensity, angles
        struct.pack_into("<H", e, 0x20, manual)
        struct.pack_into("<6f", e, 0x30, 0.5, 0.25, 0.75, 2.0, 1.2, 0.8)
    else:
        struct.pack_into("<IH", e, 0x30, w30, w34)
    return bytes(e)


def build_synthetic() -> tuple[tsc.Region, texconv.MapData]:
    r = tsc.Region(TAGS)
    S = "levels\\t\\t"
    sb = r.block(S)
    D = "levels\\t\\devices\\"
    E = lambda g, p: entry(r, g, p)  # noqa: E731
    # ---- device tags (times in seconds: ticks 30 x)
    write_device(r, D + "door", "machine", model=D + "door", anim=D + "door", flags=0,
                 times=(0.5, 0.1, 1.0, 0.2, 2.0, 0.0), fn=(3, 1, 0, 0), fx=("open", "close", "effects\\opened", None, None, None),
                 delay=0.5, delay_fx="effects\\opened", radius=2.0, mtype=0, mflags=3, open_time=3.0, coll=1)
    write_device(r, D + "platform", "machine", times=(0.0, 0.0, 2.0, 0.0, 1.0, 0.0), mtype=1, mflags=4, elev=2)
    write_device(r, D + "gear", "machine", model=D + "gear", anim=D + "gear", flags=1,
                 times=(1.0, 0.0, 0.5, 0.0, 0.0, 0.0), mtype=2)
    write_device(r, D + "toggle", "control", times=(0.0, 0.0, 0.0, 0.0, 0.0, 0.0), ctype=0,
                 cfx=("effects\\opened", None, "effects\\deny"))
    write_device(r, D + "button on", "control", ctype=1, cfx=(None, None, "effects\\deny"))
    write_device(r, D + "call", "control", ctype=3, call=0.75, times=(0.0, 0.0, 0.5, 0.0, 0.0, 0.0))
    write_device(r, D + "lamp", "light", times=(0.25, 0.0, 0.0, 0.0, 0.0, 0.0))
    # animation graphs: door has a position overlay of 10 frames, gear position + power animations
    write_antr(r, D + "door", [("device position", 10)], [0])
    write_antr(r, D + "gear", [("device position", 12), ("device power", 5)], [0, 1])
    # sound scenery tag: object type 11, looping sound reference at +0x1FC
    sb_ssce = E("ssce", "sound\\hum").data_address - BASE
    r.poke(sb_ssce, "<H", 11)
    put_ref(r, sb_ssce + dc.SSCE_SOUND, "lsnd", "sound\\hum_loop")
    r.poke(E("scen", "scenery\\crate\\crate").data_address - BASE, "<H", 6)

    # ---- scenario: names, groups
    names = [("door_a", 7, 1), ("crate_a", 6, 0), ("ctl_a", 8, 0), ("hum_a", 11, 0), ("ghost", 6, 0), ("lamp_a", 9, 0)]
    r.refl(sb + dc.SCN_NAMES, len(names), r.alloc(S, b"".join(n.encode().ljust(32, b"\0") + struct.pack("<HH", t, i)
                                                          for n, t, i in names)))
    groups = [("grp_a", 0.0, 0), ("grp_b", 1.0, 1), ("grp_c", 0.5, 0)]
    r.refl(sb + dc.SCN_GROUPS, 3, r.alloc(S, b"".join(n.encode().ljust(32, b"\0") + struct.pack("<fI", v, f).ljust(20, b"\0")
                                                   for n, v, f in groups)))

    def palette(group: str, off: int, paths: list[str]) -> None:
        data = b"".join(struct.pack("<4sIII", group[::-1].encode(), 0, 0, E(group, p).tag_id).ljust(48, b"\0") for p in paths)
        r.refl(sb + off, len(paths), r.alloc(S, data))

    # scenery (SCEN order: scenery first): placed, dropped (deleted palette entry), script created
    palette("scen", 0x21C, ["scenery\\crate\\crate"])
    rows_block(r, sb, 0x210, 72, [bytes(placement(72, 0, 1, 0, (-5.0, 100.0, 0.5))), bytes(placement(72, 0xFFFF, 0xFFFF, 0, (0, 0, 0))),
                                  bytes(placement(72, 0, 0xFFFF, 1, (25.0, 100.0, 0.0)))])
    # machines: door, door one sided (named), a dropped one, platform, gear, door initially off
    palette("mach", 0x2A0, [D + "door", D + "platform", D + "gear"])
    rows_block(r, sb, 0x294, 64, [
        device_row(64, 0, 0xFFFF, 0, (-5.0, 100.0, 0.0), 0.0),
        device_row(64, 0, 0, 1, (5.0, 101.0, 0.0), math.pi, w30=2),
        device_row(64, 0xFFFF, 0xFFFF, 0, (0.0, 0.0, 0.0)),
        device_row(64, 1, 0xFFFF, 0x0A, (25.0, 100.0, 0.0), 0.0, qg=0),
        device_row(64, 2, 0xFFFF, 0, (0.0, 100.0, 0.0), 0.0, pg=2, qg=1, dflags=8 | 1),
        device_row(64, 0, 0xFFFF, 0, (-3.0, 100.0, 0.0), 0.0, pg=0, dflags=2 | 4, w30=1 | 8),
    ])
    # controls: toggle (power group, position group, both sides), on button, call button
    palette("ctrl", 0x2B8, [D + "toggle", D + "button on", D + "call"])
    rows_block(r, sb, 0x2AC, 64, [
        device_row(64, 0, 2, 0, (-2.0, 100.0, 0.0), 0.0, pg=2, qg=0, dflags=16, w30=1, w34=3),
        device_row(64, 1, 0xFFFF, 0, (2.0, 100.0, 0.0), 0.5, qg=1),
        device_row(64, 2, 0xFFFF, 0, (-1.0, 100.0, 0.0), 0.0, qg=2),
    ])
    palette("lifi", 0x2D0, [D + "lamp"])
    rows_block(r, sb, 0x2C4, 88, [device_row(88, 0, 5, 0, (1.0, 100.0, 0.5), 0.0, manual=2)])
    # sound scenery: two placements, one named
    palette("ssce", 0x2E8, ["sound\\hum"])
    rows_block(r, sb, 0x2DC, 40, [bytes(placement(40, 0, 3, 0, (-4.0, 100.0, 1.0))), bytes(placement(40, 0, 0xFFFF, 1, (4.0, 100.0, 1.0)))])
    # trigger volumes (96 B): axis aligned, rotated 90 degrees about z, tilted, and one with NaN extents
    def volume(name: str, fw, up, pos, ext) -> bytes:
        v = bytearray(96)
        struct.pack_into("<I", v, 0, 1)
        v[4:4 + len(name)] = name.encode()
        struct.pack_into("<3f3f3f3f", v, 0x30, *fw, *up, *pos, *ext)
        return bytes(v)
    s2, c2 = math.sin(0.5), math.cos(0.5)
    vols = [volume("vol_a", (1, 0, 0), (0, 0, 1), (-5.0, 95.0, 0.0), (10.0, 10.0, 5.0)),
            volume("vol_b", (0, 1, 0), (0, 0, 1), (20.0, 95.0, 0.0), (4.0, 2.0, 3.0)),
            volume("vol_c", (c2, 0, s2), (-s2, 0, c2), (0.0, 100.0, 0.0), (2.0, 3.0, 4.0)),
            volume("vol_bad", (1, 0, 0), (0, 0, 1), (0, 0, 0), (NAN, 1.0, 1.0))]
    rows_block(r, sb, dc.SCN_VOLUMES, 96, vols)
    # starting locations (52 B)
    def start(pos, facing, team, bsp_idx, types) -> bytes:
        s = bytearray(52)
        struct.pack_into("<3ff6h", s, 0, *pos, facing, team, bsp_idx, *types)
        return bytes(s)
    rows_block(r, sb, dc.SCN_STARTS, 52, [start((-5.0, 100.0, 0.5), 1.5, 0, 0, (0, 0, 0, 0)),
                                          start((25.0, 100.0, 0.5), -0.5, 100, 1, (1, 0, 0, 2))])
    # cutscene flags (92 B), camera points (104 B)
    def flag(name: str, pos, yaw, pitch) -> bytes:
        f = bytearray(92)
        f[4:4 + len(name)] = name.encode()
        struct.pack_into("<3fff", f, 0x24, *pos, yaw, pitch)
        return bytes(f)
    rows_block(r, sb, dc.SCN_CFLAGS, 92, [flag("flag_a", (-5.0, 100.0, 0.5), 1.0, 0.25), flag("flag_far", (60.0, 0.0, 0.0), -3.0, 0.0)])

    def camera(name: str, pos, ori, fov) -> bytes:
        c = bytearray(104)
        c[4:4 + len(name)] = name.encode()
        struct.pack_into("<3f3ff", c, 0x28, *pos, *ori, fov)
        return bytes(c)
    rows_block(r, sb, dc.SCN_CAMERAS, 104, [camera("cam_a", (25.0, 100.0, 0.5), (0.5, -0.25, 0.0), 1.2217),
                                            camera("cam_b", (0.0, 100.0, 0.5), (-3.0, 0.5, 0.1), 0.9)])
    # decals: palette of two, three placements (the third has a type outside the palette: dropped)
    r.refl(sb + dc.SCN_DECAL_PALETTE, 2, r.alloc(S, ref(r, "deca", "decals\\scorch") + ref(r, "deca", "decals\\bullet")))
    def decal(typ, yaw, pitch, pos) -> bytes:
        return struct.pack("<Hbb3f", typ, yaw, pitch, *pos)
    rows_block(r, sb, dc.SCN_DECALS, 16, [decal(0, 64, -64, (-5.0, 100.0, 0.0)), decal(1, -32, 16, (25.0, 100.0, 0.5)),
                                          decal(7, 0, 0, (0.0, 0.0, 0.0))])
    # BSP switches (8 B): volume 0: BSP 0 -> 1; volume 1: 1 -> 0; one with a BSP the map does not have (dropped)
    rows_block(r, sb, dc.SCN_BSPSW, 8, [struct.pack("<4H", 0, 0, 1, 0xFFFF), struct.pack("<4H", 1, 1, 0, 5),
                                        struct.pack("<4H", 2, 0, 9, 0xFFFF)])
    return r, r.map()


def expect(fn, exc=dc.DevError) -> None:
    try:
        fn()
    except exc:
        return
    raise SystemExit("corrupt input accepted")


# ---- independent volume reference: solve for the local coordinates with Cramer's rule ---------------------------
def det3(a, b, c) -> float:
    return a[0] * (b[1] * c[2] - b[2] * c[1]) - a[1] * (b[0] * c[2] - b[2] * c[0]) + a[2] * (b[0] * c[1] - b[1] * c[0])


def reference_inside(v: dc.Volume, p) -> bool:
    """p - position = s0 forward + s1 left + s2 up, solved by Cramer's rule (no assumption of orthonormality)."""
    d = tuple(p[i] - v.position[i] for i in range(3))
    f, l, u = v.forward, v.left, v.up
    den = det3(f, l, u)
    s = (det3(d, l, u) / den, det3(f, d, u) / den, det3(f, l, d) / den)
    return all(0.0 <= s[i] <= v.extents[i] for i in range(3))


# ---- tests --------------------------------------------------------------------------------------------------
def check_collect() -> dc.DevSet:
    r, m = build_synthetic()
    ds = dc.collect(m, tsc.synthetic_probes())
    assert len(ds.problems) == 3, ds.problems          # the NaN volume, the decal of a type outside the palette, the BSP switch to BSP 9
    assert any("trigger volume 3" in p for p in ds.problems) and any("BSP switch" in p for p in ds.problems)
    assert any("decal 2" in p for p in ds.problems)
    dc.check_scen(m, ds)
    # defs: sorted by tag id = tag order
    names = [d.name for d in ds.defs]
    assert names == ["door", "platform", "gear", "toggle", "button on", "call", "lamp"], names
    door, plat, gear, tog, bon, call, lamp = ds.defs
    assert (door.kind, door.subtype, plat.subtype, gear.subtype) == (dc.K_MACHINE, 0, 1, 2)
    assert door.ticks == (15, 3, 30, 6, 60, 0) and door.open_ticks == 90 and door.delay_ticks == 15
    assert door.auto_radius == 2.0 and door.kind_flags == 3 and door.collision == 1 and door.fn_in == (3, 1, 0, 0)
    assert plat.ticks == (0, 0, 60, 0, 30, 0) and plat.kind_flags == 4 and plat.elevator_node == 2 and plat.auto_radius == 0.0
    assert gear.dev_flags == 1 and gear.ticks[0] == 30 and gear.ticks[2] == 15
    e = lambda g, p: entry(r, g, p).tag_id  # noqa: E731
    assert door.fx[:3] == (e("snd!", "sound\\open"), e("snd!", "sound\\close"), e("effe", "effects\\opened"))
    assert door.fx[3:6] == (0, 0, 0) and door.fx[6] == e("effe", "effects\\opened") and door.fx_snd_mask == 0b11
    assert tog.fx[7] == e("effe", "effects\\opened") and tog.fx[8] == 0 and tog.fx[9] == e("effe", "effects\\deny")
    assert (tog.subtype, bon.subtype, call.subtype) == (0, 1, 3) and abs(call.call_value - 0.75) < 1e-6 and call.ticks[2] == 15
    assert lamp.kind == dc.K_LIGHT and lamp.ticks[0] == 8 and lamp.model_tag == 0 and lamp.anim_tag == 0
    # animations: position slot only on the door, position + power on the gear, frame counts
    assert (door.pos_anim, door.power_anim, door.pos_frames, door.power_frames) == (0, dc.NONE16, 10, 0)
    assert (gear.pos_anim, gear.power_anim, gear.pos_frames, gear.power_frames) == (0, 1, 12, 5)
    assert plat.pos_anim == dc.NONE16 and door.model_tag == e("mode", "levels\\t\\devices\\door") and gear.anim_tag
    # groups
    assert [(g.name, g.initial, g.flags) for g in ds.groups] == [("grp_a", 0.0, 0), ("grp_b", 1.0, 1), ("grp_c", 0.5, 0)]
    # devices: scenario order (machines, controls, light fixtures), SCEN indices after 2 scenery placements
    assert len(ds.devices) == 9 and ds.dropped_devices == 1 and ds.scen_count == 11
    D = ds.devices
    assert [d.kind for d in D] == [0, 0, 0, 0, 0, 1, 1, 1, 2]
    assert [d.scen_index for d in D] == [2, 3, 4, 5, 6, 7, 8, 9, 10]
    assert [d.def_index for d in D] == [0, 0, 1, 2, 0, 3, 4, 5, 6]
    assert [d.power_group for d in D] == [-1, -1, -1, 2, 0, 2, -1, -1, -1]
    assert [d.position_group for d in D] == [-1, -1, 0, 1, -1, 0, 1, 2, -1]
    assert [d.device_flags for d in D] == [0, 0, 0, 9, 6, 16, 0, 0, 0]
    assert [d.kind_flags for d in D] == [0, 2, 0, 0, 9, 1, 0, 0, 0] and D[5].extra == 3
    assert [d.flags for d in D] == [1, 0, 0x0B, 1, 1, 1, 1, 1, 1]       # SCEN style: bit 0 = placed automatically
    assert [d.name_index for d in D] == [-1, 0, -1, -1, -1, 2, -1, -1, 5]
    assert abs(D[1].rot[0] - math.pi) < 1e-6 and D[2].pos == (25.0, 100.0, 0.0)
    assert [d.bsp_mask for d in D] == [1, 1, 2, 1, 1, 1, 1, 1, 3]       # the lamp's manual BSP flags add BSP 1
    assert D[8].light == (0.5, 0.25, 0.75, 2.0, 1.2, 0.8) or all(abs(a - b) < 1e-6 for a, b in zip(D[8].light, (0.5, 0.25, 0.75, 2.0, 1.2, 0.8)))
    # volumes (the invalid one is kept as an empty box so that indices stay stable)
    assert len(ds.volumes) == 4 and ds.volumes[3].extents == (0.0, 0.0, 0.0) and ds.volumes[0].name == "vol_a"
    assert ds.volumes[1].left == (-1.0, 0.0, 0.0) and ds.volumes[0].left == (0.0, 1.0, 0.0)
    # starts, flags, cameras
    assert [s.bsp_mask for s in ds.starts] == [1, 2] and ds.starts[1].team == 100 and ds.starts[1].types == (1, 0, 0, 2)
    assert ds.starts[0].facing == 1.5 and ds.starts[1].bsp == 1
    assert [c.bsp_mask for c in ds.cflags] == [1, 0] and ds.cflags[0].name == "flag_a" and ds.cflags[0].pitch == 0.25
    assert [c.bsp_mask for c in ds.cameras] == [2, 1] and abs(ds.cameras[0].fov - 1.2217) < 1e-6
    # sound scenery and decals
    assert len(ds.ambients) == 2 and ds.ambients[0].sound_tag == e("lsnd", "sound\\hum_loop") and ds.ambients[0].name_index == 3
    assert ds.ambients[0].flags == 1 and ds.ambients[1].flags == 0 and ds.ambients[1].name_index == -1
    assert ds.decal_types == [e("deca", "decals\\scorch"), e("deca", "decals\\bullet")] and len(ds.decals) == 2
    assert ds.decals[0].yaw_raw == 64 and ds.decals[0].pitch_raw == -64 and abs(ds.decals[0].yaw - math.pi / 2) < 1e-6
    assert abs(ds.decals[0].pitch + math.pi / 2) < 1e-6 and ds.decals[1].bsp_mask == 2
    assert [(b.volume, b.source, b.dest, b.unknown) for b in ds.bspsw] == [(0, 0, 1, 0xFFFF), (1, 1, 0, 5)]
    # names: carried names resolve to SCEN placement and device / ambient indices, 'ghost' is dangling
    N = ds.names
    assert [n.name for n in N] == ["door_a", "crate_a", "ctl_a", "hum_a", "ghost", "lamp_a"] and ds.dangling_names == 1
    assert [n.flags for n in N] == [3, 1, 3, 5, 0, 3]
    assert [n.scen_index for n in N] == [3, 0, 7, dc.NONE32, dc.NONE32, 10]
    assert [n.aux_index for n in N] == [1, dc.NONE32, 5, 0, dc.NONE32, 8]
    assert [n.scen_type for n in N] == [sc.MACHINE, sc.SCENERY, sc.CONTROL, sc.OTHER, sc.SCENERY, sc.LIGHT_FIXTURE]
    assert [n.table_index for n in N] == [1, 0, 0, 0, 0, 0]
    # roots for the other packs
    assert dc.models_of(ds) == {e("mode", "levels\\t\\devices\\door"), e("mode", "levels\\t\\devices\\gear")}
    assert dc.anims_of(ds) == {e("antr", "levels\\t\\devices\\door"), e("antr", "levels\\t\\devices\\gear")}
    assert dc.sounds_of(ds) == {e("snd!", "sound\\open"), e("snd!", "sound\\close"), e("effe", "effects\\opened"),
                                e("effe", "effects\\deny"), e("lsnd", "sound\\hum_loop")}
    assert dc.effects_of(ds) == {e("effe", "effects\\opened"), e("effe", "effects\\deny")}
    # without BSPs nothing is located
    ds0 = dc.collect(m, [])
    assert all(d.bsp_mask == 0 for d in ds0.devices) and ds0.bsp_count == 0 and len(ds0.bspsw) == 3
    return ds


def check_scen_cross(ds: dc.DevSet) -> None:
    """Every device's scen_index points at the SCEN placement of the same tag and position."""
    r, m = build_synthetic()
    objs = sc.collect_objects(m, [])
    assert len(objs.placements) == ds.scen_count == 11
    for d in ds.devices:
        p = objs.placements[d.scen_index]
        assert p.kind == dc.KIND_NAMES[d.kind] and p.pos == d.pos and p.tag_id == ds.defs[d.def_index].tag_id
        assert p.flags == d.flags and p.name_index == d.name_index
    # a swapped placement count is detected
    ds.scen_count += 1
    expect(lambda: dc.check_scen(m, ds))
    ds.scen_count -= 1


def check_tag_problems() -> None:
    # a tag block that is too short for its kind, a wrong object type, a time out of range, a reference of a wrong group
    r, m = build_synthetic()
    ds = dc.collect(m, [])
    base = len(ds.problems)
    r2, _ = build_synthetic()
    b = entry(r2, "mach", "levels\\t\\devices\\door").data_address - BASE
    r2.poke(b, "<H", 3)
    r2.poke(b + dc.DEV_TIMES, "<f", 1e9)
    put = lambda off, g, p: r2.region.__setitem__(slice(b + off, b + off + 16), ref(r2, g, p))  # noqa: E731
    put(dc.DEV_FX + 32, "snd!", None)
    r2.region[b + dc.DEV_FX + 48:b + dc.DEV_FX + 64] = struct.pack("<4sIII", b"edom", 0, 0, entry(r2, "mode", "levels\\t\\devices\\door").tag_id)
    ds2 = dc.collect(r2.map(), [])
    assert len(ds2.problems) >= base + 3 and any("object type" in p for p in ds2.problems)
    assert any("outside 0..3600 s" in p for p in ds2.problems) and any("not usable" in p for p in ds2.problems)
    # too-short block: pretend the next tag starts 0x100 bytes after the machine
    ext = dc.tag_extents(m)
    tid = entry(r, "mach", "levels\\t\\devices\\platform").tag_id
    assert ext[tid] == 0x2000
    ext2 = dict(ext)
    ext2[tid] = 0x100
    probs: list[str] = []
    assert dc.read_def(sc.Region(m), ext2, entry(r, "mach", "levels\\t\\devices\\platform"), dc.K_MACHINE, probs) is None and probs
    # door open ticks disagreeing with the time is noted
    r3, _ = build_synthetic()
    r3.poke(entry(r3, "mach", "levels\\t\\devices\\door").data_address - BASE + dc.MACH_TICKS, "<I", 77)
    assert any("door open ticks 77" in p for p in dc.collect(r3.map(), []).problems)


def check_group_ranges() -> None:
    # a device whose group index is outside the group block is kept with -1 and noted
    r, m = build_synthetic()
    sb = r.block("levels\\t\\t")
    n, o = sc.Region(m).reflexive(sb + 0x294, 64)
    r.poke(o + 3 * 64 + 0x2A, "<h", 77)                          # platform's position group
    ds = dc.collect(r.map(), [])
    assert ds.devices[2].position_group == -1 and any("position group 77" in p for p in ds.problems)


def check_corrupt_map() -> None:
    r, m = build_synthetic()
    sb = r.block("levels\\t\\t")
    for off in (dc.SCN_GROUPS, dc.SCN_VOLUMES, dc.SCN_STARTS, dc.SCN_CFLAGS, dc.SCN_CAMERAS, dc.SCN_DECALS, dc.SCN_DECAL_PALETTE,
                dc.SCN_BSPSW, dc.SCN_NAMES, 0x294, 0x2AC, 0x2C4, 0x2DC):
        r2, _ = build_synthetic()
        r2.poke(sb + off, "<II", 5, 0x80000000 + 0x7FFF0000)
        expect(lambda: dc.collect(r2.map(), []))
    r3, _ = build_synthetic()
    r3.poke(sb + dc.SCN_VOLUMES + 4, "<I", 1)                    # pointer below the region base
    expect(lambda: dc.collect(r3.map(), []))
    # NaN position of a machine: dropped from the devices (and from SCEN: the replica agrees), not carried
    r4, _ = build_synthetic()
    n, o = sc.Region(r4.map()).reflexive(sb + 0x294, 64)
    r4.poke(o + 8, "<I", 0x7FC00000)
    ds = dc.collect(r4.map(), [])
    assert len(ds.devices) == 8 and ds.dropped_devices == 2
    dc.check_scen(r4.map(), ds)
    # a NaN starting location is zeroed with a note, not carried
    r5, _ = build_synthetic()
    n, o = sc.Region(r5.map()).reflexive(sb + dc.SCN_STARTS, 52)
    r5.poke(o, "<I", 0x7F800000)
    ds = dc.collect(r5.map(), [])
    assert ds.starts[0].pos == (0.0, 0.0, 0.0) and any("starting location 0" in p for p in ds.problems)
    # an object name table entry pointing at the wrong kind is noted
    r6, _ = build_synthetic()
    n, o = sc.Region(r6.map()).reflexive(sb + dc.SCN_NAMES, 36)
    r6.poke(o + 32, "<HH", 8, 0)                                 # name 0 says control 0, but a machine carries it
    assert any("object name 0" in p for p in dc.collect(r6.map(), []).problems)


def check_serialisation(ds: dc.DevSet) -> bytes:
    blob = dc.build(ds)
    assert len(blob) % 16 == 0
    s = dc.parse_section(blob)
    c = s["counts"]
    assert (c["defs"], c["devices"], c["groups"], c["volumes"], c["starts"], c["cflags"], c["cameras"], c["names"],
            c["ambients"], c["decals"], c["decal_types"], c["bspsw"]) == (7, 9, 3, 4, 2, 2, 2, 6, 2, 2, 2, 2)
    assert s["bsp_count"] == 2 and s["dropped_devices"] == 1 and s["dangling_names"] == 1
    d = s["defs"][0]
    assert d["name"] == "door" and d["ticks"] == (15, 3, 30, 6, 60, 0) and d["open_ticks"] == 90 and d["fx_snd_mask"] == 3
    assert d["pos_anim"] == 0 and d["pos_frames"] == 10 and d["auto_radius"] == 2.0 and d["fn_in"] == (3, 1, 0, 0)
    dv = s["devices"][5]
    assert dv["power_group"] == 2 and dv["position_group"] == 0 and dv["device_flags"] == 16 and dv["extra"] == 3
    assert dv["scen_index"] == 7 and dv["bsp_mask"] == 1 and dv["def_index"] == 3
    assert s["groups"][1] == dict(name="grp_b", initial=1.0, flags=1)
    v = s["volumes"][1]
    assert v["forward"] == (0.0, 1.0, 0.0) and v["left"] == (-1.0, 0.0, 0.0) and v["extents"] == (4.0, 2.0, 3.0)
    assert s["names"][3]["flags"] == 5 and s["names"][3]["aux_index"] == 0 and s["names"][4]["scen_index"] == dc.NONE32
    assert [s["names"][i]["name"] for i in s["name_order"]] == sorted(n.name for n in ds.names)
    assert s["decals"][0]["pitch_raw"] == -64 and s["decal_types"] == ds.decal_types
    # round trip: parse equals the data model for every table
    for i, dev in enumerate(ds.devices):
        p = s["devices"][i]
        assert (p["def_index"], p["flags"], p["bsp_mask"], p["pos"], p["rot"], p["power_group"], p["position_group"],
                p["device_flags"], p["kind_flags"], p["name_index"], p["extra"], p["scen_index"]) == (
            dev.def_index, dev.flags, dev.bsp_mask, dev.pos, dev.rot, dev.power_group, dev.position_group,
            dev.device_flags, dev.kind_flags, dev.name_index, dev.extra, dev.scen_index)
    # empty section
    e = dc.parse_section(dc.build(dc.DevSet()))
    assert all(len(e[k]) == 0 for k in ("defs", "devices", "groups", "volumes", "names", "decals"))
    # reference parser: every truncation, a bad signature and version are rejected
    for n in range(len(blob)):
        expect(lambda n=n: dc.parse_section(blob[:n]))
    expect(lambda: dc.parse_section(b"XXXX" + blob[4:]))
    expect(lambda: dc.parse_section(blob[:4] + struct.pack("<H", 2) + blob[6:]))
    # writer refuses unsorted defs, a device def index out of range, too many BSPs
    ds2 = dc.DevSet(defs=list(reversed(ds.defs)))
    expect(lambda: dc.build(ds2))
    bad = dc.DevSet(devices=[dc.Device(1, 0, 0, (0.0,) * 3, (0.0,) * 3, def_index=3)])
    expect(lambda: dc.build(bad))
    expect(lambda: dc.build(dc.DevSet(bsp_count=33)))
    # strings are cut to fit and stay terminated
    long = dc.DevSet(groups=[dc.Group("x" * 80, 0.5, 0)])
    assert dc.parse_section(dc.build(long))["groups"][0]["name"] == "x" * 31
    return blob


def check_volumes() -> None:
    """volume_contains against the Cramer's-rule reference on random points of the synthetic boxes and of random boxes."""
    ds = dc.collect(build_synthetic()[1], [])
    rnd = random.Random(11)
    boxes = list(ds.volumes[:3])
    for _ in range(20):                                        # random orthonormal boxes
        ax = [rnd.gauss(0, 1) for _ in range(3)]
        n = math.sqrt(sum(a * a for a in ax))
        fw = tuple(a / n for a in ax)
        t = (rnd.gauss(0, 1), rnd.gauss(0, 1), rnd.gauss(0, 1))
        d = sum(a * b for a, b in zip(t, fw))
        up = tuple(t[i] - d * fw[i] for i in range(3))
        un = math.sqrt(sum(a * a for a in up))
        boxes.append(dc.Volume("r", fw, tuple(a / un for a in up), tuple(rnd.uniform(-20, 20) for _ in range(3)),
                               tuple(rnd.uniform(0.5, 8) for _ in range(3))))
    agree = inside = 0
    for v in boxes:
        for _ in range(500):
            if rnd.random() < 0.5:                             # a point known to be inside
                s = [rnd.random() * e for e in v.extents]
                p = tuple(v.position[k] + s[0] * v.forward[k] + s[1] * v.left[k] + s[2] * v.up[k] for k in range(3))
            else:
                p = tuple(v.position[k] + rnd.uniform(-12, 12) for k in range(3))
            ref_ = reference_inside(v, p)
            got = dc.volume_contains(v, p)
            if ref_ != got:                                    # only boundary rounding may differ
                d = tuple(p[i] - v.position[i] for i in range(3))
                loc = [sum(d[i] * ax[i] for i in range(3)) for ax in (v.forward, v.left, v.up)]
                assert any(abs(loc[i]) < 1e-4 or abs(loc[i] - v.extents[i]) < 1e-4 for i in range(3)), (v, p)
            agree += 1
            inside += got
    assert agree == len(boxes) * 500 and 0.2 < inside / agree < 0.8


def check_real_maps() -> None:
    maps = sorted((ROOT / "assets" / "decompressed").glob("*.map")) if (ROOT / "assets" / "decompressed").is_dir() else []
    maps = [p for p in maps if p.stem != "ui"]
    if not maps:
        print("  (no maps under assets/decompressed: real-map checks skipped)")
        return
    for path in maps:
        m = texconv.MapData.open(path)
        probes = sc.load_bsps(m)
        ds = dc.collect(m, probes)
        dc.check_scen(m, ds)
        assert ds.problems == [], (path.stem, ds.problems[:5])
        r = sc.Region(m)
        sb = r.block(m.index.scenario)
        # counts equal the scenario's own blocks
        assert len(ds.groups) == r.reflexive(sb + dc.SCN_GROUPS, 52)[0] and len(ds.volumes) == r.reflexive(sb + dc.SCN_VOLUMES, 96)[0]
        assert len(ds.cflags) == r.reflexive(sb + dc.SCN_CFLAGS, 92)[0] and len(ds.cameras) == r.reflexive(sb + dc.SCN_CAMERAS, 104)[0]
        assert len(ds.names) == r.reflexive(sb + dc.SCN_NAMES, 36)[0] and len(ds.decals) == r.reflexive(sb + dc.SCN_DECALS, 16)[0]
        assert len(ds.ambients) == r.reflexive(sb + dc.SSCE_BLOCK, 40)[0] and len(ds.starts) == 2
        nplaced = sum(r.reflexive(sb + off, stride)[0] for _k, _n, off, stride, _p, _g in dc.DEVICE_BLOCKS)
        assert len(ds.devices) + ds.dropped_devices == nplaced
        # volumes: orthonormal frame, positive extents; the box convention beats the alternatives on the BSP switch volumes
        for v in ds.volumes:
            assert abs(sum(c * c for c in v.forward) - 1) < 1e-3 and abs(sum(c * c for c in v.up) - 1) < 1e-3
            assert abs(sum(a * b for a, b in zip(v.forward, v.up))) < 1e-3 and min(v.extents) > 0
        # ranges of the fields the doc calls verified
        assert all(d.device_flags < 32 and d.kind_flags < 64 and d.extra < 16 for d in ds.devices)
        assert all(abs(s_.facing) <= math.pi + 1e-3 and s_.bsp == 0 for s_ in ds.starts)
        assert all(abs(c.yaw) <= math.pi + 1e-3 and abs(c.pitch) <= math.pi / 2 + 1e-3 for c in ds.cflags)
        assert all(abs(c.ori[0]) <= math.pi + 1e-3 and abs(c.ori[1]) <= math.pi / 2 + 1e-3 and 0.0 <= c.fov <= 2.5 for c in ds.cameras)
        assert all(n.obj_type in dc.OBJ_TYPE_KIND for n in ds.names) and all(0.0 <= g.initial <= 1.0 and g.flags < 2 for g in ds.groups)
        assert all(0 <= b.unknown <= 0xFFFF and b.source != b.dest for b in ds.bspsw) and all(d.type < len(ds.decal_types) for d in ds.decals)
        assert all(df.subtype == 0 for df in ds.defs if df.kind == dc.K_CONTROL) and all(df.dev_flags < 4 for df in ds.defs)
        # devices: group indices valid, SCEN indices strictly ascending, tags consistent with the raw floats
        assert all(a.scen_index < b.scen_index for a, b in zip(ds.devices, ds.devices[1:]))
        for d in ds.devices:
            assert -1 <= d.power_group < len(ds.groups) and -1 <= d.position_group < len(ds.groups)
            assert d.bsp_mask < 1 << len(probes)
        for df in ds.defs:
            e = r.entry(df.tag_id)
            b = r.block(e)
            times = r.u("<6f", b + dc.DEV_TIMES)
            assert df.ticks == tuple(int(round(t * 30)) for t in times)
            if df.kind == dc.K_MACHINE:
                assert df.open_ticks == r.u("<I", b + dc.MACH_TICKS)[0]
                assert (df.auto_radius > 0) <= (df.subtype == 0)          # only doors have an activation radius
        # names: every name carried by a placement resolves to a SCEN placement of the same kind and position
        objs = sc.collect_objects(m, [])
        for k, n in enumerate(ds.names):
            if n.flags & dc.NF_RESOLVED and n.scen_index != dc.NONE32:
                assert objs.placements[n.scen_index].name_index == k
            if n.flags & dc.NF_DEVICE:
                assert ds.devices[n.aux_index].name_index == k
        # serialisation round trip
        blob = dc.build(ds)
        s = dc.parse_section(blob)
        assert s["counts"]["devices"] == len(ds.devices) and s["counts"]["names"] == len(ds.names)
        assert [s["names"][i]["name"] for i in s["name_order"]] == sorted(n.name for n in ds.names)
    # the box convention on a10 (the maps with BSP switch volumes straddling two BSPs)
    a10 = ROOT / "assets" / "decompressed" / "a10.map"
    if a10.exists():
        m = texconv.MapData.open(a10)
        probes = sc.load_bsps(m)
        res = dc.verify_volume_convention(dc.collect(m, probes), probes)
        best = res["corner, left = up x forward"]
        assert best > 0.5 and all(best > v + 0.2 for k, v in res.items() if k != "corner, left = up x forward"), res
        # a10 is the map with the most devices: 245 machines, no controls
        ds = dc.collect(m, probes)
        assert len(ds.devices) == 245 and len(ds.groups) == 9 and len(ds.volumes) == 126 and len(ds.ambients) == 39
        assert len(ds.bspsw) == 12 and len(ds.names) == 379 and len(ds.cflags) == 221 and len(ds.cameras) == 292
    print(f"  real maps: {len(maps)} converted and cross-checked against SCEN")


def main() -> None:
    ds = check_collect()
    check_scen_cross(ds)
    check_tag_problems()
    check_group_ranges()
    check_corrupt_map()
    blob = check_serialisation(ds)
    check_volumes()
    check_real_maps()
    fixture = os.environ.get("DEVS_FIXTURE")
    if fixture:
        Path(fixture).write_bytes(hp2.build("synthetic", [("DEVS", blob)]))
    print("devconv tests passed")


if __name__ == "__main__":
    main()
