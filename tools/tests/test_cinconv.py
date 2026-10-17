"""CINE section on a synthetic map (no game data needed): recorded animation streams (writer / walker / reference player,
tick rule, aim wrap and clamp), camera tracks (conjugated orientations), titles (ticks, string index checks), help strings,
animation references and graph / sound roots, the section writer and its strict reference parser, corruption handling. Set
CINE_FIXTURE=<path> to also write the .hp2 the C reader tests (engine/tests/test_cinedef.c, test_cine.c, which hard-code the
numbers of this world) open. Skip-if-missing checks convert every campaign map found in assets/decompressed/ and verify the
format rules on all of its recordings."""
import math
import os
import struct
import sys
from pathlib import Path

sys.path[:0] = [str(Path(__file__).resolve().parents[1]), str(Path(__file__).resolve().parent)]
from halo2ps2 import cinconv as cc, hp2, tagindex  # noqa: E402
import test_scenconv as scn  # noqa: E402

BASE = tagindex.XBOX_TAG_BASE
ROOT = Path(__file__).resolve().parents[2]
S = "levels\\t\\t"
TAGS = [
    ("scnr", S, 0x8000), ("ustr", "levels\\t\\ingame help text", 0x2000), ("hmt ", "levels\\t\\hud messages", 0x2000),
    ("trak", "camera\\t\\small", 0x2000), ("trak", "camera\\t\\gunner", 0x2000),
    ("antr", "characters\\a\\a", 0x2000), ("antr", "characters\\b\\b", 0x2000),
    ("snd!", "sound\\one", 0x100), ("snd!", "sound\\two", 0x100), ("lsnd", "sound\\loop", 0x100), ("effe", "effects\\boom", 0x100),
]
STRINGS = ["Alpha", "Beta é", "", "Gamma", "Delta"]
PI = math.pi


# ---- the synthetic world (engine/tests/test_cinedef.c and test_cine.c hard-code these numbers) ---------------------------
def streams() -> list[tuple[str, int, cc.Ctl, bytes]]:
    """(name, control data version, initial state, stream) of the four recordings."""
    ev = cc.encode_event
    a_init = cc.Ctl(mode=3, flags=0x0300, weapon=-1, extra_a=-1, extra_b=-1, yaw=100, pitch=0)
    a = (ev(cc.T_THROTTLE, 1, struct.pack("<2f", 1.0, 0.0)) +
         ev(cc.T_AIM_SMALL, 0, struct.pack("<2b", 3, -2)) +
         ev(cc.T_FLAGS, 4, struct.pack("<H", 0x0800)) +
         ev(cc.T_AIM_LARGE, 300, struct.pack("<2h", 1950, 5)) +
         ev(cc.T_WEAPON, 0, struct.pack("<h", 1)) +
         ev(cc.T_WAIT, 20))                                   # waits 1 + 0 + 4 + 300 + 0 + 20: 326 ticks
    b_init = cc.Ctl(mode=3, flags=0x0300, weapon=0, extra_a=1, yaw=-1000, pitch=-500)
    b = ev(cc.T_AIM_SMALL, 10, struct.pack("<2b", -1, -1))      # yaw wraps to 999, pitch stays clamped: 11 ticks
    c_init = cc.Ctl(mode=0, flags=0x12, weapon=-1, yaw=0, pitch=0)
    c = ev(cc.T_MODE, 0, bytes([3])) + ev(cc.T_THROTTLE, 2, struct.pack("<2f", 0.5, -0.5))      # 3 ticks
    return [("rec_a", 4, a_init, a), ("rec_b", 3, b_init, b), ("rec_c", 0, c_init, c), ("rec_a", 4, a_init, a)]


LENGTHS = [326, 11, 3, 326]


def track_points(angles_deg, centre, radius, fixed=None) -> list[tuple]:
    """Tag points (Xbox convention: the conjugate of the Hamilton quaternion)."""
    out = []
    for a in angles_deg:
        r = math.radians(a)
        pos = fixed or (centre[0] - radius * math.cos(r), 0.0, centre[2] + radius * math.sin(r))
        out.append((*pos, 0.0, -math.sin(r / 2), 0.0, math.cos(r / 2)))
    return out


