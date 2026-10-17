#!/usr/bin/env python3
"""Convert the cinematic and camera data of an Xbox scenario into the PS2 ``CINE`` section, version 1
(docs/28-cine-section.md; reader engine/src/world/cinedef.c, runtime engine/src/sim/cine.c).

Everything below was read off the retail maps (all ten campaign maps for every layout, range and rule named here; the
checks live in ``collect`` / ``check_stream`` and in tools/tests/test_cinconv.py).  "verified" = checked against the data
(the cross-check is named), "unverified" = a convention the data does not contradict.  Offsets are inside the tag's own
data block, every pointer is an absolute tag-space address, a reflexive is ``u32 count, u32 address, u32 pad``, a tag
reference is 16 bytes ``char[4] group (byte-reversed), u32 path pointer, u32 path length, u32 tag id``.

scenario (``scnr``)
    +0x36C recorded animations (64 B)        +0x444 ai animation references (60 B)       +0x4B4 references (40 B)
    +0x4FC cutscene titles (96 B)            +0x584 ingame help text (ustr reference)    +0x594 hud messages (hmt reference)
    verified: every stride equals (next block - this block) / count where the next block follows directly (the recorded
    animation records end where the first event stream starts, the streams are packed 4-aligned one after the other).
    Not converted: +0x4E4 cutscene flags (92 B) and +0x4F0 cutscene camera points (104 B, workstream D); their layout, as
    far as the camera state machine needs it, is: +0x04 char[32] name, +0x28 f32 x y z, +0x34 f32 yaw pitch roll (radians),
    +0x40 f32 field of view (radians; 1.2217 = 70 degrees on the default points).
recorded animation (64 B): +0x00 char[32] name, +0x20 s8 version (4 on every one of the 349 recordings), +0x21 s8 raw
    animation data (0 on all), +0x22 s16 unit control data version (4 on 271, 3 on 71, 0 on 7), +0x24 s16 length in ticks
    (1..3524), +0x26 6 bytes unused, +0x2C data {u32 size, flags 0, file offset 0, pointer, id 0}.
    WHAT IT IS (derived, not documented anywhere): the stream is a recording of a unit's *controls* (the input of a player
    or an AI unit), not of positions.  The script commands ``recording_play`` / ``_and_delete`` / ``_and_hover`` /
    ``recording_kill`` / ``recording_time`` feed it to a unit; the unit's own physics and animations then produce the
    motion.  Evidence: the state is aim yaw / pitch, throttle (x forward, y left), control flags and a weapon index.
    Event stream, byte layout (reverse engineered; verified: all 349 recordings of the ten maps tokenise exactly to the
    end of their stream with the table below and the tick rule below holds for every one):
      header, size by version: version 0 = 64 B, version 3 = 70 B, version 4 = 72 B
        +0x00 u8 mode (0 or 3; the stream's only type 2 event sets it to 3 and occurs on 73 recordings, which are the ones
              whose header holds 0: 67 + 6)
        +0x01 u8 aux (always 0)   +0x02 u16 word2 (0 except one recording where it is 2)
        +0x04 s16 weapon index (-1 or 0)         +0x06 u16 control flags (0x0300 on 275 of 349, 0 on 67, 0x12 on 5, 0x8EE and 0x8E4 on one each)
        +0x08 f32 throttle x, y (always 0, 0)
        +0x10 three f32[3] unit vectors: facing, aiming, looking.  Together with the next block they are the same angles:
              verified, the vectors equal (cos p cos y, cos p sin y, sin p) of the quantised pairs below to 0.5 count
              on all 349.  aiming == looking on all 349; facing has the aiming yaw on 346 (three recordings differ by 12 to 303
              counts) and the aiming pitch on 274 (pitch 0 on 223).  The section keeps ONE yaw and ONE pitch (aiming = looking)
              and drops facing: a unit's body heading is its own physics, not an input (unverified; the facing's own
              dynamics cannot be told from the data).
        +0x34 version >= 3: u32 (uninitialised padding: stack addresses and tag-id-like words), s16 (-1, 0 or 1), and
              version 4 only: one more s16 (always -1).  Preserved as extra_a / extra_b, meaning unknown (guess: grenade
              index and zoom index).
        then six s16: facing yaw, facing pitch, aiming yaw, aiming pitch, looking yaw, looking pitch.  Angles are in
              counts of pi / 1000 radians (2000 counts = one turn): verified, a quantised yaw of -500 is the vector
              (0, -1, 0) and the integer yaw of every recording stays inside [-1000, 1000] over its whole length.
      events, until the end of the stream: one byte ``op`` = ``type << 2 | kind``, then, in this order:
        kind 2: u8 wait   kind 3: u16 wait  (kind 0: no wait, kind 1: wait 1 tick)   then the payload of ``type``:
          type  1 (04..07)  none                       a pure wait; the last event of every stream (310 of 349 use kind 2)
          type  2 (08..0B)  u8 mode
          type  4 (10..13)  u16 control flags          values seen: 0 1 2 0x100 0x140 0x200 0x300 0x400 0x600 0x800
                                                      0x801 0x900 0x2000 (the HEK flag names are in cinedef.h)
          type  5 (14..17)  u16 weapon index           (4 events, values 0 and 1)
          type  6 (18..1B)  f32 throttle x, f32 throttle y   values (0,0) (1,0) (-1,0) (0,1) (0,-1) (.707,-.707)
          type 14 (38..3B)  s8 d_yaw, s8 d_pitch       small aim change, 86504 events
          type 22 (58..5B)  s16 d_yaw, s16 d_pitch     large aim change (593 events; 409 have |d_yaw| above 1500: the yaw
                                                      crossing +-1000), always an exact difference of the wrapped values
        Other types do not occur.  Aim = plain integer addition: yaw never leaves [-1000, 1000] and pitch (integrated over
        all 86k changes) stays inside [-475, 490], i.e. inside +-pi/2 = +-500, on all 349 recordings: strong evidence that the
        deltas are what they are.
      tick rule (verified on all 349): every event applies its payload, THEN the stream waits ``wait`` ticks before the
        next event; the recording lasts ``1 + sum(waits)`` ticks and that equals the tag's length exactly (tick 0 .. length - 1).
        Whether the very first event belongs to tick 0 or tick 1 cannot be told from the data (+-1 tick), unverified.
      Semantics NOT verified (no ground truth in the data): the names of control flag bits and of the header bytes
        (``mode``, ``aux``, ``word2``, ``extra``), which of the three aim vectors the deltas drive (the converter's model:
        yaw shared by all, pitch for aiming / looking), the meaning of weapon index 0 / 1.
ai animation reference (60 B): +0x00 char[32] name, +0x20 animation graph reference (``antr``), 12 B unused.  verified:
    every reference resolves to an ``antr`` tag; the name is an animation of the graph (exact match) for 14 of the 23
    references of the ten maps, otherwise a label.
references (40 B): 24 B zero, +0x18 tag reference.  verified on all ten maps (910 entries): every entry resolves to a live
    tag of the group it names.  Groups seen: snd! 570, lsnd 186, antr 55, effe 33, scen 29, jpt! 8, vehi 8, bipd 8, actv 9,
    weap 2, mach 2.  They are the tags scripts name: sounds, animation graphs (``custom_animation``) and a few objects.
cutscene title (96 B): +0x04 char[32] name (the first 4 bytes are zero on every record; the same shift exists in cutscene
    flags and camera points), +0x28 s16 top, left, bottom, right (the text box on the 640 x 480 virtual screen: top 425 and
    right 597 on all 44 titles, bottom 444, 446 or 450, left 0 on 37, 50 on 6, 275 on one), +0x30 s16 string index into the
    scenario's ingame help text list, +0x34 u16 text style (1 on 37 of 44 titles, 0 on the others: HEK order plain 0, bold 1,
    italic 2, condense 3, underline 4, unverified), +0x36 u16 justification (always 0: left, unverified), +0x38 u32 unused
    (0), +0x3C ColorARGBInt text colour (white on 37, two dark tints on 7), +0x40 ColorARGBInt shadow colour (black, alpha 0
    on 28, opaque on 16), +0x44 f32 fade in, up, fade out times.
    verified: the string index is a valid index of the help list on all 44 titles of the ten maps; the times are whole
    numbers (0, 30, 60, 90, 120, 150, 180, 210) so they are TICKS (up to 7 seconds), not the seconds the tag editor calls
    them; the usual title is 30 / 120 / 30 ticks (32 of 44), seven have a zero fade in or out.
ingame help text (``ustr``, +0x584): the scenario's unicode_string_list; read with hudconv.Collector.ustr.  It carries the
    texts of the cutscene titles AND the texts ``display_scenario_help <n>`` shows (a short n indexes it).  The level's
    ``hud_message_text`` (+0x594) is NOT stored here: hudconv puts it into the HUD pack's STRS (``level_messages``), where
    ``hud_set_help_text`` / ``hud_set_objective_text`` find their message by name.
camera_track (``trak``): +0x00 u32 flags (0 on all 11 distinct tracks), +0x04 reflexive control points (60 B each: f32
    position x y z, f32 orientation x y z w, 32 B zero), 32 B unused: the tag block is 48 + 60 * n bytes (verified).  Ten of
    the 11 tracks have 9 control points whose orientation is a rotation about the unit's y axis in 22.5 degree steps from -90
    to +90; the eleventh (a fixed-position seat camera) has 6 points at one position with angles 90, 50, 0, -46.8, -80.2, -80.2.
    Orientations are unit length (norm 1.0000 on all) and the position's y is within 0.007 of zero (a path in the unit's x-z
    plane).  Halo stores the CONJUGATE of the Hamilton quaternion (docs/12): the section
    stores the conjugate, i.e. x y z negated.  Verified by geometry: point 0 is above the unit looking down (z > 0, the
    converted rotation is +90 degrees about y: forward = (0, 0, -1)), the last point is below it looking up, the middle point is
    behind it looking forward; a camera on the arc follows the player's look pitch.

Usage:
    python3 -I tools/halo2ps2/cinconv.py assets/decompressed/a10.map -o assets/hp2/a10.cin.hp2
    python3 -I tools/halo2ps2/cinconv.py --info assets/decompressed/a10.map | assets/hp2/a10.cin.hp2
    python3 -I tools/halo2ps2/cinconv.py --golden assets/hp2/a10.cin.hp2
"""
from __future__ import annotations

