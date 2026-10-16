"""ANIM section conversion on a synthetic map: model_animations tags (uncompressed and compressed
animations, a node-less device graph, hinge/static/animated/scale channels, frame info), models and
objects are written as Xbox tag structs, converted, read back and sampled with the reference sampler,
and compared against the decoded source frames. Optional checks run on the real a10 map when
assets/decompressed/a10.map exists (skipped otherwise).

Set ANIM_FIXTURE=<path> to also write the resulting pack (ANIM + AEXP: the reference sampler's expected
poses) for the C reader test (engine/tests/test_anim.c)."""
import contextlib
import dataclasses
import io
import math
import os
import random
import struct
import sys
import tempfile
from pathlib import Path

sys.path[:0] = [str(Path(__file__).resolve().parents[1]), str(Path(__file__).resolve().parent)]
from halo2ps2 import animconv as ac  # noqa: E402
from halo2ps2 import hp2, mapinfo, tagindex  # noqa: E402
from test_mapinfo import make_header  # noqa: E402

BASE = tagindex.XBOX_TAG_BASE
SALT = 0xE174
WU_MM = ac.WU_MM


# ---- tiny quaternion toolbox ---------------------------------------------------------------------------
def axis_angle(axis, ang):
    n = math.sqrt(sum(c * c for c in axis))
    s = math.sin(ang / 2) / n
    return (axis[0] * s, axis[1] * s, axis[2] * s, math.cos(ang / 2))


def qmul(a, b):
    ax, ay, az, aw = a
    bx, by, bz, bw = b
    return (aw * bx + ax * bw + ay * bz - az * by, aw * by - ax * bz + ay * bw + az * bx,
            aw * bz + ax * by - ay * bx + az * bw, aw * bw - ax * bx - ay * by - az * bz)


# ---- Xbox tag writers -----------------------------------------------------------------------------------
def fourcc_ref(group: str, tag_id: int) -> bytes:
    return group.encode()[::-1] + struct.pack("<III", 0, 0, tag_id)


def node_block(names, rest_rot=None, rest_trans=None):
    out = b""
    for i, n in enumerate(names):
        out += n.encode().ljust(32, b"\0") + struct.pack("<hhhhI3ff4x", -1 if i == 0 else i + 1, i + 1, max(i - 1, 0), 0, 1,
                                                          0.1, 0.0, 0.0, 0.5)
    return out


def td(size, ptr):
    return struct.pack("<IIIII", size, 0, 0, ptr if size else 0, 0)


def uncompressed_parts(nn, fc, rot, trans, scale, frame_info=None):
    """rot/trans/scale: per node either a value (static) or a list of fc values (animated)."""
    def anim(v, n_single):
        return isinstance(v, list)
    rf = tf = sf = 0
    defaults, frames = b"", [b""] * fc
    for n in range(nn):
        for ch, vals in ((0, rot[n]), (1, trans[n]), (2, scale[n])):
            animated = isinstance(vals, list)
            if animated:
                if ch == 0:
                    rf |= 1 << n
                elif ch == 1:
                    tf |= 1 << n
                else:
                    sf |= 1 << n
            enc = []
            for f in range(fc if animated else 1):
                v = vals[f] if animated else vals
                if ch == 0:
                    enc.append(struct.pack("<4h", *(round(c * 32767) for c in v)))
                elif ch == 1:
                    enc.append(struct.pack("<3f", *v))
                else:
                    enc.append(struct.pack("<f", v))
            if animated:
                for f in range(fc):
                    frames[f] += enc[f]
            else:
                defaults += enc[0]
    fs = len(frames[0]) if fc else 0
    fi = b"".join(struct.pack("<%df" % len(v), *v) for v in frame_info) if frame_info else b""
    return dict(rot_flags=rf, trans_flags=tf, scale_flags=sf, default=defaults, frame_data=b"".join(frames),
                frame_size=fs, frame_info=fi, flags=0)


def compressed_parts(nn, fc, rot_def, rot_keys, trans_def, trans_keys, frame_info=None):
    """rot_def/trans_def: value per node (frame 0 / static); *_keys: {node: [(frame, value)]}, a node with an empty
    list is flagged animated without keys."""
    rot_nodes, tr_nodes = sorted(rot_keys), sorted(trans_keys)
    keys = [v for n in rot_nodes for v in rot_keys[n]]
    tkeys = [v for n in tr_nodes for v in trans_keys[n]]
    table, first = b"", 0
    for n in rot_nodes:
        table += struct.pack("<I", first << 12 | len(rot_keys[n]))
        first += len(rot_keys[n])
    ridx = b"".join(struct.pack("<H", f) for f, _ in keys)
    rdef = b"".join(ac.pack_q48(q).to_bytes(6, "little") for q in rot_def)
    rval = b"".join(ac.pack_q48(q).to_bytes(6, "little") for _, q in keys)
    ttable, first = b"", 0
    for n in tr_nodes:
        ttable += struct.pack("<I", first << 12 | len(trans_keys[n]))
        first += len(trans_keys[n])
    tidx = b"".join(struct.pack("<H", f) for f, _ in tkeys)
    tdef = b"".join(struct.pack("<3f", *t) for t in trans_def)
    tval = b"".join(struct.pack("<3f", *v) for _, v in tkeys)
    o0 = 44 + len(table)
    o1 = o0 + len(ridx)
    o2 = o1 + len(rdef)
    o3 = o2 + len(rval)
    o4 = o3 + len(ttable)
    o5 = o4 + len(tidx)
    o6 = o5 + len(tdef)
    o7 = o6 + len(tval)
    head = struct.pack("<11I", o0, o1, o2, o3, o4, o5, o6, o7, o7, o7, o7)
    blob = head + table + ridx + rdef + rval + ttable + tidx + tdef + tval
    rf = sum(1 << n for n in rot_nodes)
    tf = sum(1 << n for n in tr_nodes)
    fi = b"".join(struct.pack("<%df" % len(v), *v) for v in frame_info) if frame_info else b""
    return dict(rot_flags=rf, trans_flags=tf, scale_flags=0, default=b"", frame_data=blob, frame_size=0,
                frame_info=fi, flags=ac.F_COMPRESSED)