SMALL = track_points([90 - 22.5 * k for k in range(9)], (0.0, 0.0, 0.5), 2.0)
GUNNER = track_points([90.0, 20.0, -80.0], None, 0.0, fixed=(0.34, 0.0, 0.16))


def build_world(title_index_bad: bool = True) -> tuple[scn.Region, object]:
    r = scn.Region(TAGS)
    sb = r.block(S)

    def E(group, path):
        return scn.entry(r, group, path)

    # help strings (ustr: 20 B entries {bytes incl NUL, flags, file offset, pointer, id})
    e = E("ustr", "levels\\t\\ingame help text")
    ents = bytearray()
    for text in STRINGS:
        raw = text.encode("utf-16le") + b"\0\0"
        ptr = r.alloc_at(e, raw)
        ents += struct.pack("<IIII", len(raw), 0, 0, ptr).ljust(20, b"\0")
    r.refl(e.data_address - BASE, len(STRINGS), r.alloc_at(e, bytes(ents)))
    r.region[sb + cc.SCN_HELP:sb + cc.SCN_HELP + 16] = struct.pack("<4sIII", b"rtsu", 0, 0, e.tag_id)
    # hud messages (hmt): two messages of one text element each
    e = E("hmt ", "levels\\t\\hud messages")
    text = "Hello\0World\0".encode("utf-16le")
    b = e.data_address - BASE
    r.poke(b, "<IIII", len(text), 0, 0, r.alloc_at(e, text))
    r.refl(b + 0x14, 2, r.alloc_at(e, bytes([0, 6, 0, 6])))
    msgs = struct.pack("<32sHHH", b"m0", 0, 0, 1).ljust(64, b"\0") + struct.pack("<32sHHH", b"m1", 6, 1, 1).ljust(64, b"\0")
    r.refl(b + 0x20, 2, r.alloc_at(e, msgs))
    r.region[sb + 0x594:sb + 0x5A4] = struct.pack("<4sIII", b" tmh", 0, 0, e.tag_id)
    # recorded animations
    recs = bytearray()
    for (name, ver, init, stream), length in zip(streams(), LENGTHS):
        data = cc.encode_header(init, ver) + stream
        ptr = r.alloc(S, data)
        rec = bytearray(64)
        rec[:len(name)] = name.encode()
        struct.pack_into("<bbhh", rec, 0x20, 4, 0, ver, length)
        struct.pack_into("<IIII", rec, 0x2C, len(data), 0, 0, ptr)
        struct.pack_into("<I", rec, 0x3C, 0)
        recs += rec
    r.refl(sb + cc.SCN_RECORDINGS, 4, r.alloc(S, bytes(recs)))
    # camera tracks
    for path, pts in (("camera\\t\\small", SMALL), ("camera\\t\\gunner", GUNNER)):
        e = E("trak", path)
        data = b"".join(struct.pack("<7f", *p).ljust(60, b"\0") for p in pts)
        r.refl(e.data_address - BASE + 4, len(pts), r.alloc_at(e, data))
    # titles
    titles = bytearray()
    for name, string, style, rect, times in (("t_first", 1, 1, (425, 0, 446, 597), (30.0, 120.0, 30.0)),
                                              ("t_bad", 9 if title_index_bad else 3, 0, (425, 50, 444, 597), (0.0, 90.0, 0.0))):
        t = bytearray(96)
        t[4:4 + len(name)] = name.encode()
        struct.pack_into("<4h", t, 0x28, *rect)
        struct.pack_into("<h", t, 0x30, string)
        struct.pack_into("<HH", t, 0x34, style, 0)
        t[0x3C:0x40] = bytes([255, 255, 255, 255])         # white, opaque (B G R A)
        t[0x40:0x44] = bytes([0, 0, 0, 255])               # black, opaque
        struct.pack_into("<3f", t, 0x44, *times)
        titles += t
    r.refl(sb + cc.SCN_TITLES, 2, r.alloc(S, bytes(titles)))
    # ai animation references and script references
    ga, gb = E("antr", "characters\\a\\a"), E("antr", "characters\\b\\b")
    # graph a has the animation "idle_a"
    an = struct.pack("<32sHHHH", b"idle_a", 0, 10, 0, 0).ljust(scn.sc.ANIM_REC, b"\0")
    r.refl(ga.data_address - BASE + scn.sc.ANIM_ANIMATIONS, 1, r.alloc_at(ga, an))
    arefs = b"".join(struct.pack("<32s4sIII", n, b"rtna", 0, 0, g.tag_id).ljust(60, b"\0")
                     for n, g in ((b"idle_a", ga), (b"zzz", gb)))
    r.refl(sb + cc.SCN_AI_ANIMS, 2, r.alloc(S, arefs))
    refs = b"".join(bytes(24) + struct.pack("<4sIII", grp[::-1].encode(), 0, 0, E(grp, path).tag_id)
                    for grp, path in (("snd!", "sound\\one"), ("snd!", "sound\\two"), ("lsnd", "sound\\loop"),
                                      ("antr", "characters\\a\\a"), ("antr", "characters\\a\\a"), ("effe", "effects\\boom")))
    r.refl(sb + cc.SCN_REFERENCES, 6, r.alloc(S, refs))
    return r, r.map()