import argparse
import math
import struct
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from halo2ps2 import hp2, hudconv, scenconv, tagindex, texconv  # noqa: E402

MAGIC = b"CINE"
VERSION = 1
HEADER_SIZE = 128
REC_SIZE, TRACK_SIZE, POINT_SIZE, TITLE_SIZE, AREF_SIZE, STR_SIZE = 80, 32, 32, 64, 48, 8
NAME_LEN = 32
TRACK_NAME_LEN = 16
NONE16 = 0xFFFF
NONE32 = 0xFFFFFFFF
TICK = 30

# scenario offsets
SCN_RECORDINGS, SCN_AI_ANIMS, SCN_REFERENCES, SCN_TITLES, SCN_HELP = 0x36C, 0x444, 0x4B4, 0x4FC, 0x584
RECORDING_REC, AI_ANIM_REC, REFERENCE_REC, TITLE_REC_TAG = 64, 60, 40, 96
TRACK_POINT_TAG = 60

# recorded animation stream
YAW_WRAP = 2000                       # counts per turn (pi / 1000 radians each)
PITCH_LIMIT = 500                     # +- pi / 2
HEADER_SIZES = {0: 64, 3: 70, 4: 72}
PAYLOAD = {1: 0, 2: 1, 4: 2, 5: 2, 6: 8, 14: 2, 22: 4}      # payload bytes per event type
T_WAIT, T_MODE, T_FLAGS, T_WEAPON, T_THROTTLE, T_AIM_SMALL, T_AIM_LARGE = 1, 2, 4, 5, 6, 14, 22
MAX_EVENTS = 1 << 22