def antr_block(addr, node_names, anims, checksum):
    """Whole data block of an antr tag at absolute address addr: [0x80 struct][nodes][anim records][blobs]."""
    nodes = node_block(node_names)
    node_off = 0x80
    anim_off = node_off + len(nodes)
    blob_off = anim_off + 180 * len(anims)
    recs, blobs = b"", b""
    cursor = blob_off

    def put(raw):
        nonlocal cursor, blobs
        at = cursor
        blobs += raw
        cursor += len(raw)
        return addr + at
    for a in anims:
        fi_ptr = put(a["frame_info"])
        dd_ptr = put(a["default"])
        fd_ptr = put(a["frame_data"])
        fit = {0: 0, 8: 1, 12: 2, 16: 3}[len(a["frame_info"]) // a["fc"] if a["frame_info"] else 0]
        nn = a["nn"]
        b = bytearray(180)
        b[:32] = a["name"].encode().ljust(32, b"\0")
        struct.pack_into("<hhhhI", b, 0x20, a["type"], a["fc"], a["frame_size"], fit, checksum)
        struct.pack_into("<hhf", b, 0x2C, nn, a.get("loop", 0), 0.0)
        struct.pack_into("<hHhh", b, 0x38, -1, a["flags"], -1, 0)
        struct.pack_into("<f", b, 0x44, 1.0)
        b[0x48:0x5C] = td(len(a["frame_info"]), fi_ptr)
        struct.pack_into("<2I", b, 0x5C, a["trans_flags"] & 0xFFFFFFFF, a["trans_flags"] >> 32)
        struct.pack_into("<2I", b, 0x6C, a["rot_flags"] & 0xFFFFFFFF, a["rot_flags"] >> 32)
        struct.pack_into("<2I", b, 0x7C, a["scale_flags"] & 0xFFFFFFFF, a["scale_flags"] >> 32)
        b[0x8C:0xA0] = td(len(a["default"]), dd_ptr)
        b[0xA0:0xB4] = td(len(a["frame_data"]), fd_ptr)
        recs += bytes(b)
    head = bytearray(0x80)
    struct.pack_into("<III", head, ANTR_NODES, len(node_names), addr + node_off if node_names else 0, 0)
    struct.pack_into("<III", head, ANTR_ANIMS, len(anims), addr + anim_off, 0)
    return bytes(head) + nodes + recs + blobs


ANTR_NODES, ANTR_ANIMS = ac.ANTR_NODE_AT, ac.ANTR_ANIM_AT


def model_block(addr, names, rest_rot, rest_trans, checksum):
    out = bytearray(0xE8)
    struct.pack_into("<II", out, 0, 0, checksum)
    nodes = b""
    for i, n in enumerate(names):
        nodes += (n.encode().ljust(32, b"\0") + struct.pack("<hhhh", -1, -1, -1, 0) + struct.pack("<3f", *rest_trans[i])
                  + struct.pack("<4f", *rest_rot[i]) + bytes(156 - 68))
    struct.pack_into("<III", out, ac.MODEL_NODES_AT, len(names), addr + 0xE8, 0)
    return bytes(out) + nodes


def object_block(model_id, antr_id):
    out = bytearray(0x100)
    out[ac.OBJ_MODEL_AT:ac.OBJ_MODEL_AT + 16] = fourcc_ref("mode", model_id)
    out[ac.OBJ_ANIM_AT:ac.OBJ_ANIM_AT + 16] = fourcc_ref("antr", antr_id)
    return bytes(out)


def build_map(path: Path, specs):
    """specs: [(group, path, builder(addr) -> bytes)]; the first must be the scenario."""
    sizes = [(g, p, len(b(0))) for g, p, b in specs]
    region = bytearray(tagindex.TagIndex.build(sizes, scenario_index=0))
    idx = tagindex.TagIndex.parse_region(bytes(region), 0x1000)
    for e, (g, p, b) in zip(idx.entries, specs):
        raw = b(e.data_address)
        o = e.data_address - BASE
        region[o:o + len(raw)] = raw
    tag_off = 0x1000
    body = bytes(tag_off - mapinfo.HEADER_SIZE) + bytes(region)
    path.write_bytes(make_header(mapinfo.HEADER_SIZE + len(body), tag_off, len(region)) + body)
    return idx


def tag_id(i: int) -> int:
    return ((SALT + i) << 16) | i


# ---- the synthetic world ------------------------------------------------------------------------------------------
NODES = ["root", "hip", "knee", "foot", "gizmo"]
CHECK = 0x12345678
REST_ROT = [(0.0, 0.0, 0.0, 1.0), axis_angle((0, 1, 0), 0.4), axis_angle((0, 0, 1), -0.2), (0.0, 0.0, 0.0, 1.0),
            axis_angle((1, 0, 0), 1.0)]
REST_TRANS = [(0.0, 0.0, 0.5), (0.0, 0.0, 0.1), (0.3, 0.0, 0.0), (0.25, 0.0, 0.0), (0.05, 0.1, 0.0)]


def smooth_rot(fc, axis, amp, phase=0.0, base=(0, 0, 0, 1), flip=False):
    out = []
    for f in range(fc):
        q = qmul(axis_angle(axis, amp * math.sin(f / fc * 2 * math.pi + phase)), base)
        out.append(tuple(-c for c in q) if flip and f % 3 == 1 else q)
    return out


def idle_parts():
    fc = 40
    rot = [smooth_rot(fc, (0, 1, 0), 0.5, 0.0, REST_ROT[0], flip=True),     # ball joint, sign flips between frames
           smooth_rot(fc, (0.3, 1.0, 0.2), 0.7, 1.0, REST_ROT[1]),
           [axis_angle((0, 0, 1), 0.8 * (0.5 - 0.5 * math.cos(f / fc * 2 * math.pi))) for f in range(fc)],   # hinge
           axis_angle((0, 0, 1), 0.1234),                                        # static, not the rest pose
           REST_ROT[4]]                                                          # equals the rest pose: mode 0
    trans = [[(0.01 * math.sin(f / fc * 6.28), 0.0, 0.5 + 0.004 * f) for f in range(fc)],
             REST_TRANS[1], REST_TRANS[2], (0.26, 0.001, 0.0), REST_TRANS[4]]
    scale = [1.0, 1.0, 1.0, 1.0, [1.0 + 0.2 * math.sin(f / fc * 6.28) for f in range(fc)]]
    fi = [(0.0, 0.0)] * fc
    return uncompressed_parts(5, fc, rot, trans, scale, fi), (rot, trans, scale, fi)


def walk_parts():
    fc = 30
    rot = [smooth_rot(fc, (1, 0, 0), 0.3, 0.5, REST_ROT[0]), smooth_rot(fc, (0, 1, 0), 0.9, 0.0, REST_ROT[1]),
           [axis_angle((0, 0, 1), 0.9 + 0.7 * math.sin(f / fc * 6.28)) for f in range(fc)],
           [axis_angle((0, 1, 0), 0.5 * math.sin(f / fc * 12.56)) for f in range(fc)], REST_ROT[4]]
    trans = [[(0.0, 0.0, 0.5 + 0.02 * math.sin(f / fc * 12.56)) for f in range(fc)],
             REST_TRANS[1], REST_TRANS[2], REST_TRANS[3], REST_TRANS[4]]
    scale = [1.0] * 5
    fi = [(0.0186 + 0.0005 * math.sin(f), -0.002, 0.0, 0.01 * f / fc) for f in range(fc)]
    return uncompressed_parts(5, fc, rot, trans, scale, fi), (rot, trans, scale, fi)


def aim_parts():
    fc = 9
    rot = [smooth_rot(fc, (0, 0, 1), 0.3 * (n + 1), n) if n in (0, 3) else (0.0, 0.0, 0.0, 1.0) for n in range(5)]
    rot = [r if isinstance(r, list) else r for r in rot]
    trans = [REST_TRANS[n] for n in range(5)]
    scale = [1.0] * 5
    return uncompressed_parts(5, fc, rot, trans, scale), (rot, trans, scale, [])


def long_parts():
    fc = 300
    rng = random.Random(7)
    rot = [[axis_angle((0.2, 1.0, 0.1), 0.6 * math.sin(f / 40.0) + 0.02 * rng.uniform(-1, 1)) for f in range(fc)],
           REST_ROT[1], REST_ROT[2], REST_ROT[3], REST_ROT[4]]
    trans = [[(300.0 * f / fc, 0.5 * math.sin(f / 20.0), 0.5) for f in range(fc)],       # 300 wu range: float32 track
             REST_TRANS[1], REST_TRANS[2], REST_TRANS[3], REST_TRANS[4]]
    scale = [1.0] * 5
    return uncompressed_parts(5, fc, rot, trans, scale), (rot, trans, scale, [])


def comp_parts():
    fc = 60
    def curve(amp, axis, base):
        return [qmul(axis_angle(axis, amp * math.sin(f / 15.0)), base) for f in range(fc)]
    keyframes = [0, 3, 9, 20, 21, 33, 47, 59]
    rot_def = [REST_ROT[0], REST_ROT[1], axis_angle((0, 0, 1), 0.3), axis_angle((0, 0, 1), 0.1234), REST_ROT[4]]
    r1 = curve(0.6, (0, 1, 0), REST_ROT[1])
    r2 = [axis_angle((0, 0, 1), 0.3 + 0.5 * math.sin(f / 10.0)) for f in range(fc)]
    rot_keys = {1: [(f, r1[f]) for f in keyframes[1:]], 2: [(f, r2[f]) for f in keyframes[1:]], 3: []}
    rot_def[1] = r1[0]
    rot_def[2] = r2[0]
    trans_def = [(0.0, 0.0, 0.5)] + REST_TRANS[1:]
    trans_keys = {0: [(10, (0.1, 0.0, 0.52)), (30, (0.2, 0.0, 0.5)), (59, (0.3, 0.05, 0.48))]}
    fi = [(0.01, 0.0, 0.002)] * fc
    return compressed_parts(5, fc, rot_def, rot_keys, trans_def, trans_keys, fi), None


def device_parts():
    fc = 3
    trans = [[(0.0, 0.0, 0.0)] * 3, [(0.0, 0.0, 0.0), (0.0, 0.0, 0.5), (0.0, 0.0, 1.0)], (0.0, 0.0, 0.0)]
    rot = [(0.0, 0.0, 0.0, 1.0)] * 3
    return uncompressed_parts(3, fc, rot, trans, [1.0] * 3), None


def build_world(path: Path):
    def anim(name, typ, parts, nn, fc, loop=0, checksum=None):
        d = dict(parts[0])
        d.update(name=name, type=typ, nn=nn, fc=fc, loop=loop)
        return d
    idle, walk, aim, long_, comp = idle_parts(), walk_parts(), aim_parts(), long_parts(), comp_parts()
    biped = [anim("stand idle", 0, idle, 5, 40), anim("stand move-front", 0, walk, 5, 30),
             anim("stand aim-still", 1, aim, 5, 9), anim("stand long", 0, long_, 5, 300),
             anim("cine walk", 0, comp, 5, 60), anim("stand idle%1", 0, idle, 5, 40),
             anim("stand airborne", 0, walk, 5, 30, loop=10), anim("idle", 0, idle, 5, 40)]
    dev = [anim("device position", 1, device_parts(), 3, 3)]
    specs = [
        ("scnr", "levels/test/test", lambda a: bytes(0x40)),
        ("mode", "characters/test/test", lambda a: model_block(a, NODES, REST_ROT, REST_TRANS, CHECK)),
        ("antr", "characters/test/test", lambda a: antr_block(a, NODES, biped, CHECK)),
        ("bipd", "characters/test/test", lambda a: object_block(tag_id(1), tag_id(2))),
        ("mode", "levels/test/door", lambda a: model_block(a, ["a", "b", "c"], [(0, 0, 0, 1)] * 3, [(0, 0, 0)] * 3, 0xAAAA)),
        ("antr", "levels/test/door", lambda a: antr_block(a, [], dev, 0xAAAA)),
        ("mach", "levels/test/door", lambda a: object_block(tag_id(4), tag_id(5))),
        ("antr", "characters/test/orphan", lambda a: antr_block(a, [], [anim("t", 0, device_parts(), 3, 3)], 0xBBBB)),
    ]
    build_map(path, specs)
    return biped


# ---- tests ------------------------------------------------------------------------------------------------------------
def test_q48_roundtrip():
    rng = random.Random(1)
    for _ in range(500):
        q = ac.norm4(tuple(rng.uniform(-1, 1) for _ in range(4)))
        u = ac.unpack_q48(ac.pack_q48(q))
        assert all(abs(a / 2047 - b) <= 1 / 2047 for a, b in zip(u, q)), (u, q)
    # the documented bit layout, one component at a time
    assert ac.unpack_q48(0x0000_0000_0010) == (1, 0, 0, 0)
    assert ac.unpack_q48(0x0000_0100_0000) == (0, 1, 0, 0) and ac.unpack_q48(0x0000_0000_0100) == (16, 0, 0, 0)
    assert ac.unpack_q48(0x0000_0000_0001)[1] == 256 and ac.unpack_q48(0x0000_0001_0000) == (0, 0, 16, 0)
    assert ac.unpack_q48(0x0000_0000_0000 | 1 << 32) == (0, 0, 0, 1) and ac.unpack_q48(1 << 44) == (0, 0, 1, 0)
    assert ac.unpack_q48(0x7F << 36 | 0xF << 32) == (0, 0, 0, 2047)


def test_quantisers():
    rng = random.Random(2)
    worst = 0.0
    for i in range(4000):
        q = ac.norm4(tuple(rng.uniform(-1, 1) for _ in range(4)))
        if i < 8:
            q = tuple(1.0 if k == i % 4 else 0.0 for k in range(4))          # axis aligned: the dropped component is exact
            q = tuple(-c for c in q) if i >= 4 else q
        r = ac.unpack_q16(*ac.pack_q16(q))
        worst = max(worst, ac.qangle(q, r))
        assert abs(sum(c * c for c in r) - 1) < 1e-6
    assert worst < 0.01, worst
    for ax in range(3):
        q = [0.0] * 4
        q[ax], q[3] = -0.6, 0.8
        assert ac.planar_axis([tuple(q)]) == ax
        r = ac.unpack_planar(*ac.pack_planar(tuple(q), ax), ax)
        assert ac.qangle(r, tuple(q)) < 0.01
    assert ac.planar_axis([(0.5, 0.5, 0.5, 0.5)]) == -1
    for _ in range(200):
        q = ac.norm4(tuple(rng.uniform(-1, 1) for _ in range(4)))
        assert ac.qangle(ac.unpack_qs16(ac.pack_qs16(q)), q) < 0.005
    # reduction: a straight line needs two keys, a kink three
    line = [(f * 0.01, 0.0, 0.0) for f in range(20)]
    assert ac.reduce_keys(line, line, ac._dist3, ac.lerp3, 1e-6) == [0, 19]
    kink = [(abs(f - 10) * 0.01, 0.0, 0.0) for f in range(21)]
    assert ac.reduce_keys(kink, kink, ac._dist3, ac.lerp3, 1e-6) == [0, 10, 20]


def check_world(tmp: Path):
    mp = tmp / "world.map"
    biped = build_world(mp)
    t = ac.MapTags(mp)
    # the antr parser reads what the writer wrote
    gs = {e.path: ac.read_graph(t, e) for e in t.idx.entries if e.group == "antr"}
    g = gs["characters/test/test"]
    assert [n.name for n in g.nodes] == NODES and len(g.anims) == 8
    comp = g.anims[4]
    assert comp.flags & 1 and comp.name == "cine walk"
    # decode: uncompressed rotations equal the source within s16 quantisation, compressed keys expand linearly
    d = ac.decode(g.anims[0])
    rot, trans, scale, fi = idle_parts()[1]
    for n in range(5):
        for f in range(40):
            src = rot[n][f] if isinstance(rot[n], list) else rot[n]
            assert ac.qangle(d.rot[n][min(f, len(d.rot[n]) - 1)], ac.hamilton(src)) < 0.005   # Halo stores conjugates
    assert len(d.rot[3]) == 1 and len(d.rot[0]) == 40 and len(d.scale[4]) == 40
    dc = ac.decode(comp)
    assert [len(dc.rot[n]) for n in range(5)] == [1, 60, 60, 1, 1]      # node 3: animated without keys -> frame-0 value
    assert ac.qangle(dc.rot[3][0], ac.hamilton(axis_angle((0, 0, 1), 0.1234))) < 0.1
    assert abs(dc.trans[0][10][0] - 0.1) < 1e-6 and abs(dc.trans[0][20][0] - 0.15) < 1e-6 and abs(dc.trans[0][59][2] - 0.48) < 1e-6
    for n in range(5):
        for q in dc.rot[n]:
            assert abs(sum(c * c for c in q) - 1) < 1e-9

    # full conversion
    stats = ac.BuildStats()
    data = ac.build_section(mp, stats=stats)
    sec = ac.parse_section(data)
    assert len(sec.graphs) == 3 and len(sec.anims) == 10 and len(data) % 16 == 0
    ids = [x.tag_id for x in sec.graphs]
    assert ids == sorted(ids) == [tag_id(2), tag_id(5), tag_id(7)]
    gi = sec.graph_index(tag_id(2))
    gr = sec.graphs[gi]
    assert gr.node_count == 5 and gr.anim_count == 8 and gr.model_tag == tag_id(1)
    rest = sec.rest_pose(gi)
    for n in range(5):
        assert ac.qangle(rest.rot[n], ac.hamilton(REST_ROT[n])) < 0.05 and ac._dist3(rest.trans[n], REST_TRANS[n]) < 1e-6
    names = [a.name for a in sec.graph_anims(gi)]
    assert names[0] == "stand idle" and names[5] == "stand idle%1"
    flags = {a.name: a.flags for a in sec.graph_anims(gi)}
    assert flags["stand idle"] & ac.ANIM_LOOPS and flags["stand idle%1"] & ac.ANIM_LOOPS
    assert flags["stand move-front"] & ac.ANIM_LOOPS and flags["stand airborne"] & ac.ANIM_LOOPS
    assert not flags["stand aim-still"] & ac.ANIM_LOOPS and not flags["stand long"] & ac.ANIM_LOOPS
    assert sec.graph_anims(gi)[6].loop_frame == 10 and sec.graph_anims(gi)[0].loop_frame == 0
    assert all(a.fps == 30.0 for a in sec.anims)
    # the device graph has no node block and no model rest pose in common with the biped: node count from the animation
    dgi = sec.graph_index(tag_id(5))
    assert sec.graphs[dgi].node_count == 3 and sec.graphs[dgi].model_tag == tag_id(4)
    ogi = sec.graph_index(tag_id(7))
    assert sec.graphs[ogi].model_tag == 0 and sec.graphs[ogi].node_count == 3    # no model with checksum 0xBBBB
    # identical animations share their data
    an = sec.graph_anims(gi)
    assert an[0].data_off == an[5].data_off == an[7].data_off and an[0].root_off == an[5].root_off and stats.dedup_saved > 0
    assert an[0].data_off != an[1].data_off
    # channel modes: static equal to the rest pose -> graph default; static elsewhere -> static; animated tracks
    blob = sec.blob(an[0])
    modes = list(blob[:5])
    assert modes[0] & 3 == 2 and modes[1] & 3 == 2 and modes[2] & 3 == 2          # three animated rotations
    assert modes[3] & 3 == 1 and modes[4] & 3 == 0                                  # static, graph default
    assert modes[0] >> 2 & 3 == 2 and modes[1] >> 2 & 3 == 0 and modes[3] >> 2 & 3 == 1   # translations
    assert modes[4] >> 4 & 3 == 2 and modes[0] >> 4 & 3 == 0                       # scale: animated / default
    # the hinge track (node 2) uses the 4-byte axis keys with axis z; the ball joints the 6-byte ones
    tr = sec.tracks(gi, 0)
    assert tr[2][0][0] == "track" and len(tr[2][0][3]) >= 2 and tr[0][0][0] == "track"
    hinge = ac.encode_rot_track(idle_parts()[1][0][2], 40)[0]
    ball = ac.encode_rot_track(idle_parts()[1][0][1], 40)[0]
    hflags, bflags = struct.unpack_from("<H", hinge, 2)[0], struct.unpack_from("<H", ball, 2)[0]
    assert hflags & ac.T_AXIS and hflags >> 4 & 3 == 2 and not bflags & ac.T_AXIS
    # frame info: type 1 all zero -> none; walk has 4 components
    assert an[0].root_type == 0 and an[0].root_off == 0
    assert an[1].root_type == 3 and an[1].root_off % 16 == 0
    walk_fi = walk_parts()[1][3]
    for f in range(30):
        got = sec.root_motion(an[1], f)
        assert all(abs(got[c] - walk_fi[f][c]) < 1e-5 for c in range(4)), (f, got, walk_fi[f])
    assert sec.root_motion(an[1], 999) == sec.root_motion(an[1], 29)
    assert an[4].root_type == 2 and abs(sec.root_motion(an[4], 5)[0] - 0.01) < 1e-6
    assert sec.root_motion(an[2], 3) == (0.0, 0.0, 0.0, 0.0)

    # fidelity against the decoded Xbox frames: < 0.2 degrees, < 0.5 mm (quantisation and key reduction included)
    graphs = ac.load_graphs(t)
    err = ac.measure_errors(sec, graphs)
    assert err.rot_max < 0.2 and err.trans_max < 0.5 and err.scale_max < 2e-3, (err.rot_max, err.trans_max, err.scale_max)
    # sampling semantics
    s0 = sec.sample(gi, 0, 7.0)
    d0 = ac.decode(g.anims[0])
    assert len(s0) == 5 and ac.qangle(s0[0][0], d0.rot[0][7]) < 0.2 and ac.qangle(s0[1][0], ac.hamilton(rot[1][7])) < 0.2
    # fractional frames interpolate between neighbouring frames
    a, b, mid = sec.sample(gi, 1, 10.0), sec.sample(gi, 1, 11.0), sec.sample(gi, 1, 10.5)
    for n in range(5):
        assert ac.qangle(mid[n][0], ac.nlerp(a[n][0], b[n][0], 0.5)) < 0.2
    # non-looping clamps; looping wraps (sub-frame blends last -> first)
    last, beyond = sec.sample(gi, 3, 299.0), sec.sample(gi, 3, 5000.0)
    assert all(ac.qangle(last[n][0], beyond[n][0]) < 1e-4 for n in range(5))
    lo = sec.sample(gi, 1, 29.5)                  # looping walk: between frame 29 and frame 0
    f29, f0 = sec.sample(gi, 1, 29.0), sec.sample(gi, 1, 0.0)
    for n in range(5):
        assert ac.qangle(lo[n][0], ac.nlerp(f29[n][0], f0[n][0], 0.5)) < 1e-4
    wrapped = sec.sample(gi, 1, 30.0 + 7.0)
    assert all(ac.qangle(wrapped[n][0], sec.sample(gi, 1, 7.0)[n][0]) < 1e-4 for n in range(5))
    air = sec.sample(gi, 6, 30.0 + 12.0)          # loop frame 10: wraps to 10 + fmod(42 - 10, 20) = 22
    assert all(ac.qangle(air[n][0], sec.sample(gi, 6, 22.0)[n][0]) < 1e-4 for n in range(5))
    assert all(ac.qangle(sec.sample(gi, 2, -3.0)[n][0], sec.sample(gi, 2, 0.0)[n][0]) < 1e-4 for n in range(5))
    # a node beyond the animation's node count would get the rest pose (the device graph has 3 nodes everywhere)
    dev = sec.sample(dgi, 0, 1.0)
    assert abs(dev[1][1][2] - 0.5) < 1e-4 and abs(dev[0][1][2]) < 1e-9 and len(dev) == 3
    # rotations are unit and interpolate along the shortest path even when keys flip sign
    for fr in (0.0, 0.5, 1.5, 2.5, 3.0):
        for n, (rq, _t, _s) in enumerate(sec.sample(gi, 0, fr)):
            assert abs(sum(c * c for c in rq) - 1) < 1e-6
    # restricting the conversion
    only = ac.parse_section(ac.build_section(mp, [tag_id(5)]))
    assert [x.tag_id for x in only.graphs] == [tag_id(5)] and len(only.anims) == 1
    try:
        ac.build_section(mp, [tag_id(1)])           # a model, not an animation graph
    except ac.ConvertError:
        pass
    else:
        raise AssertionError("expected a ConvertError for a tag that is not model_animations")
    return mp, sec, graphs


def check_model_checks(tmp: Path):
    """Node names / counts / checksums of the models used with a graph are cross-checked."""
    mp = tmp / "bad.map"
    other = ["root", "hip", "KNEE", "foot", "gizmo"]
    specs = [
        ("scnr", "levels/test/test", lambda a: bytes(0x40)),
        ("mode", "m/a", lambda a: model_block(a, other, REST_ROT, REST_TRANS, CHECK)),
        ("antr", "m/a", lambda a: antr_block(a, NODES, [dict(idle_parts()[0], name="x", type=0, nn=5, fc=40)], CHECK)),
        ("bipd", "m/a", lambda a: object_block(tag_id(1), tag_id(2))),
        ("mode", "m/b", lambda a: model_block(a, NODES[:4], REST_ROT[:4], REST_TRANS[:4], CHECK + 1)),
        ("antr", "m/b", lambda a: antr_block(a, NODES, [dict(idle_parts()[0], name="x", type=0, nn=5, fc=40)], CHECK + 1)),
        ("bipd", "m/b", lambda a: object_block(tag_id(4), tag_id(5))),
        ("mode", "m/c", lambda a: model_block(a, NODES, REST_ROT, REST_TRANS, 77)),
        ("bipd", "m/c", lambda a: object_block(tag_id(7), tag_id(2))),
    ]
    build_map(mp, specs)
    problems = {g.path: g.problems for g in ac.load_graphs(mp)}
    assert any("node names differ" in p for p in problems["m/a"]), problems
    assert any("different node list checksum" in p for p in problems["m/a"]), problems
    assert any("4 nodes, graph 5" in p for p in problems["m/b"]), problems


def check_bad_sources(tmp: Path):
    parts = idle_parts()[0]
    a = ac.Anim("x", 0, 40, parts["frame_size"], 0, 0, 5, 0, -1, 0, parts["trans_flags"], parts["rot_flags"],
                parts["scale_flags"], b"", parts["default"], parts["frame_data"])
    assert ac.decode(a).fc == 40
    for bad in (dict(frame_data=parts["frame_data"][:-1]), dict(default_data=parts["default"][:-4]),
                dict(frame_size=parts["frame_size"] + 4), dict(node_count=0), dict(node_count=65), dict(frame_count=0),
                dict(frame_info_type=1)):
        try:
            ac.decode(dataclasses.replace(a, **bad))
        except ac.ConvertError:
            continue
        raise AssertionError(f"decode accepted {list(bad)}")
    cp = comp_parts()[0]
    c = ac.Anim("c", 0, 60, 0, 0, 0, 5, 0, -1, ac.F_COMPRESSED, cp["trans_flags"], cp["rot_flags"], 0, b"", b"",
                cp["frame_data"])
    assert ac.decode(c).fc == 60
    for cut in (0, 10, 43, 44, 60, len(cp["frame_data"]) - 1):
        try:
            ac.decode(dataclasses.replace(c, frame_data=cp["frame_data"][:cut]))
        except ac.ConvertError:
            continue
        raise AssertionError(f"compressed decode accepted {cut} bytes")
    # key frames out of order
    bad = bytearray(cp["frame_data"])
    h = struct.unpack_from("<11I", bad, 0)
    struct.pack_into("<H", bad, h[0], 40)                 # first key of channel 0 beyond the following ones
    try:
        ac.decode(dataclasses.replace(c, frame_data=bytes(bad)))
    except ac.ConvertError:
        pass
    else:
        raise AssertionError("unordered key frames accepted")
    # section parser
    for bad_sec in (b"", b"ANIM" + bytes(60), bytes(64)):
        try:
            ac.parse_section(bad_sec)
        except ac.ConvertError:
            pass
        else:
            raise AssertionError("parse_section accepted garbage")


def write_expectations(sec: ac.Section) -> bytes:
    """AEXP: the reference sampler's poses (the C reader must reproduce them)."""
    recs = []
    for gi, g in enumerate(sec.graphs):
        for ai, a in enumerate(sec.graph_anims(gi)):
            fc = a.frame_count
            frames = [0.0, 0.5, 1.25, fc / 2.0, fc - 1.5, fc - 1.0, fc - 0.5, float(fc), fc + 3.3, 2.0 * fc + 1.5,
                      1000.0, -2.0]
            for fr in frames:
                pose = sec.sample(gi, ai, f32(fr))
                rec = struct.pack("<IIf", gi, ai, fr)
                for rq, tq, sq in pose:
                    rec += struct.pack("<4f3ff", *rq, *tq, sq)
                recs.append(rec)
    return struct.pack("<I", len(recs)) + b"".join(recs)


def f32(v):
    return struct.unpack("<f", struct.pack("<f", v))[0]


def check_real_map():
    path = Path(__file__).resolve().parents[2] / "assets" / "decompressed" / "a10.map"
    if not path.exists():
        print("skip: assets/decompressed/a10.map not present")
        return
    t = ac.MapTags(path)
    paths = {e.path: e.tag_id for e in t.idx.entries if e.group == "antr"}
    want = [paths[p] for p in ("characters\\cyborg\\cyborg", "cinematics\\animations\\chief\\x30\\x30",
                               "levels\\a10\\devices\\doors\\door small\\door small", "vehicles\\warthog\\warthog")]
    stats = ac.BuildStats()
    graphs = ac.load_graphs(t, want)
    data = ac.build_section_from_graphs(graphs, stats)
    sec = ac.parse_section(data)
    assert len(sec.graphs) == 4
    for g in graphs:
        assert ac.verify_graph(g) == [], g.path
    cy = sec.graph_index(paths["characters\\cyborg\\cyborg"])
    assert sec.graphs[cy].node_count == 19 and sec.graphs[cy].model_tag != 0
    an = {a.name: i for i, a in enumerate(sec.graph_anims(cy))}
    idle = an["stand rifle idle"]
    first, last = sec.sample(cy, idle, 0.0), sec.sample(cy, idle, sec.graph_anims(cy)[idle].frame_count - 1.0)
    # a looping idle starts and ends near the same pose
    assert max(ac.qangle(a[0], b[0]) for a, b in zip(first, last)) < 10.0
    assert max(ac._dist3(a[1], b[1]) for a, b in zip(first, last)) * WU_MM < 30.0
    # translations are bone lengths: small, continuous
    walk = an["alert unarmed move-front"]
    wa = sec.graph_anims(cy)[walk]
    prev = None
    for f in range(wa.frame_count):
        pose = sec.sample(cy, walk, float(f))
        for n, (rq, tq, sq) in enumerate(pose):
            assert abs(sum(c * c for c in rq) - 1) < 1e-6 and max(abs(c) for c in tq) < 1.0 and abs(sq - 1.0) < 1e-6
        if prev:
            assert max(ac.qangle(a[0], b[0]) for a, b in zip(prev, pose)) < 60.0
        prev = pose
    # frame info: one stride per walk cycle
    stride = sum(sec.root_motion(wa, f)[0] for f in range(wa.frame_count))
    assert 0.3 < stride < 1.0, stride
    # the compressed idle of the cinematic equals the uncompressed idle of the biped (same motion)
    ch = sec.graph_index(paths["cinematics\\animations\\chief\\x30\\x30"])
    ci = {a.name: i for i, a in enumerate(sec.graph_anims(ch))}["stand rifle idle"]
    for f in (0, 10, 30, 58):
        a, b = sec.sample(cy, idle, float(f)), sec.sample(ch, ci, float(f))
        assert max(ac.qangle(x[0], y[0]) for x, y in zip(a, b)) < 0.8
    # doors: translation-only animation
    dg = sec.graph_index(paths["levels\\a10\\devices\\doors\\door small\\door small"])
    p0, p1 = sec.sample(dg, 0, 0.0), sec.sample(dg, 0, 2.0)
    assert max(ac._dist3(x[1], y[1]) for x, y in zip(p0, p1)) > 0.01
    err = ac.measure_errors(sec, graphs)
    assert err.rot_max < 0.2 and err.trans_max < 0.5, (err.rot_max, err.trans_max)
    print(f"real a10: {len(data) / 1024:.0f} KiB for 4 graphs, rot err max {err.rot_max:.3f} deg, trans {err.trans_max:.3f} mm")


def main() -> None:
    test_q48_roundtrip()
    test_quantisers()
    with tempfile.TemporaryDirectory() as td_:
        tmp = Path(td_)
        mp, sec, graphs = check_world(tmp)
        check_model_checks(tmp)
        check_bad_sources(tmp)
        # CLI: --info on the map and on the written pack
        out = tmp / "world.anim.hp2"
        log, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(log), contextlib.redirect_stderr(err):
            assert ac.main([str(mp), "-o", str(out), "--info", "--verify"]) == 0
            pk = hp2.parse(out.read_bytes())
            assert ac.parse_section(pk.read("ANIM")).anims[0].name == "stand idle"
            assert ac.main([str(out), "--info"]) == 0
            assert ac.main([str(mp), "--info", "--tags", "characters/test/orphan"]) == 0
        assert "error vs the Xbox frames" in log.getvalue() and "no model with this node list checksum" in err.getvalue()
        assert "3 graphs, 10 animations" in log.getvalue() and "1 graphs, 1 animations" in log.getvalue()
        if os.environ.get("ANIM_FIXTURE"):
            Path(os.environ["ANIM_FIXTURE"]).write_bytes(
                hp2.build("test", [("ANIM", ac.build_section(mp)), ("AEXP", write_expectations(sec))]))
    check_real_map()
    print("ok")


if __name__ == "__main__":
    main()