def expect(fn, exc=cc.CineError) -> None:
    try:
        fn()
    except exc:
        return
    raise SystemExit("corrupt input accepted")


# ---- tests ----------------------------------------------------------------------------------------------------------
def check_streams() -> None:
    # tick rule and playback of the first recording (hand computed in the comments of streams())
    name, ver, init, stream = streams()[0]
    assert cc.check_stream(stream, 326) == 6
    states = list(cc.play(init, stream, 326))
    assert len(states) == 326
    s0, s1, s4, s5, s304, s305, s325 = (states[i] for i in (0, 1, 4, 5, 304, 305, 325))
    assert s0.throttle == (1.0, 0.0) and (s0.yaw, s0.pitch, s0.flags, s0.weapon) == (100, 0, 0x0300, -1)
    assert (s1.yaw, s1.pitch, s1.flags) == (103, -2, 0x0800) and s4 == s1
    assert (s5.yaw, s5.pitch) == (53, 3), (s5.yaw, s5.pitch)           # 103 + 1950 wraps to 53
    assert s304.weapon == -1 and s305.weapon == 1 and s325 == s305
    # yaw wrap and pitch clamp
    _, _, init_b, stream_b = streams()[1]
    st = list(cc.play(init_b, stream_b, 11))
    assert (st[0].yaw, st[0].pitch) == (999, -500) and st[10] == st[0]
    # wrong length, unknown type, truncated event
    expect(lambda: cc.check_stream(stream, 327))
    expect(lambda: cc.check_stream(bytes([3 << 2]), 1))                   # type 3 does not exist
    expect(lambda: cc.check_stream(bytes([cc.T_FLAGS << 2 | 2, 5, 0]), 6))   # payload cut short
    expect(lambda: cc.check_stream(cc.encode_event(cc.T_THROTTLE, 0, struct.pack("<2f", float("nan"), 0.0)), 1))
    # header round trip for every version
    for ver in (0, 3, 4):
        c = cc.Ctl(mode=3, aux=0, word2=2, weapon=0, extra_a=1 if ver >= 3 else 0, extra_b=-1 if ver >= 4 else 0,
                   flags=0x0900, throttle=(0.5, -0.5), yaw=-321, pitch=77)
        got, size = cc.decode_header(cc.encode_header(c, ver), ver)
        assert size == cc.HEADER_SIZES[ver] and got == c, (ver, got, c)
    expect(lambda: cc.decode_header(bytes(100), 2))
    expect(lambda: cc.decode_header(bytes(40), 4))
    # the vectors of the header are the unit vectors of the quantised angles
    h = cc.encode_header(cc.Ctl(yaw=-500, pitch=0), 4)
    assert abs(struct.unpack_from("<f", h, 16)[0]) < 1e-6 and abs(struct.unpack_from("<f", h, 20)[0] + 1.0) < 1e-6
    # waits of every kind round trip
    for wait in (0, 1, 2, 255, 256, 65535):
        e = cc.encode_event(cc.T_WAIT, wait)
        assert list(cc.walk_stream(e))[0][3] == wait
    expect(lambda: cc.encode_event(cc.T_WAIT, 65536))
    # state hash is order sensitive and stable
    assert cc.state_hash(cc.play(init, stream, 326)) == cc.state_hash(cc.play(init, stream, 326))
    assert cc.state_hash(cc.play(init, stream, 326)) != cc.state_hash(cc.play(init_b, stream_b, 11))