class CineError(ValueError):
    pass


# ---- data model -----------------------------------------------------------------------------------------------------
@dataclass
class Ctl:
    """Control state of a recorded unit at one tick (the Python twin of cinedef_ctl)."""
    mode: int = 0
    aux: int = 0
    word2: int = 0
    weapon: int = -1
    extra_a: int = 0
    extra_b: int = 0
    flags: int = 0
    throttle: tuple[float, float] = (0.0, 0.0)
    yaw: int = 0                         # counts of pi / 1000 rad, [-1000, 1000] (events wrap it into [-1000, 1000))
    pitch: int = 0                       # clamped to +-500


@dataclass
class Recording:
    name: str
    version: int
    raw: int
    ctl_version: int
    length: int
    init: Ctl
    stream: bytes                        # the event stream after the header, verbatim
    events: int = 0


@dataclass
class Track:
    tag_id: int
    name: str
    flags: int
    points: list[tuple[float, float, float, float, float, float, float]]   # x y z + quaternion x y z w (engine convention)


@dataclass
class Title:
    name: str
    top: int
    left: int
    bottom: int
    right: int
    string: int                          # index into the help list, NONE16 if out of range
    style: int
    justify: int
    colour: int                          # GS colour word (hudconv.argb_int)
    shadow: int
    fade_in: int                         # ticks
    up: int
    fade_out: int


@dataclass
class AnimRef:
    name: str
    graph: int
    resolved: int                        # 1 if `name` is an animation of the graph


@dataclass
class CineSet:
    name: str
    recordings: list[Recording] = field(default_factory=list)
    tracks: list[Track] = field(default_factory=list)
    titles: list[Title] = field(default_factory=list)
    strings: list[str] = field(default_factory=list)
    help_tag: int = 0
    anim_refs: list[AnimRef] = field(default_factory=list)
    graphs: list[int] = field(default_factory=list)
    sounds: list[int] = field(default_factory=list)
    reference_groups: dict[str, int] = field(default_factory=dict)
    message_count: int = 0                # level hud messages (not stored: counted for the report)
    problems: list[str] = field(default_factory=list)
    seconds: float = 0.0