def check_collect() -> cc.CineSet:
    r, m = build_world()
    cs = cc.collect_map(m, "t")
    assert [p for p in cs.problems if "title 1" not in p] == [], cs.problems
    assert len(cs.problems) == 1 and "string index 9" in cs.problems[0]
    assert [x.name for x in cs.recordings] == ["rec_a", "rec_b", "rec_c", "rec_a"]
    assert [x.length for x in cs.recordings] == LENGTHS and [x.ctl_version for x in cs.recordings] == [4, 3, 0, 4]
    assert [x.events for x in cs.recordings] == [6, 1, 2, 6]
    assert cs.recordings[0].init.yaw == 100 and cs.recordings[2].init.flags == 0x12
    # tracks: sorted by tag id, conjugated orientation: the first point looks down (+90 degrees about y, forward (0, 0, -1))
    assert len(cs.tracks) == 2 and cs.tracks[0].tag_id < cs.tracks[1].tag_id
    small = next(t for t in cs.tracks if len(t.points) == 9)
    x, y, z, qx, qy, qz, qw = small.points[0]
    assert abs(qy - math.sin(math.radians(45))) < 1e-6 and abs(qw - math.cos(math.radians(45))) < 1e-6
    assert abs(qx) < 1e-9 and abs(qz) < 1e-9
    fwd_z = 2 * (qx * qz - qw * qy)
    assert abs(fwd_z + 1.0) < 1e-6 and z > 2.0                              # forward straight down, camera above
    last = small.points[-1]
    assert abs(2 * (last[3] * last[5] - last[6] * last[4]) - 1.0) < 1e-6 and last[2] < -1.0
    # titles
    assert [t.name for t in cs.titles] == ["t_first", "t_bad"]
    t0 = cs.titles[0]
    assert (t0.top, t0.left, t0.bottom, t0.right, t0.string, t0.style) == (425, 0, 446, 597, 1, 1)
    assert (t0.fade_in, t0.up, t0.fade_out) == (30, 120, 30)
    assert t0.colour == 0x80808080 and t0.shadow == 0x80000000, (hex(t0.colour), hex(t0.shadow))
    assert cs.titles[1].string == cc.NONE16
    assert cs.strings == STRINGS and cs.help_tag and cs.message_count == 2
    # references
    assert [a.name for a in cs.anim_refs] == ["idle_a", "zzz"] and [a.resolved for a in cs.anim_refs] == [1, 0]
    assert len(cs.graphs) == 2 and cs.graphs == sorted(cs.graphs)
    assert len(cs.sounds) == 3 and cs.reference_groups == {"snd!": 2, "lsnd": 1, "antr": 2, "effe": 1}
    assert cc.referenced_anims(cs) == set(cs.graphs) and cc.referenced_sounds(cs) == set(cs.sounds)
    return cs