# ---- recorded animation streams ---------------------------------------------------------------------------------------
def wrap_yaw(y: int) -> int:
    return (y + YAW_WRAP // 2) % YAW_WRAP - YAW_WRAP // 2


def clamp_pitch(p: int) -> int:
    return max(-PITCH_LIMIT, min(PITCH_LIMIT, p))


def decode_header(data: bytes, ctl_version: int) -> tuple[Ctl, int]:
    """(initial control state, header size) of a recorded animation's raw event stream."""
    if ctl_version not in HEADER_SIZES:
        raise CineError(f"unit control data version {ctl_version} is not 0, 3 or 4")
    size = HEADER_SIZES[ctl_version]
    if len(data) < size:
        raise CineError(f"event stream of {len(data)} bytes is shorter than the version {ctl_version} header ({size})")
    c = Ctl()
    c.mode, c.aux, c.word2 = data[0], data[1], struct.unpack_from("<H", data, 2)[0]
    c.weapon = struct.unpack_from("<h", data, 4)[0]
    c.flags = struct.unpack_from("<H", data, 6)[0]
    t0, t1 = struct.unpack_from("<2f", data, 8)
    if not (math.isfinite(t0) and math.isfinite(t1)):
        raise CineError("non-finite throttle in the header")
    c.throttle = (t0, t1)
    pairs = struct.unpack_from("<6h", data, size - 12)
    if not -YAW_WRAP // 2 <= pairs[2] <= YAW_WRAP // 2 or abs(pairs[3]) > PITCH_LIMIT:
        raise CineError(f"header aim ({pairs[2]}, {pairs[3]}) outside +-{YAW_WRAP // 2} / +-{PITCH_LIMIT} counts")
    c.yaw, c.pitch = pairs[2], pairs[3]
    if ctl_version >= 3:
        c.extra_a = struct.unpack_from("<h", data, 0x38)[0]
    if ctl_version >= 4:
        c.extra_b = struct.unpack_from("<h", data, 0x3A)[0]
    return c, size


def walk_stream(stream: bytes):
    """Yield (position, type, kind, wait, payload) per event; raises CineError on anything that is not the format."""
    pos, n, count = 0, len(stream), 0
    while pos < n:
        op = stream[pos]
        typ, kind = op >> 2, op & 3
        if typ not in PAYLOAD:
            raise CineError(f"event type {typ} at stream offset {pos}")
        p = pos + 1
        if kind == 0:
            wait = 0
        elif kind == 1:
            wait = 1
        elif kind == 2:
            if p + 1 > n:
                raise CineError("stream ends inside an event")
            wait = stream[p]
            p += 1
        else:
            if p + 2 > n:
                raise CineError("stream ends inside an event")
            wait = stream[p] | stream[p + 1] << 8
            p += 2
        end = p + PAYLOAD[typ]
        if end > n:
            raise CineError("stream ends inside an event")
        count += 1
        if count > MAX_EVENTS:
            raise CineError("too many events")
        yield pos, typ, kind, wait, stream[p:end]
        pos = end


def check_stream(stream: bytes, length: int) -> int:
    """Validates a stream against the recording length (1 + sum of waits); returns the event count."""
    total = events = 0
    for _pos, typ, _k, wait, payload in walk_stream(stream):
        total += wait
        events += 1
        if typ == T_THROTTLE and not all(math.isfinite(v) for v in struct.unpack("<2f", payload)):
            raise CineError("non-finite throttle event")
    if total + 1 != length:
        raise CineError(f"stream lasts {total + 1} ticks, the recording says {length}")
    return events


def apply_event(c: Ctl, typ: int, payload: bytes) -> None:
    if typ == T_MODE:
        c.mode = payload[0]
    elif typ == T_FLAGS:
        c.flags = struct.unpack("<H", payload)[0]
    elif typ == T_WEAPON:
        c.weapon = struct.unpack("<h", payload)[0]
    elif typ == T_THROTTLE:
        c.throttle = struct.unpack("<2f", payload)
    elif typ == T_AIM_SMALL:
        dy, dp = struct.unpack("<2b", payload)
        c.yaw, c.pitch = wrap_yaw(c.yaw + dy), clamp_pitch(c.pitch + dp)
    elif typ == T_AIM_LARGE:
        dy, dp = struct.unpack("<2h", payload)
        c.yaw, c.pitch = wrap_yaw(c.yaw + dy), clamp_pitch(c.pitch + dp)


def play(init: Ctl, stream: bytes, length: int):
    """Reference playback: yields a copy of the control state at ticks 0 .. length - 1 (the C twin is cine_play)."""
    events = list(walk_stream(stream))
    c = Ctl(**{k: getattr(init, k) for k in Ctl.__dataclass_fields__})
    i, t_next = 0, 0
    for tick in range(length):
        while i < len(events) and t_next <= tick:
            _pos, typ, _kind, wait, payload = events[i]
            apply_event(c, typ, payload)
            t_next += wait
            i += 1
        yield Ctl(**{k: getattr(c, k) for k in Ctl.__dataclass_fields__})


def encode_event(typ: int, wait: int, payload: bytes = b"") -> bytes:
    """Writer twin of walk_stream (tests and synthetic fixtures): the shortest wait kind."""
    if typ not in PAYLOAD or len(payload) != PAYLOAD[typ]:
        raise CineError("bad event")
    if wait == 0:
        kind, w = 0, b""
    elif wait == 1:
        kind, w = 1, b""
    elif wait < 256:
        kind, w = 2, bytes([wait])
    elif wait < 65536:
        kind, w = 3, struct.pack("<H", wait)
    else:
        raise CineError("wait too long")
    return bytes([typ << 2 | kind]) + w + payload


def encode_header(c: Ctl, ctl_version: int) -> bytes:
    """Writer twin of decode_header (the unknown padding words are zero)."""
    out = bytearray(HEADER_SIZES[ctl_version])
    out[0], out[1] = c.mode, c.aux
    struct.pack_into("<H", out, 2, c.word2)
    struct.pack_into("<h", out, 4, c.weapon)
    struct.pack_into("<H", out, 6, c.flags)
    struct.pack_into("<2f", out, 8, *c.throttle)
    size = len(out)
    if ctl_version >= 3:
        struct.pack_into("<h", out, 0x38, c.extra_a)
    if ctl_version >= 4:
        struct.pack_into("<h", out, 0x3A, c.extra_b)
    pairs = (c.yaw, 0, c.yaw, c.pitch, c.yaw, c.pitch)
    struct.pack_into("<6h", out, size - 12, *pairs)
    for k in range(3):
        yaw, pitch = pairs[2 * k] * math.pi / 1000.0, pairs[2 * k + 1] * math.pi / 1000.0
        struct.pack_into("<3f", out, 16 + 12 * k, math.cos(pitch) * math.cos(yaw), math.cos(pitch) * math.sin(yaw), math.sin(pitch))
    return bytes(out)


def state_hash(states) -> int:
    """FNV-1a over the per tick state (what the C test recomputes: flags, weapon, mode, yaw, pitch, throttle bit patterns)."""
    h = 0x811C9DC5
    for c in states:
        for b in struct.pack("<HhBhh2f", c.flags, c.weapon, c.mode, c.yaw, c.pitch, *c.throttle):
            h = ((h ^ b) * 0x01000193) & 0xFFFFFFFF
    return h


# ---- tag region access -----------------------------------------------------------------------------------------------
def _cstr(raw: bytes) -> str:
    return raw.split(b"\0", 1)[0].decode("latin1")


def _read_recordings(r: scenconv.Region, sb: int, problems: list[str]) -> list[Recording]:
    n, o = r.reflexive(sb + SCN_RECORDINGS, RECORDING_REC, 4096)
    out = []
    for i in range(n):
        q = o + i * RECORDING_REC
        name = r.cstr(q, NAME_LEN)
        ver, raw, ctl_ver, length = r.u("<bbhh", q + 0x20)
        size, _fl, _fo, ptr = r.u("<4I", q + 0x2C)
        po = ptr - r.base
        if po < 0 or po + size > len(r.data) or size == 0:
            raise CineError(f"recording {i}: event stream {size} bytes at {ptr:#x} outside the tag region")
        data = r.data[po:po + size]
        try:
            init, hsize = decode_header(data, ctl_ver)
            stream = data[hsize:]
            events = check_stream(stream, length)
        except CineError as ex:
            raise CineError(f"recording {i}: {ex}") from None
        if ver != 4 or raw != 0:
            problems.append(f"recording {i}: version {ver} / raw {raw} (expected 4 / 0)")
        out.append(Recording(name, ver & 0xFF, raw & 0xFF, ctl_ver, length, init, stream, events))
    return out


def _read_tracks(r: scenconv.Region, m: texconv.MapData, problems: list[str]) -> list[Track]:
    out = []
    for e in sorted((e for e in m.index.entries if e.group == "trak"), key=lambda e: e.tag_id):
        b = r.block(e)
        flags = r.u("<I", b)[0]
        n, o = r.reflexive(b + 4, TRACK_POINT_TAG, 4096)
        pts = []
        for i in range(n):
            x, y, z, qx, qy, qz, qw = r.u("<7f", o + i * TRACK_POINT_TAG)
            if not all(map(math.isfinite, (x, y, z, qx, qy, qz, qw))):
                raise CineError(f"{e.path}: point {i} is not finite")
            norm = math.sqrt(qx * qx + qy * qy + qz * qz + qw * qw)
            if abs(norm - 1.0) > 0.01:
                problems.append(f"{e.path}: point {i} orientation has length {norm:.3f}")
            pts.append((x, y, z, -qx, -qy, -qz, qw))                       # conjugate: the engine's convention
        out.append(Track(e.tag_id, e.path.rsplit("\\", 1)[-1], flags, pts))
    return out


def _read_titles(r: scenconv.Region, sb: int, nstrings: int, problems: list[str]) -> list[Title]:
    n, o = r.reflexive(sb + SCN_TITLES, TITLE_REC_TAG, 1024)
    out = []
    for i in range(n):
        q = o + i * TITLE_REC_TAG
        name = r.cstr(q + 4, NAME_LEN)
        top, left, bottom, right = r.u("<4h", q + 0x28)
        index, = r.u("<h", q + 0x30)
        style, just = r.u("<HH", q + 0x34)
        text = bytes(r.data[q + 0x3C:q + 0x40])
        shadow = bytes(r.data[q + 0x40:q + 0x44])
        times = r.u("<3f", q + 0x44)
        if not all(math.isfinite(t) and 0 <= t < 65536 for t in times):
            raise CineError(f"title {i}: bad times {times}")
        if any(t != int(t) for t in times):
            problems.append(f"title {i}: fade times {times} are not whole ticks")
        if not 0 <= index < nstrings:
            problems.append(f"title {i}: string index {index} outside the help list of {nstrings}")
            index = NONE16
        out.append(Title(name, top, left, bottom, right, index, style, just,
                         hudconv.argb_int(text), hudconv.argb_int(shadow), *[int(t + 0.5) for t in times]))
    return out


def _read_refs(r: scenconv.Region, m: texconv.MapData, sb: int, problems: list[str]) -> tuple[list[AnimRef], list[int], list[int], dict]:
    """(ai animation references, antr tag ids named by scripts or ai references, sound tag ids named by scripts, group counts)."""
    arefs: list[AnimRef] = []
    graphs: set[int] = set()
    n, o = r.reflexive(sb + SCN_AI_ANIMS, AI_ANIM_REC, 4096)
    for i in range(n):
        q = o + i * AI_ANIM_REC
        g, tid = r.tagref(q + 0x20)
        e = m.by_id.get(tid)
        if g != "antr" or e is None or e.group != "antr":
            problems.append(f"ai animation reference {i}: not an animation graph")
            continue
        name = r.cstr(q, NAME_LEN)
        resolved = int(name in scenconv.animation_names(r, r.block(e)))
        arefs.append(AnimRef(name, tid, resolved))
        graphs.add(tid)
    groups: dict[str, int] = {}
    sounds: set[int] = set()
    n, o = r.reflexive(sb + SCN_REFERENCES, REFERENCE_REC, 8192)
    for i in range(n):
        q = o + i * REFERENCE_REC
        g, tid = r.tagref(q + 0x18)
        e = m.by_id.get(tid)
        if g is None or e is None or e.group != g:
            problems.append(f"script reference {i}: does not resolve")
            continue
        groups[g] = groups.get(g, 0) + 1
        if g == "antr":
            graphs.add(tid)
        elif g in ("snd!", "lsnd"):
            sounds.add(tid)
    return arefs, sorted(graphs), sorted(sounds), groups


def collect_map(m: texconv.MapData, name: str) -> CineSet:
    t0 = time.time()
    r = scenconv.Region(m)
    cs = CineSet(name)
    sb = r.block(m.index.scenario)
    try:
        # strings first: the titles check their index against them
        hc = hudconv.Collector(m)
        g, tid = r.tagref(sb + SCN_HELP)
        e = m.by_id.get(tid)
        if g == "ustr" and e is not None and e.group == "ustr":
            try:
                cs.strings = hc.ustr(e)
                cs.help_tag = tid
            except hudconv.HudError as ex:
                cs.problems.append(f"ingame help text: {ex}")
        elif g and g.strip("\0") and tid != NONE32:
            cs.problems.append("ingame help text reference does not resolve to a ustr tag")
        mid = hc.r.ref(sb + 0x594, "hmt ")
        if mid:
            try:
                cs.message_count = len(hc.hmt(m.by_id[mid]))
            except hudconv.HudError as ex:
                cs.problems.append(f"hud messages: {ex}")
        cs.recordings = _read_recordings(r, sb, cs.problems)
        cs.tracks = _read_tracks(r, m, cs.problems)
        cs.titles = _read_titles(r, sb, len(cs.strings), cs.problems)
        cs.anim_refs, cs.graphs, cs.sounds, cs.reference_groups = _read_refs(r, m, sb, cs.problems)
    except scenconv.ScenError as ex:
        raise CineError(str(ex)) from None
    cs.seconds = time.time() - t0
    return cs


def collect(map_path: Path | str) -> CineSet:
    p = Path(map_path)
    return collect_map(texconv.MapData.open(p), p.stem)


# ---- roots for the object / sound packs --------------------------------------------------------------------------------
def referenced_anims(map_path) -> set[int]:
    """Tag ids of the ``model_animations`` graphs the scenario names for scripted objects: the ``antr`` entries of the
    script references (``custom_animation`` and friends) and of the ai animation references.  Recorded animations name no
    graph (they are control streams played through the unit's own graph)."""
    cs = map_path if isinstance(map_path, CineSet) else collect(map_path)
    return set(cs.graphs)


def referenced_sounds(map_path) -> set[int]:
    """Tag ids of the ``sound`` / ``sound_looping`` tags the scenario's script references name (dialogue, music and cinematic
    effects started by ``sound_impulse_start`` and friends).  A superset: the sound workstream decides what is streamed
    (class dialog / music, docs/29) and what is resident."""
    cs = map_path if isinstance(map_path, CineSet) else collect(map_path)
    return set(cs.sounds)


# ---- section writer ------------------------------------------------------------------------------------------------------
def _align(b: bytearray, n: int = 16) -> None:
    b.extend(bytes(-len(b) % n))


def _name(s: str, n: int) -> bytes:
    raw = s.encode("latin1")[:n - 1]
    return raw.ljust(n, b"\0")


def build(cs: CineSet) -> bytes:
    pool = bytearray(16)                                  # offset 0 is "no stream"
    seen: dict[bytes, int] = {}
    recs = bytearray()
    for rec in cs.recordings:
        off = seen.get(rec.stream)
        if off is None:
            _align(pool, 4)
            off = len(pool)
            seen[rec.stream] = off
            pool += rec.stream
        c = rec.init
        recs += struct.pack("<32sIIIBBBBHhhhBBHffhhII", _name(rec.name, NAME_LEN), off, len(rec.stream), rec.length, rec.version,
                            rec.raw, rec.ctl_version, 0, c.flags, c.weapon, c.extra_a, c.extra_b, c.mode, c.aux, c.word2,
                            c.throttle[0], c.throttle[1], c.yaw, c.pitch, rec.events, 0)
    _align(pool)
    tracks, points = bytearray(), bytearray()
    for t in cs.tracks:
        tracks += struct.pack("<IIII16s", t.tag_id, t.flags, len(points) // POINT_SIZE, len(t.points), _name(t.name, TRACK_NAME_LEN))
        for p in t.points:
            points += struct.pack("<7fI", *p, 0)
    titles = bytearray()
    for t in cs.titles:
        titles += struct.pack("<32s4hHHHHIIHHHH", _name(t.name, NAME_LEN), t.top, t.left, t.bottom, t.right, t.string, t.style,
                              t.justify, 0, t.colour, t.shadow, t.fade_in, t.up, t.fade_out, 0)
    chars = bytearray()
    strtab = bytearray()
    for s in cs.strings:
        raw = s.encode("utf-16le")
        strtab += struct.pack("<II", len(chars) // 2, len(raw) // 2)
        chars += raw + b"\0\0"
    arefs = b"".join(struct.pack("<32sII", _name(a.name, NAME_LEN), a.graph, a.resolved).ljust(AREF_SIZE, b"\0") for a in cs.anim_refs)
    graphs = struct.pack(f"<{len(cs.graphs)}I", *cs.graphs)

    body = bytearray()
    offs: dict[str, int] = {}
    cur = HEADER_SIZE
    for key, blob in (("recs", bytes(recs)), ("pool", bytes(pool)), ("tracks", bytes(tracks)), ("points", bytes(points)),
                      ("titles", bytes(titles)), ("strtab", bytes(strtab)), ("chars", bytes(chars)), ("arefs", arefs),
                      ("graphs", graphs)):
        pad = -cur % 16
        body += bytes(pad)
        cur += pad
        offs[key] = cur
        body += blob
        cur += len(blob)
    pad = -cur % 16
    body += bytes(pad)
    total = cur + pad
    head = struct.pack("<4sHH" + "I" * 20, MAGIC, VERSION, 0, total,
                       len(cs.recordings), offs["recs"], offs["pool"], len(pool),
                       len(cs.tracks), offs["tracks"], len(points) // POINT_SIZE, offs["points"],
                       len(cs.titles), offs["titles"],
                       len(cs.strings), offs["strtab"], len(chars) // 2, offs["chars"], cs.help_tag,
                       len(cs.anim_refs), offs["arefs"], len(cs.graphs), offs["graphs"])
    return bytes(head.ljust(HEADER_SIZE, b"\0") + body)


def build_section(map_path: Path | str) -> bytes:
    return build(collect(map_path))


def build_pack(cs: CineSet) -> bytes:
    return hp2.build(cs.name, [("CINE", build(cs))])


# ---- reference parser (the Python twin of engine/src/world/cinedef.c) ------------------------------------------------------
def _u(data: bytes, fmt: str, off: int):
    try:
        return struct.unpack_from(fmt, data, off)
    except struct.error:
        raise CineError(f"read of {fmt} at {off:#x} outside the section") from None


def parse_section(data: bytes) -> dict:
    """Strict reader: every table, index and stream is validated, like cinedef_open."""
    if len(data) < HEADER_SIZE:
        raise CineError("shorter than the header")
    if data[:4] != MAGIC or _u(data, "<H", 4)[0] != VERSION:
        raise CineError("bad magic or version")
    (total, nrec, orec, opool, npool, ntrk, otrk, npt, opt, ntit, otit, nstr, ostr, nch, och, help_tag, nar, oar, ngr,
     ogr) = _u(data, "<20I", 8)
    if total != len(data):
        raise CineError("size field differs from the data")

    def table(off: int, count: int, rec: int, what: str) -> None:
        if off % 16 or off < HEADER_SIZE or off > len(data) or count * rec > len(data) - off:
            raise CineError(f"{what} table outside the section")

    for off, count, rec, what in ((orec, nrec, REC_SIZE, "recording"), (otrk, ntrk, TRACK_SIZE, "track"),
                                  (opt, npt, POINT_SIZE, "point"), (otit, ntit, TITLE_SIZE, "title"),
                                  (ostr, nstr, STR_SIZE, "string"), (oar, nar, AREF_SIZE, "animation reference"),
                                  (ogr, ngr, 4, "graph")):
        table(off, count, rec, what)
    table(opool, 1, npool, "stream pool")
    table(och, 1, nch * 2, "character")
    out: dict = {"recs": [], "tracks": [], "titles": [], "strings": [], "arefs": [], "graphs": [], "help_tag": help_tag}
    for i in range(nrec):
        q = orec + i * REC_SIZE
        name = _cstr(data[q:q + NAME_LEN])
        if data[q + NAME_LEN - 1] != 0:
            raise CineError("recording name not terminated")
        soff, sbytes, length, ver, raw, cver, _p = _u(data, "<IIIBBBB", q + 0x20)
        flags, weapon, ea, eb, mode, aux, word2, t0, t1, yaw, pitch, events, _p2 = _u(data, "<HhhhBBHffhhII", q + 0x30)
        if soff % 4 or soff + sbytes > npool or sbytes == 0 or cver not in HEADER_SIZES:
            raise CineError(f"recording {i}: bad stream range or version")
        if not (math.isfinite(t0) and math.isfinite(t1)) or not -1000 <= yaw <= 1000 or not -PITCH_LIMIT <= pitch <= PITCH_LIMIT:
            raise CineError(f"recording {i}: bad initial state")
        stream = data[opool + soff:opool + soff + sbytes]
        n = check_stream(stream, length)
        if n != events:
            raise CineError(f"recording {i}: event count {events} differs from the stream's {n}")
        out["recs"].append({"name": name, "length": length, "version": ver, "raw": raw, "ctl_version": cver, "stream": stream,
                            "init": Ctl(mode, aux, word2, weapon, ea, eb, flags, (t0, t1), yaw, pitch)})
    for i in range(ntrk):
        q = otrk + i * TRACK_SIZE
        tag, flags, first, cnt = _u(data, "<IIII", q)
        if first + cnt > npt or cnt < 2:
            raise CineError(f"track {i}: points {first}+{cnt} of {npt}")
        pts = [_u(data, "<7f", opt + (first + k) * POINT_SIZE) for k in range(cnt)]
        if not all(math.isfinite(v) for p in pts for v in p):
            raise CineError(f"track {i}: non-finite point")
        out["tracks"].append({"tag": tag, "flags": flags, "name": _cstr(data[q + 16:q + 32]), "points": pts})
    for i in range(nstr):
        first, ln = _u(data, "<II", ostr + i * STR_SIZE)
        if first + ln + 1 > nch:
            raise CineError(f"string {i} outside the character table")
        raw = data[och + first * 2:och + (first + ln) * 2 + 2]
        if raw[-2:] != b"\0\0":
            raise CineError(f"string {i} is not terminated")
        out["strings"].append(raw[:-2].decode("utf-16le", "replace"))
    for i in range(ntit):
        q = otit + i * TITLE_SIZE
        top, left, bottom, right, string, style, just, _p, colour, shadow, fi, up, fo, _p2 = _u(data, "<4hHHHHIIHHHH", q + 0x20)
        if string != NONE16 and string >= nstr:
            raise CineError(f"title {i}: string {string} of {nstr}")
        out["titles"].append({"name": _cstr(data[q:q + NAME_LEN]), "rect": (top, left, bottom, right), "string": string,
                              "style": style, "justify": just, "colour": colour, "shadow": shadow, "times": (fi, up, fo)})
    for i in range(nar):
        q = oar + i * AREF_SIZE
        graph, resolved = _u(data, "<II", q + 0x20)
        out["arefs"].append({"name": _cstr(data[q:q + NAME_LEN]), "graph": graph, "resolved": resolved})
    out["graphs"] = list(_u(data, f"<{ngr}I", ogr)) if ngr else []
    if out["graphs"] != sorted(set(out["graphs"])):
        raise CineError("graph list not sorted / unique")
    return out


# ---- driver --------------------------------------------------------------------------------------------------------------------
def describe(cs: CineSet, size: int | None = None) -> str:
    ticks = sum(r.length for r in cs.recordings)
    streams = {r.stream for r in cs.recordings}
    lines = [f"{cs.name}: {len(cs.recordings)} recorded animations ({ticks} ticks, {sum(r.events for r in cs.recordings)} events, "
             f"{sum(len(s) for s in streams)} stream bytes in {len(streams)} distinct streams), {len(cs.tracks)} camera tracks "
             f"({sum(len(t.points) for t in cs.tracks)} points), {len(cs.titles)} titles, {len(cs.strings)} help strings, "
             f"{len(cs.anim_refs)} animation references, {len(cs.graphs)} custom animation graphs, {len(cs.sounds)} script "
             f"sounds, {cs.message_count} hud messages (HUD pack)"]
    if size is not None:
        lines.append(f"  section {size} bytes, converted in {cs.seconds:.2f} s")
    versions: dict[int, int] = {}
    for r in cs.recordings:
        versions[r.ctl_version] = versions.get(r.ctl_version, 0) + 1
    lines.append("  control data versions: " + ", ".join(f"v{k} x {v}" for k, v in sorted(versions.items())))
    for p in cs.problems:
        lines.append(f"  problem: {p}")
    return "\n".join(lines)


def golden(pack_path: Path) -> list[str]:
    """One line per recording of a converted pack: ``<index> <length> <hash>``: the reference for test_cinedef / test_cine."""
    pk = hp2.parse(pack_path.read_bytes())
    sec = parse_section(pk.read("CINE"))
    out = []
    for i, r in enumerate(sec["recs"]):
        out.append(f"{i} {r['length']} {state_hash(play(r['init'], r['stream'], r['length'])):08x}")
    return out


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("input", nargs="?", type=Path, help="decompressed .map (or, with --info/--golden, a .cin.hp2)")
    ap.add_argument("-o", "--out", type=Path, help="output .hp2 (CINE section)")
    ap.add_argument("--info", action="store_true", help="print a summary instead of writing")
    ap.add_argument("--golden", action="store_true", help="input is a .cin.hp2: print per recording state hashes")
    args = ap.parse_args(argv)
    if args.input is None:
        ap.error("input required")
    if args.golden:
        print("\n".join(golden(args.input)))
        return 0
    if args.input.suffix == ".hp2":
        pk = hp2.parse(args.input.read_bytes())
        sec = parse_section(pk.read("CINE"))
        print(f"{pk.name}: {len(sec['recs'])} recordings, {len(sec['tracks'])} tracks, {len(sec['titles'])} titles, "
              f"{len(sec['strings'])} strings, {len(sec['arefs'])} animation references, {len(sec['graphs'])} graphs")
        return 0
    cs = collect(args.input)
    if args.info or not args.out:
        print(describe(cs))
        return 0
    blob = build_pack(cs)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_bytes(blob)
    print(describe(cs, len(build(cs))))
    print(f"wrote {args.out} ({len(blob)} bytes)")
    return 1 if cs.problems else 0


if __name__ == "__main__":
    sys.exit(main())