def check_serialisation(cs: cc.CineSet) -> bytes:
    blob = cc.build(cs)
    assert len(blob) % 16 == 0
    sec = cc.parse_section(blob)
    assert len(sec["recs"]) == 4 and len(sec["tracks"]) == 2 and len(sec["titles"]) == 2 and sec["strings"] == STRINGS
    assert sec["recs"][3]["stream"] == sec["recs"][0]["stream"] and sec["recs"][0]["length"] == 326
    # identical streams are stored once: the header's pool size is smaller than the sum of the stream sizes
    pool = struct.unpack_from("<I", blob, 0x18)[0]
    assert pool < sum(len(x.stream) for x in cs.recordings) + 16 + 32
    assert [t["tag"] for t in sec["tracks"]] == sorted(t["tag"] for t in sec["tracks"])
    assert sec["titles"][0]["times"] == (30, 120, 30) and sec["titles"][0]["rect"] == (425, 0, 446, 597)
    assert sec["titles"][1]["string"] == cc.NONE16
    assert [a["resolved"] for a in sec["arefs"]] == [1, 0] and sec["graphs"] == cs.graphs
    # parse -> play equals the collected state
    for rec, got in zip(cs.recordings, sec["recs"]):
        assert list(cc.play(got["init"], got["stream"], got["length"])) == list(cc.play(rec.init, rec.stream, rec.length))
    # the strict parser rejects broken sections
    expect(lambda: cc.parse_section(blob[:-16]))
    expect(lambda: cc.parse_section(b"XXXX" + blob[4:]))
    bad = bytearray(blob)
    bad[0x14] ^= 0x01                                    # pool offset unaligned
    expect(lambda: cc.parse_section(bytes(bad)))
    rec_off = struct.unpack_from("<I", blob, 0x10)[0]
    bad = bytearray(blob)
    struct.pack_into("<I", bad, rec_off + 0x28, 327)     # tag length no longer matches the stream
    expect(lambda: cc.parse_section(bytes(bad)))
    bad = bytearray(blob)
    bad[rec_off + 0x2E] = 5                              # unknown control data version
    expect(lambda: cc.parse_section(bytes(bad)))
    bad = bytearray(blob)
    struct.pack_into("<f", bad, rec_off + 0x3C, float("inf"))
    expect(lambda: cc.parse_section(bytes(bad)))
    # every single byte flip either parses or raises CineError (never anything else)
    for i in range(0, len(blob), 3):
        bad = bytearray(blob)
        bad[i] ^= 0xFF
        try:
            cc.parse_section(bytes(bad))
        except cc.CineError:
            pass
    return blob


def check_corrupt_map() -> None:
    sb = None
    for field, why in ((cc.SCN_RECORDINGS, "recordings"), (cc.SCN_TITLES, "titles"), (cc.SCN_REFERENCES, "references")):
        r, m = build_world()
        sb = r.block(S)
        r.poke(sb + field, "<II", 5, 0xFFFF0000)
        expect(lambda: cc.collect_map(r.map(), "t"))
    # a recording whose stream points outside the region
    r, m = build_world()
    sb = r.block(S)
    n, o = scn.sc.Region(m).reflexive(sb + cc.SCN_RECORDINGS, 64)
    r.poke(o + 0x2C + 12, "<I", 0x10)
    expect(lambda: cc.collect_map(r.map(), "t"))
    # a recording with the wrong length
    r, m = build_world()
    sb = r.block(S)
    n, o = scn.sc.Region(m).reflexive(sb + cc.SCN_RECORDINGS, 64)
    r.poke(o + 0x24, "<h", 77)
    expect(lambda: cc.collect_map(r.map(), "t"))
    # a track with a NaN position
    r, m = build_world()
    e = scn.entry(r, "trak", "camera\\t\\small")
    n, o = scn.sc.Region(m).reflexive(e.data_address - BASE + 4, 60)
    r.poke(o, "<I", 0x7FC00000)
    expect(lambda: cc.collect_map(r.map(), "t"))
    # a title with a NaN time
    r, m = build_world()
    sb = r.block(S)
    n, o = scn.sc.Region(m).reflexive(sb + cc.SCN_TITLES, 96)
    r.poke(o + 0x44, "<I", 0x7FC00000)
    expect(lambda: cc.collect_map(r.map(), "t"))
    # an ai animation reference to a tag that is not a graph is skipped with a note
    r, m = build_world()
    sb = r.block(S)
    n, o = scn.sc.Region(m).reflexive(sb + cc.SCN_AI_ANIMS, 60)
    r.region[o + 0x20:o + 0x30] = struct.pack("<4sIII", b"rtna", 0, 0, scn.entry(r, "snd!", "sound\\one").tag_id)
    cs = cc.collect_map(r.map(), "t")
    assert any("animation graph" in p for p in cs.problems) and len(cs.anim_refs) == 1
    # no help text at all: strings empty, titles lose their text
    r, m = build_world()
    sb = r.block(S)
    r.region[sb + cc.SCN_HELP:sb + cc.SCN_HELP + 16] = struct.pack("<4sIII", b"rtsu", 0, 0, 0xFFFFFFFF)
    cs = cc.collect_map(r.map(), "t")
    assert cs.strings == [] and cs.help_tag == 0 and all(t.string == cc.NONE16 for t in cs.titles)
    # a scenario without any of it
    r = scn.Region([("scnr", S, 0x8000)])
    cs = cc.collect_map(r.map(), "empty")
    assert not cs.recordings and not cs.tracks and not cs.titles and cs.problems == []
    sec = cc.parse_section(cc.build(cs))
    assert sec["recs"] == [] and sec["graphs"] == []


def check_real_maps() -> None:
    maps = [p for p in sorted((ROOT / "assets" / "decompressed").glob("*.map")) if p.stem != "ui"]
    if not maps:
        print("skip: no assets/decompressed maps")
        return
    total = 0
    for p in maps:
        cs = cc.collect(p)
        assert cs.problems == [], (p.stem, cs.problems)
        blob = cc.build(cs)
        sec = cc.parse_section(blob)
        assert len(sec["recs"]) == len(cs.recordings)
        for rec in cs.recordings:
            total += 1
            # tag length == 1 + sum of waits, and the aim integer stays in range over the whole recording
            assert cc.check_stream(rec.stream, rec.length) == rec.events
            yaw_lo = yaw_hi = rec.init.yaw
            plain = rec.init.yaw
            pitch = rec.init.pitch
            for _pos, typ, _k, _w, payload in cc.walk_stream(rec.stream):
                if typ in (cc.T_AIM_SMALL, cc.T_AIM_LARGE):
                    dy, dp = struct.unpack("<2b" if typ == cc.T_AIM_SMALL else "<2h", payload)
                    plain += dy
                    pitch += dp
                    yaw_lo, yaw_hi = min(yaw_lo, plain), max(yaw_hi, plain)
                    assert abs(pitch) <= cc.PITCH_LIMIT, (p.stem, rec.name, pitch)      # the clamp never engages
            assert -1000 <= yaw_lo and yaw_hi <= 1000, (p.stem, rec.name, yaw_lo, yaw_hi)
        for t in cs.tracks:
            assert len(t.points) >= 2
            for pt in t.points:
                assert abs(math.sqrt(sum(v * v for v in pt[3:])) - 1.0) < 1e-3
        for tt in cs.titles:
            assert tt.string != cc.NONE16 and tt.string < len(cs.strings)
        assert set(cc.referenced_anims(cs)) <= {e.tag_id for e in __import__("halo2ps2.texconv", fromlist=["x"]).MapData.open(p).index.entries
                                                   if e.group == "antr"}
        # determinism of the reference player on one recording
        if cs.recordings:
            r0 = cs.recordings[-1]
            assert cc.state_hash(cc.play(r0.init, r0.stream, r0.length)) == cc.state_hash(cc.play(r0.init, r0.stream, r0.length))
    print(f"real maps: {len(maps)} maps, {total} recordings checked")
    a10 = ROOT / "assets" / "decompressed" / "a10.map"
    if a10.exists():
        cs = cc.collect(a10)
        assert len(cs.recordings) == 99 and len(cs.tracks) == 7 and len(cs.titles) == 3 and len(cs.strings) == 7


def main() -> None:
    check_streams()
    cs = check_collect()
    blob = check_serialisation(cs)
    check_corrupt_map()
    check_real_maps()
    pack = hp2.build("test", [("CINE", blob)])
    got = hp2.parse(pack)
    assert cc.parse_section(got.read("CINE"))["strings"] == STRINGS
    assert cc.golden.__doc__
    if os.environ.get("CINE_FIXTURE"):
        Path(os.environ["CINE_FIXTURE"]).write_bytes(pack)
    print("ok")


if __name__ == "__main__":
    main()
