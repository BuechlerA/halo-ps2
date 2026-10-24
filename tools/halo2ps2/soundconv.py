#!/usr/bin/env python3
"""Convert the gameplay sounds of a Xbox cache file into the PS2 ``SND0`` / ``SNDD`` sections, version 1
(docs/21-snd-section.md; the reader is engine/src/audio/snddef.c, its contract engine/src/audio/snddef.h).

Layouts below were checked against all ten retail campaign maps (a10 a30 a50 b30 b40 c10 c20 c40 d20 d40; every
snd! tag of every map passed the structural checks named, ``--check`` repeats them).  Offsets are inside the
tag's own data block, every pointer is an absolute tag-space address, a reflexive is ``u32 count, u32 address,
u32 pad``, a tag reference is 16 bytes ``char[4] group (byte-reversed), u32 path pointer, u32 path length (0 on
Xbox), u32 tag id``.  "verified" names the cross-check, "unverified" means a name or meaning taken from the public
tag documentation that the data is merely consistent with.

sound (``snd!``), struct 0xA4 bytes
    +0x00 u32 flags (0..3 seen: bit 0 "fit to ADPCM block size", bit 1 "split long sound into permutations")
    +0x04 u16 sound class      verified by tag path on every class that occurs: 0 projectile impact, 1 projectile
                               detonation, 4 weapon fire, 5 ready, 6 reload, 7 empty, 8 charge, 9 overheat, 10 idle,
                               13 object impacts, 14 particle impacts, 15 slow particle impacts, 18 unit footsteps,
                               19 unit dialog, 22 vehicle collision, 23 vehicle engine, 26 door, 27 force field,
                               28 machinery, 30 computers, 32 music, 33 ambient nature, 34 ambient machinery,
                               39 first person damage, 44 and 47 scripted dialog, 50 game event
    +0x06 u16 sample rate: 0 = 22050 Hz, 1 = 44100 Hz
    +0x08 f32 minimum distance, +0x0C f32 maximum distance (world units; 0 = default), +0x10 f32 skip fraction
    +0x14 f32 random pitch bounds lo, +0x18 f32 hi (1.0 on 1463 of the 1468 a10 sounds)
    +0x1C f32 inner cone angle, +0x20 f32 outer cone angle (both 2 pi = omnidirectional on all 11559 sounds),
    +0x24 f32 outer cone gain (1.0 on all)
    +0x28 f32 random gain modifier (0.85 on 6297 of the 11559 sounds of the ten maps, 1.0 on 3711, 0.5 on 1246,
          0.25 on 115, 0.75 on 47; 16 distinct values)                                           unverified name
    +0x2C f32 maximum bend per second (0 on 11535, 1.002 .. 1.006 on 24)                          unverified name
    +0x3C..+0x5C six f32 "scale modifiers": skip fraction, gain, pitch when the impact scale is 0 (+0x3C, +0x40,
          +0x44) and 1 (+0x54, +0x58, +0x5C). +0x3C and +0x54 are 1.0 on all sounds, +0x40 is 0.0 on 8839 and 1.0 on
          2676 (42 small values), +0x44 1.0 (two 0.7), +0x58 1.0 (four 0.6), +0x5C 1.0 on 11465 (up to 2.0)   unverified meaning
    +0x6C u16 channels (0 mono, 1 stereo)   +0x6E u16 format (1 = Xbox ADPCM on every sound of every map; PCM16 is
          handled but no retail sound uses it, so that path is unverified)
    +0x70 tag reference: promotion sound (group snd!), +0x80 u16 promotion count
    +0x84 u32 maximum play length in ms.  verified: equals floor(longest permutation bytes * 2 * 1000 /
          (rate * channels)) for 98 % of the sounds of a10 and c40 (the rest are mostly vehicle sounds); note the
          formula counts 2 samples per byte, the real decoded length is 64/72 of it (see the ADPCM section)
    +0x98 reflexive -> pitch_range[72 B], extent of the tag (up to the next tag's block) = 0xA4 + sub-blocks
pitch_range: +0x00 char[32] name ("default"), +0x20 f32 natural pitch, +0x24 f32 bend lo, +0x28 f32 bend hi,
    +0x2C u16 permutation count, +0x30 f32 (1.0 in the ranges inspected, unknown), +0x34/+0x38 u32 (0xFFFFFFFF, runtime slots),
    +0x3C reflexive -> permutation[124 B]
permutation: +0x00 char[32] name ("01", "loop1", "hollowmetalhit3"), +0x20 f32 skip fraction, +0x24 f32 gain,
    +0x28 u16 compression (1 = Xbox ADPCM, equal to the tag's format on every permutation), +0x2A u16 next
    permutation index (0xFFFF = none), +0x34 and +0x3C u32 tag id of the owning sound (verified: equals the sound's
    own id on all 7000+ permutations of a10), +0x40 u32 sample bytes, +0x44 u32 flags (always 0 = in the map file),
    +0x48 u32 sample offset in the map file.  +0x5C / +0x70 hold running byte totals (mouth/subtitle slots): ignored.
    verified on every permutation of all ten maps (11559 snd! tags): the sample range lies in the map file before the
    tag region, the size is a multiple of 36 (mono) or 72 (stereo), the owner id matches, flags are 0, next
    indices are in range, and no two permutations share a sample range.  Pitch ranges per sound: 1 (11545 tags),
    2 (2 tags) or 4 (12 tags).

Xbox ADPCM (IMA ADPCM with the standard step and index tables), verified on every permutation of the ten maps:
    mono block = 36 bytes: s16 predictor, u8 step index, u8 zero, 32 bytes of nibbles (low nibble first).
    A block yields **64 samples**: the header predictor is sample 0 (output as is) and nibbles 0..62 give samples
    1..63; the 64th nibble is always 0 (965/965 blocks of one sound) and unused.  verified: the step index in the
    next block's header equals the index reached after exactly 63 nibbles on 39,421,073 of 39,421,073 block
    boundaries of all 60,181 permutations of the ten maps (``index_continuity``; with 62 or 64 nibbles it is 4/241
    and 5/241 on one sound).  The header predictor of the next block is the *source* sample 64, so the decoded block
    is not continued across the boundary; our decode agrees with it (``boundary_ratio`` below).
    stereo block = 72 bytes (MS IMA stereo layout): L header (4), R header (4), then 8 groups of 4 bytes L / 4 bytes
    R (8 nibbles each).  verified: index continuity 100 % on 19,891,712 boundaries of the 11,440 stereo permutations
    (the "36 + 36 bytes" layout reaches 2-58 % on six stereo sounds).

sound_looping (``lsnd``), struct 0x54 bytes + tracks x 160 + details x 104 (verified: the tag extent equals this
sum on all 548 lsnd tags of the ten maps; 0 to 3 tracks, 0 or 1 detail sounds each)
    +0x00 u32 flags, +0x04..+0x18 six f32 (1.0), +0x1C u32 (0xFFFFFFFF), +0x20 f32 (3.0 or 5.0, unknown), +0x2C tag reference (group cdmg,
    continuous damage effect, null), +0x3C reflexive -> track[160 B], +0x48 reflexive -> detail sound[104 B]
track: +0x00 u32 flags, +0x04 f32 gain, +0x08 f32 fade in, +0x0C f32 fade out, +0x30 start sound, +0x40 loop sound,
    +0x50 end sound, +0x80 alternate loop sound, +0x90 alternate end sound (tag references, group snd!)
detail: +0x00 sound reference, +0x10 f32 random period lo, +0x14 f32 hi (seconds), +0x18 f32 gain, +0x1C u32 flags,
    +0x50 f32 yaw bounds lo / hi, +0x58 pitch bounds lo / hi (radians), +0x60 distance bounds lo / hi

Reachability (``--roots`` default): the tag index has no usage information, so references are found by scanning
the extent of a tag for 16-byte tag references and accepting a candidate only when group, tag id AND path pointer
all agree with the tag index entry (no false positive on any map: 0 path-pointer mismatches among the references
found).  The traversal starts at every weap proj jpt! effe bipd matg eqip unhi tag of the map (the last two carry the
pickup and shield / health feedback sounds) and follows references through those groups plus antr (footsteps and reload
sounds are animation frame events), lsnd, snd! (promotion sounds) and foot (material effects); other groups (models,
bitmaps, actors, dialogue, vehicles, devices, scenery) are not entered unless named with ``--roots``.

Conversion
    Xbox ADPCM -> PCM, stereo folded to mono ((l + r) >> 1), sources at 44100 Hz decimated by 2 (windowed-sinc FIR, 49
    taps, cutoff 0.92 of the output Nyquist, Blackman-Harris), re-encoded as SPU2 ADPCM (vag.py).  Looping sounds (used as the
    loop or alternate loop of an lsnd track) are resampled periodically to a multiple of 28 samples (shrunk by
    less than 28 samples) and their recorded rate lowered by the same ratio, so the SPU loop is seamless and keeps its
    pitch.  Dialogue and music (classes 19 32 44 45 46 47 48 and paths below sound\\dialog and sound\\music) are
    recorded as streamed (no data).  The budget (default 1.4 MiB) is met by a greedy value-per-byte policy (drop
    permutations, drop sounds, halve the rate of a sound to 11025 Hz), see ``plan_budget``.

Usage:
    python3 -I tools/halo2ps2/soundconv.py assets/decompressed/a10.map -o assets/hp2/a10.snd.hp2 [--roots 0xE1DF006B ...]
        [--budget BYTES] [--drops] [--json FILE] [--cache DIR] [--no-rate-degrade] [--weight 18=60 5=70]
    python3 -I tools/halo2ps2/soundconv.py --info assets/hp2/a10.snd.hp2
    python3 -I tools/halo2ps2/soundconv.py assets/decompressed/a10.map --check            # structure checks, all snd! tags
    python3 -I tools/halo2ps2/soundconv.py assets/decompressed/a10.map --survey 40         # decode + statistics, 40 sounds
    python3 -I tools/halo2ps2/soundconv.py assets/decompressed/a10.map --inspect "weapons\\\\pistol\\\\fire" --wav-dir /tmp/w [--converted]
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import operator
import re
import struct
import sys
import wave
from dataclasses import dataclass, field
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from halo2ps2 import hp2, tagindex, texconv, vag  # noqa: E402

SND_MAGIC = b"SND0"
VERSION = 1
HEADER_SIZE = 128
SOUND_REC = 128
RANGE_REC = 32
PERM_REC = 64
LOOP_REC = 64
TRACK_REC = 48
DETAIL_REC = 48
NAME_LEN = 32
RANGE_NAME_LEN = 16
PERM_NAME_LEN = 16
NO_TAG = 0xFFFFFFFF
NO_PERM = 0xFFFF
SPU_BUDGET = int(1.4 * 1024 * 1024) & ~63        # 1467968 bytes (1.4 MiB rounded down to the 64-byte SNDD padding)
SPU_ALIGN = 16

# sound.flags (u8 at +0x09 of the record)
F_LOOP, F_DROPPED, F_STREAMED, F_DEGRADED, F_UNSUPPORTED = 1, 2, 4, 8, 16
# permutation flags
P_LOOP, P_FOLDED = 1, 2

SOUND_STRIDE_SRC, RANGE_STRIDE_SRC, PERM_STRIDE_SRC = 0xA4, 72, 124
LSND_MAIN, TRACK_STRIDE_SRC, DETAIL_STRIDE_SRC = 0x54, 160, 104
SRC_RATES = {0: 22050, 1: 44100}
FORMAT_PCM16, FORMAT_XBOX_ADPCM = 0, 1

CLASS_NAMES = {0: "projectile_impact", 1: "projectile_detonation", 4: "weapon_fire", 5: "weapon_ready",
               6: "weapon_reload", 7: "weapon_empty", 8: "weapon_charge", 9: "weapon_overheat", 10: "weapon_idle",
               13: "object_impacts", 14: "particle_impacts", 15: "slow_particle_impacts", 18: "unit_footsteps",
               19: "unit_dialog", 22: "vehicle_collision", 23: "vehicle_engine", 26: "device_door",
               27: "device_force_field", 28: "device_machinery", 29: "device_nature", 30: "device_computers",
               32: "music", 33: "ambient_nature", 34: "ambient_machinery", 35: "ambient_computers",
               39: "first_person_damage", 44: "scripted_dialog_a", 45: "scripted_dialog_b", 46: "scripted_dialog_c",
               47: "scripted_dialog_d", 48: "scripted_dialog_e", 50: "game_event"}
STREAMED_CLASSES = {19, 32, 44, 45, 46, 47, 48}
STREAMED_PREFIXES = ("sound\\dialog\\", "sound\\music\\")
# budget weights: what silencing a sound of this class costs (relative); unlisted classes 10
CLASS_WEIGHT = {4: 100, 5: 100, 6: 100, 7: 100, 8: 100, 9: 100, 10: 100, 39: 90, 1: 80, 50: 60, 0: 45, 18: 40,
                22: 25, 23: 25, 13: 20, 14: 20, 15: 20, 46: 20, 26: 10, 27: 10, 28: 10, 29: 10, 30: 10,
                33: 8, 34: 8, 35: 8}
DEFAULT_WEIGHT = 10
CRITICAL_WEIGHT = 90             # classes at or above this (weapons, first person damage) are never normalised by population
DEGRADE_LOSS = 0.15              # fraction of a sound's weight lost by halving its sample rate
DEGRADE_LOSS_CRITICAL = 0.4      # ... for weapon sounds (weight >= 100)

ROOT_GROUPS = ("weap", "proj", "jpt!", "effe", "bipd", "matg", "eqip", "unhi")
TRAVERSE_GROUPS = frozenset(ROOT_GROUPS) | {"antr", "lsnd", "snd!", "foot"}
CACHE_VERSION = 3


class SoundError(ValueError):
    pass


# ---- tag region access ----------------------------------------------------------------------------
class Tags:
    """Bounds-checked reads of the tag region of a decompressed map (offsets are region offsets)."""

    def __init__(self, m: texconv.MapData):
        self.m = m
        self.data = m.region
        self.base = m.base
        ents = sorted((e for e in m.index.entries if e.data_address), key=lambda e: e.data_address)
        self.extent: dict[int, tuple[int, int]] = {}
        for a, b in zip(ents, ents[1:] + [None]):
            end = (b.data_address if b else self.base + len(self.data)) - self.base
            self.extent[a.tag_id] = (a.data_address - self.base, end)
        self._scan_cache: dict[int, list[tagindex.TagEntry]] = {}
        groups = {e.group for e in m.index.entries if e.group}
        self._gre = re.compile(b"(?=(?:" + b"|".join(re.escape(tagindex.fourcc_bytes(g)) for g in sorted(groups)) + b"))",
                               re.DOTALL)

    def u(self, fmt: str, off: int):
        if off < 0:
            raise SoundError(f"negative offset {off}")
        try:
            return struct.unpack_from(fmt, self.data, off)
        except struct.error:
            raise SoundError(f"read of {fmt} at {off:#x} outside the tag region") from None

    def reflexive(self, off: int, stride: int, within: tuple[int, int] | None = None, limit: int = 4096) -> tuple[int, int]:
        count, addr = self.u("<II", off)
        if count == 0:
            return 0, 0
        o = addr - self.base
        if count > limit or o < 0 or o + count * stride > len(self.data):
            raise SoundError(f"reflexive at {off:#x}: {count} x {stride} at {addr:#x} outside the tag region")
        if within and not (within[0] <= o and o + count * stride <= within[1]):
            raise SoundError(f"reflexive at {off:#x}: block leaves the tag's extent")
        return count, o

    def tagref(self, off: int) -> int:
        """Tag id of the reference at ``off`` (NO_TAG when null)."""
        raw = self.data[off:off + 4]
        (tid,) = self.u("<I", off + 12)
        if len(raw) < 4 or raw == tagindex.NULL_GROUP or tid == NO_TAG:
            return NO_TAG
        return tid

    def cstr(self, off: int, n: int) -> str:
        return self.data[off:off + n].split(b"\0", 1)[0].decode("latin1")

    def references(self, e: tagindex.TagEntry) -> list[tagindex.TagEntry]:
        """Tags referenced from the extent of ``e`` (see the module docstring for the acceptance rule)."""
        hit = self._scan_cache.get(e.tag_id)
        if hit is not None:
            return hit
        out: dict[int, tagindex.TagEntry] = {}
        ext = self.extent.get(e.tag_id)
        if ext:
            lo, hi = ext
            for mt in self._gre.finditer(self.data, lo, hi):
                o = mt.start()
                if (o - lo) % 4 or o + 16 > hi:
                    continue
                pp, _pl, tid = struct.unpack_from("<III", self.data, o + 4)
                t = self.m.by_id.get(tid)
                if t is None or pp != t.path_address or self.data[o:o + 4] != tagindex.fourcc_bytes(t.group):
                    continue
                out.setdefault(tid, t)
        res = list(out.values())
        self._scan_cache[e.tag_id] = res
        return res


# ---- source data model ------------------------------------------------------------------------------
@dataclass
class SrcPerm:
    index: int                    # position in the tag's flat permutation order (range by range)
    range_index: int
    name: str
    skip: float
    gain: float
    next_index: int               # within its pitch range, NO_PERM = none
    size: int                     # bytes in the map file
    file_offset: int
    compression: int


@dataclass
class SrcRange:
    name: str
    natural_pitch: float
    bend_lo: float
    bend_hi: float
    perms: list[SrcPerm] = field(default_factory=list)


@dataclass
class SrcSound:
    tag_id: int
    path: str
    flags: int
    klass: int
    rate: int
    min_dist: float
    max_dist: float
    skip: float
    pitch_lo: float
    pitch_hi: float
    inner_cone: float
    outer_cone: float
    outer_gain: float
    gain_mod: float
    scale_mods: tuple[float, ...]
    channels: int
    fmt: int
    promotion_tag: int
    promotion_count: int
    max_play_ms: int
    ranges: list[SrcRange]

    @property
    def perms(self) -> list[SrcPerm]:
        return [p for r in self.ranges for p in r.perms]

    @property
    def streamed(self) -> bool:
        return self.klass in STREAMED_CLASSES or self.path.lower().startswith(STREAMED_PREFIXES)

    @property
    def block_bytes(self) -> int:
        return 36 * self.channels

    def frames(self, perm: SrcPerm) -> int:
        """Decoded frames of a permutation at the source rate (64 per block and channel)."""
        if self.fmt == FORMAT_PCM16:
            return perm.size // (2 * self.channels)
        return perm.size // self.block_bytes * 64


@dataclass
class LoopTrack:
    flags: int
    gain: float
    fade_in: float
    fade_out: float
    start: int
    loop: int
    end: int
    alt_loop: int
    alt_end: int


@dataclass
class LoopDetail:
    sound: int
    period_lo: float
    period_hi: float
    gain: float
    flags: int
    yaw: tuple[float, float]
    pitch: tuple[float, float]
    dist: tuple[float, float]


@dataclass
class SrcLoop:
    tag_id: int
    path: str
    flags: int
    param: float
    tracks: list[LoopTrack]
    details: list[LoopDetail]

    def sound_ids(self) -> list[int]:
        ids = []
        for t in self.tracks:
            ids += [t.start, t.loop, t.end, t.alt_loop, t.alt_end]
        ids += [d.sound for d in self.details]
        return [i for i in ids if i != NO_TAG]

    def loop_ids(self) -> list[int]:
        return [i for t in self.tracks for i in (t.loop, t.alt_loop) if i != NO_TAG]


def read_sound(t: Tags, e: tagindex.TagEntry) -> SrcSound:
    """Parse and structurally validate one ``snd!`` tag. Raises SoundError on anything inconsistent."""
    off = e.data_address - t.base
    ext = t.extent[e.tag_id]
    if off + SOUND_STRIDE_SRC > ext[1]:
        raise SoundError(f"{e.path}: tag block shorter than a sound struct")
    flags, klass, rate_code = t.u("<IHH", off)
    if rate_code not in SRC_RATES:
        raise SoundError(f"{e.path}: sample rate code {rate_code}")
    mind, maxd, skip, plo, phi, inner, outer, ogain, gmod = t.u("<9f", off + 8)
    scale = t.u("<3f", off + 0x3C) + t.u("<3f", off + 0x54)
    chan_code, fmt = t.u("<HH", off + 0x6C)
    if chan_code > 1:
        raise SoundError(f"{e.path}: channel code {chan_code}")
    promo = t.tagref(off + 0x70)
    (pcount,) = t.u("<H", off + 0x80)
    (maxms,) = t.u("<I", off + 0x84)
    nr, ro = t.reflexive(off + 0x98, RANGE_STRIDE_SRC, ext)
    s = SrcSound(e.tag_id, e.path, flags, klass, SRC_RATES[rate_code], mind, maxd, skip, plo, phi, inner, outer, ogain,
                 gmod, scale, chan_code + 1, fmt, promo, pcount, maxms, [])
    flat = 0
    for i in range(nr):
        po = ro + i * RANGE_STRIDE_SRC
        name = t.cstr(po, 32)
        nat, blo, bhi = t.u("<3f", po + 0x20)
        npm, pmo = t.reflexive(po + 0x3C, PERM_STRIDE_SRC, ext)
        rng = SrcRange(name, nat, blo, bhi)
        for j in range(npm):
            qo = pmo + j * PERM_STRIDE_SRC
            pname = t.cstr(qo, 32)
            pskip, pgain = t.u("<2f", qo + 0x20)
            comp, nxt = t.u("<HH", qo + 0x28)
            sid1, sid2 = t.u("<2I", qo + 0x34)[0], t.u("<I", qo + 0x3C)[0]
            size, pflags, foff = t.u("<III", qo + 0x40)
            if sid1 != e.tag_id or sid2 != e.tag_id:
                raise SoundError(f"{e.path}/{pname}: owner id mismatch")
            if pflags != 0:
                raise SoundError(f"{e.path}/{pname}: sample flags {pflags:#x} (external data)")
            if nxt != NO_PERM and nxt >= npm:
                raise SoundError(f"{e.path}/{pname}: next permutation {nxt} of {npm}")
            if comp != fmt:
                raise SoundError(f"{e.path}/{pname}: compression {comp} differs from the tag format {fmt}")
            unit = 36 * s.channels if fmt == FORMAT_XBOX_ADPCM else 2 * s.channels
            if size == 0 or size % unit:
                raise SoundError(f"{e.path}/{pname}: {size} bytes is not a whole number of {unit}-byte units")
            if foff + size > t.m.header.tag_data_offset:
                raise SoundError(f"{e.path}/{pname}: sample range {foff:#x}+{size} outside the map's data area")
            rng.perms.append(SrcPerm(flat, i, pname, pskip, pgain, nxt, size, foff, comp))
            flat += 1
        s.ranges.append(rng)
    return s


def read_loop(t: Tags, e: tagindex.TagEntry) -> SrcLoop:
    off = e.data_address - t.base
    ext = t.extent[e.tag_id]
    flags = t.u("<I", off)[0]
    param = t.u("<f", off + 0x20)[0]
    nt, to = t.reflexive(off + 0x3C, TRACK_STRIDE_SRC, ext)
    nd, do = t.reflexive(off + 0x48, DETAIL_STRIDE_SRC, ext)
    if ext[1] - ext[0] != LSND_MAIN + nt * TRACK_STRIDE_SRC + nd * DETAIL_STRIDE_SRC:
        raise SoundError(f"{e.path}: extent {ext[1] - ext[0]} does not match {nt} tracks and {nd} details")
    tracks = []
    for i in range(nt):
        o = to + i * TRACK_STRIDE_SRC
        fl, gain, fin, fout = t.u("<Ifff", o)
        tracks.append(LoopTrack(fl, gain, fin, fout, t.tagref(o + 0x30), t.tagref(o + 0x40), t.tagref(o + 0x50),
                                t.tagref(o + 0x80), t.tagref(o + 0x90)))
    details = []
    for i in range(nd):
        o = do + i * DETAIL_STRIDE_SRC
        plo, phi, gain, dfl = t.u("<3fI", o + 0x10)
        yaw = t.u("<2f", o + 0x50)
        pitch = t.u("<2f", o + 0x58)
        dist = t.u("<2f", o + 0x60)
        details.append(LoopDetail(t.tagref(o), plo, phi, gain, dfl, yaw, pitch, dist))
    return SrcLoop(e.tag_id, e.path, flags, param, tracks, details)


# ---- reachability -----------------------------------------------------------------------------------
def closure(t: Tags, roots: list[tagindex.TagEntry]) -> dict[int, tagindex.TagEntry]:
    """Every tag reachable from ``roots`` (roots included), entering the roots and tags of TRAVERSE_GROUPS only."""
    seen: dict[int, tagindex.TagEntry] = {}
    stack = list(roots)
    explicit = {e.tag_id for e in roots}          # roots are always entered, whatever their group
    for e in stack:
        seen[e.tag_id] = e
    while stack:
        e = stack.pop()
        if (e.group not in TRAVERSE_GROUPS and e.tag_id not in explicit) or not e.data_address:
            continue
        for r in t.references(e):
            if r.tag_id not in seen:
                seen[r.tag_id] = r
                stack.append(r)
    return seen


UI_SOUND_PREFIX = "sound\\sfx\\ui\\"            # the main menu's clicks (cursor, forward, back, flag_failure) are played by the menu code, nothing in the tags references them


def default_roots(m: texconv.MapData) -> list[tagindex.TagEntry]:
    return [e for e in m.index.entries if (e.group in ROOT_GROUPS or (e.group == "snd!" and e.path.startswith(UI_SOUND_PREFIX))) and e.data_address]


def resolve_roots(m: texconv.MapData, specs: list[str]) -> list[tagindex.TagEntry]:
    """``--roots`` values: a tag id (0x... or decimal) or an exact tag path."""
    out = []
    for s in specs:
        e = None
        try:
            e = m.by_id.get(int(s, 0))
        except ValueError:
            e = next((x for x in m.index.entries if x.path == s or x.path.lower() == s.lower()), None)
        if e is None:
            raise SoundError(f"--roots: no tag {s!r} in {m.path.stem}")
        out.append(e)
    return out


def root_origin(t: Tags) -> dict[int, str]:
    """For reporting: the first root group (in ROOT_GROUPS order) whose closure holds each tag."""
    origin: dict[int, str] = {}
    for g in ROOT_GROUPS:
        roots = [e for e in t.m.index.entries if e.group == g and e.data_address]
        for tid in closure(t, roots):
            origin.setdefault(tid, g)
    return origin


# ---- Xbox ADPCM -------------------------------------------------------------------------------------
STEP_TABLE = (7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97,
              107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
              876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428,
              4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350,
              22385, 24623, 27086, 29794, 32767)
INDEX_ADJUST = (-1, -1, -1, -1, 2, 4, 6, 8)
XBOX_BLOCK_SAMPLES = 64


def _build_tables() -> tuple[list[int], list[int]]:
    diff, nxt = [0] * (89 * 16), [0] * (89 * 16)
    for idx in range(89):
        st = STEP_TABLE[idx]
        for nib in range(16):
            d = st >> 3
            if nib & 1:
                d += st >> 2
            if nib & 2:
                d += st >> 1
            if nib & 4:
                d += st
            diff[idx * 16 + nib] = -d if nib & 8 else d
            nxt[idx * 16 + nib] = max(0, min(88, idx + INDEX_ADJUST[nib & 7])) * 16
    return diff, nxt


_DIFF, _NEXT = _build_tables()


def _xbox_run(pred: int, idx: int, nibbles) -> tuple[list[int], int]:
    """Samples 0..len(nibbles) of a block (sample 0 = the header predictor) and the final index."""
    out = [pred]
    ap = out.append
    s16 = idx * 16
    diff, nxt = _DIFF, _NEXT
    for nib in nibbles:
        k = s16 + nib
        pred += diff[k]
        if pred > 32767:
            pred = 32767
        elif pred < -32768:
            pred = -32768
        s16 = nxt[k]
        ap(pred)
    return out, s16 // 16


def xbox_adpcm_decode(data: bytes, channels: int) -> list[list[int]]:
    """Decode Xbox ADPCM to one sample list per channel (64 samples per block). Raises SoundError on a bad block."""
    unit = 36 * channels
    if channels not in (1, 2) or not data or len(data) % unit:
        raise SoundError(f"Xbox ADPCM data of {len(data)} bytes for {channels} channel(s)")
    outs: list[list[int]] = [[] for _ in range(channels)]
    for b in range(0, len(data), unit):
        blk = data[b:b + unit]
        for ch in range(channels):
            pred, idx = struct.unpack_from("<hB", blk, ch * 4)
            if idx > 88:
                raise SoundError(f"step index {idx} in the block at byte {b}")
            if channels == 1:
                body = blk[4:36]
            else:
                body = b"".join(blk[8 + g * 8 + ch * 4:8 + g * 8 + ch * 4 + 4] for g in range(8))
            nibs = []
            for byte in body:
                nibs.append(byte & 15)
                nibs.append(byte >> 4)
            smp, _ = _xbox_run(pred, idx, nibs[:63])
            outs[ch] += smp
    return outs


def xbox_adpcm_encode(channels: list[list[int]]) -> bytes:
    """Reference encoder for the Xbox format (tests and self checks only): 64 samples per block and channel, the
    header predictor is the source sample 0, the step index carries over from the previous block."""
    nch = len(channels)
    n = len(channels[0])
    if n % 64 or any(len(c) != n for c in channels):
        raise SoundError("channels must have equal length, a multiple of 64")
    idxs = [0] * nch
    out = bytearray()
    for b in range(0, n, 64):
        parts = []
        for ch in range(nch):
            x = channels[ch][b:b + 64]
            pred, idx = x[0], idxs[ch]
            hdr = struct.pack("<hBB", pred, idx, 0)
            nibs = []
            for v in x[1:]:
                st = STEP_TABLE[idx]
                d = v - pred
                nib = 0
                if d < 0:
                    nib, d = 8, -d
                if d >= st:
                    nib |= 4
                    d -= st
                if d >= st >> 1:
                    nib |= 2
                    d -= st >> 1
                if d >= st >> 2:
                    nib |= 1
                k = idx * 16 + nib
                pred = max(-32768, min(32767, pred + _DIFF[k]))
                idx = _NEXT[k] // 16
                nibs.append(nib)
            nibs.append(0)
            idxs[ch] = idx
            body = bytes(nibs[i] | (nibs[i + 1] << 4) for i in range(0, 64, 2))
            parts.append((hdr, body))
        if nch == 1:
            out += parts[0][0] + parts[0][1]
        else:
            out += parts[0][0] + parts[1][0]
            for g in range(8):
                out += parts[0][1][g * 4:g * 4 + 4] + parts[1][1][g * 4:g * 4 + 4]
    return bytes(out)


def boundary_ratio(data: bytes, channels: int = 1) -> float:
    """Decode-fidelity probe independent of the waveform's character. The header predictor of block k + 1 is the *source*
    sample 64 (k + 1); our decode of block k ends at sample 63. If the decode were drifting (wrong tables, wrong nibble
    order) the two would disagree by far more than neighbouring samples do. Returns
    mean |header(k+1) - decoded(63 of block k)| / mean |decoded(63) - decoded(62)| over all block boundaries of channel 0
    (about 1 for a correct decode; 0.0 when the signal is constant)."""
    unit = 36 * channels
    outs = xbox_adpcm_decode(data, channels)[0]
    nb = len(data) // unit
    num = den = 0.0
    for k in range(nb - 1):
        hdr = struct.unpack_from("<h", data, (k + 1) * unit)[0]
        x63, x62 = outs[64 * k + 63], outs[64 * k + 62]
        num += abs(hdr - x63)
        den += abs(x63 - x62)
    return num / den if den else 0.0


def index_continuity(data: bytes, channels: int = 1) -> tuple[int, int]:
    """(matching, total) block boundaries where the step index in the next block's header equals the index reached after
    63 nibbles of this block, over every channel. 100 % is the signature of the nibble order, the 63-nibble rule and the
    index table (a wrong rule matches about 2 % of the boundaries)."""
    unit = 36 * channels
    ok = total = 0
    nxt = _NEXT
    for ch in range(channels):
        blocks = []
        for b in range(0, len(data) // unit * unit, unit):
            blk = data[b:b + unit]
            idx = blk[ch * 4 + 2]
            if channels == 1:
                body = blk[4:36]
            else:
                body = b"".join(blk[8 + g * 8 + ch * 4:8 + g * 8 + ch * 4 + 4] for g in range(8))
            blocks.append((idx, body))
        for k in range(len(blocks) - 1):
            s16 = min(blocks[k][0], 88) * 16
            n = 0
            for byte in blocks[k][1]:
                s16 = nxt[s16 + (byte & 15)]
                n += 1
                if n == 63:
                    break
                s16 = nxt[s16 + (byte >> 4)]
                n += 1
            total += 1
            ok += s16 // 16 == blocks[k + 1][0]
    return ok, total


def decode_source(s: SrcSound, data: bytes) -> list[list[int]]:
    """Source permutation bytes -> PCM channels (at the source rate)."""
    if s.fmt == FORMAT_XBOX_ADPCM:
        return xbox_adpcm_decode(data, s.channels)
    if s.fmt == FORMAT_PCM16:
        v = struct.unpack(f"<{len(data) // 2}h", data)
        return [list(v[c::s.channels]) for c in range(s.channels)]
    raise SoundError(f"{s.path}: unsupported format {s.fmt}")


def fold_mono(chans: list[list[int]]) -> list[int]:
    if len(chans) == 1:
        return chans[0]
    return [(a + b) >> 1 for a, b in zip(*chans)]


# ---- resampling -------------------------------------------------------------------------------------
def _blackman_harris(x: float) -> float:
    """Window value for x in [-1, 1]."""
    a = (x + 1.0) * math.pi
    return 0.35875 - 0.48829 * math.cos(a) + 0.14128 * math.cos(2 * a) - 0.01168 * math.cos(3 * a)


_FIR_CACHE: dict[int, list[float]] = {}


def _decimation_kernel(factor: int) -> list[float]:
    k = _FIR_CACHE.get(factor)
    if k is None:
        half = 12 * factor
        fc = 0.46 / factor                      # cycles per source sample (output Nyquist = 0.5 / factor)
        h = []
        for i in range(-half, half + 1):
            x = 2 * math.pi * fc * i
            h.append((2 * fc if i == 0 else math.sin(x) / (math.pi * i)) * _blackman_harris(i / (half + 1)))
        tot = sum(h)
        k = [v / tot for v in h]
        _FIR_CACHE[factor] = k
    return k


def decimate(x: list[int], factor: int, periodic: bool = False) -> list[int]:
    """Integer-factor sample rate reduction with a linear-phase low-pass; the output has ceil(n / factor) samples
    and no time shift. ``periodic``: the signal is treated as a loop (wraps around instead of fading to zero)."""
    if factor == 1:
        return list(x)
    n = len(x)
    h = _decimation_kernel(factor)
    half = len(h) // 2
    if periodic:
        pad_l = [x[(i - half) % n] for i in range(half)]
        pad_r = [x[i % n] for i in range(half + factor)]
    else:
        pad_l = [0] * half
        pad_r = [0] * (half + factor)
    xp = pad_l + list(x) + pad_r
    out = []
    mul = operator.mul
    width = len(h)
    for j in range((n + factor - 1) // factor):
        v = sum(map(mul, h, xp[j * factor:j * factor + width]))
        v = int(v + 0.5) if v >= 0 else -int(0.5 - v)
        out.append(32767 if v > 32767 else -32768 if v < -32768 else v)
    return out


_PHASES = 1024
_TAPS = 16                      # taps on each side of the interpolation point
_PTABLE: dict[float, list[list[float]]] = {}


def _shrink_table(c: float) -> list[list[float]]:
    key = round(c, 9)
    t = _PTABLE.get(key)
    if t is None:
        t = []
        for ph in range(_PHASES + 1):
            frac = ph / _PHASES
            row = []
            for k in range(-_TAPS + 1, _TAPS + 1):
                d = k - frac                     # distance from the interpolation point to tap k
                arg = math.pi * c * d
                s = c if d == 0 else c * math.sin(arg) / arg
                row.append(s * _blackman_harris(d / _TAPS))
            tot = sum(row)
            t.append([v / tot for v in row])
        _PTABLE[key] = t
    return t


def periodic_shrink(x: list[int], new_len: int) -> list[int]:
    """Resample a loop of len(x) samples to new_len <= len(x) samples, wrapping around (the seam stays continuous).
    The time axis is compressed by len(x) / new_len; the kernel low-passes at the new Nyquist."""
    n = len(x)
    if new_len == n:
        return list(x)
    if not 0 < new_len < n:
        raise SoundError("periodic_shrink only shrinks")
    c = new_len / n
    table = _shrink_table(c)
    xp = [x[(i - _TAPS + 1) % n] for i in range(_TAPS - 1)] + list(x) + [x[i % n] for i in range(_TAPS + 1)]
    mul = operator.mul
    out = []
    for j in range(new_len):
        pn = j * n
        i0, rem = divmod(pn, new_len)
        ph = (rem * _PHASES + new_len // 2) // new_len
        v = sum(map(mul, table[ph], xp[i0:i0 + 2 * _TAPS]))
        v = int(v + 0.5) if v >= 0 else -int(0.5 - v)
        out.append(32767 if v > 32767 else -32768 if v < -32768 else v)
    return out


def plan_frames(frames_in: int, factor: int, loop: bool) -> int:
    """Output frames of a permutation of ``frames_in`` source frames after decimating by ``factor``; loops are
    additionally shrunk to a multiple of 28 (when long enough)."""
    n = (frames_in + factor - 1) // factor
    if loop and n >= 2 * vag.BLOCK_SAMPLES:
        n = n // vag.BLOCK_SAMPLES * vag.BLOCK_SAMPLES
    return n


def convert_pcm(mono: list[int], factor: int, loop: bool) -> tuple[list[int], float]:
    """Decimate (and shrink loops); returns (samples ready for the encoder, rate multiplier <= 1: the playback rate
    of the result relative to ``source rate / factor``)."""
    pcm = decimate(mono, factor, periodic=loop)
    ratio = 1.0
    if loop and len(pcm) >= 2 * vag.BLOCK_SAMPLES:
        target = len(pcm) // vag.BLOCK_SAMPLES * vag.BLOCK_SAMPLES
        if target != len(pcm):
            ratio = target / len(pcm)
            pcm = periodic_shrink(pcm, target)
    return pcm, ratio


# ---- statistics -------------------------------------------------------------------------------------
def pcm_stats(x: list[int], rate: int) -> dict:
    """Plausibility numbers of a decoded waveform (see ``plausibility``)."""
    n = len(x)
    if n == 0:
        return {"frames": 0}
    energy = sum(v * v for v in x)
    rms = math.sqrt(energy / n)
    mean = sum(x) / n
    peak = max(max(x), -min(x))
    mags = sorted(abs(v) for v in x)

    def db(v: float) -> float:
        return 20 * math.log10(max(v, 0.5) / 32768.0)

    pct = {p: db(mags[min(n - 1, int(n * p / 100))]) for p in (50, 90, 99)}
    dif = sum((x[i + 1] - x[i]) ** 2 for i in range(n - 1))
    return {"frames": n, "seconds": n / rate, "peak": peak, "rms": rms, "mean": mean,
            "dc_ratio": abs(mean) / rms if rms else 0.0,
            "clip_fraction": sum(1 for v in x if v >= 32767 or v <= -32768) / n,
            "hf_ratio": dif / (2 * energy) if energy else 0.0,          # 1.0 for white noise, << 1 for natural sound
            "rms_db": db(rms), "peak_db": db(peak), "p50_db": pct[50], "p90_db": pct[90], "p99_db": pct[99],
            "silent": energy == 0}


def plausibility(st: dict) -> list[str]:
    """Reasons a decoded waveform looks wrong (empty = plausible): clipping, a real DC offset, a decode that drifts.
    A noise-like spectrum (``hf_ratio`` near 1) is *not* a failure by itself: static, sizzle and dry-fire clicks are
    noise; the block-boundary probe is what separates a broken decode from noisy material."""
    out = []
    if st.get("silent"):
        return out
    if st["clip_fraction"] > 0.01:
        out.append(f"clipping {st['clip_fraction'] * 100:.1f} % of the samples")
    if abs(st["mean"]) > 0.02 * 32768 and st["dc_ratio"] > 0.2:
        out.append(f"DC offset {st['mean']:.0f} ({st['dc_ratio'] * 100:.0f} % of the RMS)")
    if st.get("boundary", 1.0) > 3.0:
        out.append(f"decode drifts (block boundary ratio {st['boundary']:.1f})")
    return out


def write_wav(path: Path, channels: list[list[int]], rate: int) -> None:
    with wave.open(str(path), "wb") as w:
        w.setnchannels(len(channels))
        w.setsampwidth(2)
        w.setframerate(rate)
        inter = [v for frame in zip(*channels) for v in frame]
        w.writeframes(struct.pack(f"<{len(inter)}h", *inter))


# ---- audsrv sample wrapper -----------------------------------------------------------------------------
AUDSRV_HEADER = 16
AUDSRV_TERMINATOR = 16
AUDSRV_PITCH_BASE = 48000


def audsrv_pitch(rate: int) -> int:
    """The pitch word of audsrv's sample header: floor(rate * 4096 / 48000) (ps2sdk's adpenc writes 0x759 = 1881 for 22050 Hz;
    1881.6 rounded would be 1882, so it truncates)."""
    return rate * 4096 // AUDSRV_PITCH_BASE


def wrap_audsrv(blocks: bytes, frames: int, rate: int, loop: bool) -> bytes:
    """One permutation as audsrv_load_adpcm wants it (layout compared byte for byte with the output of ps2sdk's adpenc):
    16-byte header ``'APCM', u8 1, u8 1, u8 loop, u8 0, u32 pitch, u32 frames`` (little-endian), the ADPCM blocks, then a
    terminator block (predictor/shift byte of the last block, flags 7, zero nibbles).  Block flags: every block 0 except
    the first of a loop (6 = loop start) and the last: 1 for a one-shot, 3 (end + repeat: the jump to the loop start happens
    here, the terminator is never played) for a loop.  adpenc itself writes 0 on the last data block of an exact multiple
    of 28 samples and 1 otherwise, and 6 / 0 on the first block of a loop."""
    nb = len(blocks) // vag.BLOCK_BYTES
    if len(blocks) % vag.BLOCK_BYTES or nb == 0:
        raise SoundError("blocks must be a whole number of 16-byte blocks")
    out = bytearray(struct.pack("<4sBBBBII", b"APCM", 1, 1, 1 if loop else 0, 0, audsrv_pitch(rate), frames))
    body = bytearray(blocks)
    for i in range(nb):
        body[i * 16 + 1] = 0
    if loop:
        body[1] = 6
        body[(nb - 1) * 16 + 1] |= 3
        if nb == 1:
            body[1] = 7
    else:
        body[(nb - 1) * 16 + 1] = 1
    out += body
    out += bytes([body[(nb - 1) * 16], 7]) + bytes(14)
    return bytes(out)


def unwrap_audsrv(sample: bytes) -> tuple[dict, bytes]:
    """Inverse check of ``wrap_audsrv``: (header fields, data blocks without the terminator); raises SoundError if the
    sample does not have the audsrv layout."""
    if len(sample) < AUDSRV_HEADER + 2 * vag.BLOCK_BYTES or len(sample) % 16:
        raise SoundError("audsrv sample too short or not block aligned")
    magic, v1, v2, loop, z, pitch, frames = struct.unpack_from("<4sBBBBII", sample)
    if magic != b"APCM" or v1 != 1 or v2 != 1 or loop > 1 or z != 0:
        raise SoundError("bad audsrv header")
    blocks, term = sample[AUDSRV_HEADER:-AUDSRV_TERMINATOR], sample[-AUDSRV_TERMINATOR:]
    if term[1] != 7 or any(term[2:]):
        raise SoundError("bad terminator block")
    nb = len(blocks) // 16
    fl = [blocks[i * 16 + 1] for i in range(nb)]
    if loop:
        want = [0] * nb
        want[0] = 6
        want[-1] |= 3
        if nb == 1:
            want[0] = 7
    else:
        want = [0] * (nb - 1) + [1]
    if fl != want:
        raise SoundError("block flags do not match the loop flag")
    return {"loop": bool(loop), "pitch": pitch, "frames": frames}, blocks


# ---- budget planning --------------------------------------------------------------------------------
@dataclass
class PlanSound:
    snd: SrcSound
    loop: bool
    weight: float
    base_factor: int                     # decimation factor to reach <= 22050 Hz
    factor: int                          # current decimation factor (base, or 2 * base once degraded)
    kept: list[SrcPerm]
    dropped: list[SrcPerm] = field(default_factory=list)
    reason: str = ""
    _sizes: dict = field(default_factory=dict, repr=False)

    def perm_bytes(self, p: SrcPerm, factor: int | None = None) -> int:
        """SPU2 bytes of a permutation: the ADPCM blocks plus audsrv's terminator block (the 16-byte header stays in EE RAM)."""
        f = factor or self.factor
        v = self._sizes.get((p.index, f))
        if v is None:
            v = self._sizes[(p.index, f)] = vag.encoded_size(plan_frames(self.snd.frames(p), f, self.loop)) + AUDSRV_TERMINATOR
        return v

    def bytes(self) -> int:
        return sum(self.perm_bytes(p) for p in self.kept)


@dataclass
class Plan:
    sounds: list[PlanSound]
    streamed: list[SrcSound]
    unsupported: list[tuple[SrcSound, str]]
    budget: int
    total_before: int                    # bytes with every permutation at the base rate
    total: int                           # after budgeting

    @property
    def dropped_sounds(self) -> list[PlanSound]:
        return [p for p in self.sounds if not p.kept]

    @property
    def dropped_perms(self) -> int:
        return sum(len(p.dropped) for p in self.sounds if p.kept)

    @property
    def degraded(self) -> list[PlanSound]:
        return [p for p in self.sounds if p.kept and p.factor != p.base_factor]


def class_weight(klass: int) -> float:
    return float(CLASS_WEIGHT.get(klass, DEFAULT_WEIGHT))


def make_plan_sounds(sounds: list[SrcSound], loop_ids: set[int]) -> tuple[list[PlanSound], list[SrcSound], list[tuple[SrcSound, str]]]:
    plan, streamed, unsupported = [], [], []
    usable = []
    for s in sorted(sounds, key=lambda s: s.tag_id):
        if s.streamed:
            streamed.append(s)
        elif s.fmt not in (FORMAT_XBOX_ADPCM, FORMAT_PCM16):
            unsupported.append((s, f"format {s.fmt}"))
        elif not s.perms:
            unsupported.append((s, "no permutations"))
        else:
            usable.append(s)
    population: dict[int, int] = {}
    for s in usable:
        population[s.klass] = population.get(s.klass, 0) + 1
    for s in usable:
        base = max(1, s.rate // 22050)
        cw = class_weight(s.klass)
        w = cw if cw >= CRITICAL_WEIGHT else cw / math.sqrt(population[s.klass])
        plan.append(PlanSound(s, s.tag_id in loop_ids, w, base, base, list(s.perms)))
    return plan, streamed, unsupported


def _best_option(ps: PlanSound, degrade: bool):
    """The cheapest-per-byte way to shrink one sound: (loss per byte, tag id, kind, perm or None, bytes saved)."""
    if not ps.kept:
        return None
    n = len(ps.kept)
    big = max(ps.kept, key=lambda p: (ps.perm_bytes(p), p.index))
    saved = ps.perm_bytes(big)
    loss = ps.weight * (2.0 ** -n) if n >= 2 else ps.weight
    best = (loss / saved, ps.snd.tag_id, "perm", big, saved)
    if degrade and ps.factor == ps.base_factor and ps.snd.rate // (ps.factor * 2) >= 11025:
        dsaved = sum(ps.perm_bytes(p) - ps.perm_bytes(p, ps.factor * 2) for p in ps.kept)
        if dsaved > 0:
            dloss = ps.weight * (DEGRADE_LOSS_CRITICAL if class_weight(ps.snd.klass) >= 100 else DEGRADE_LOSS)
            cand = (dloss / dsaved, ps.snd.tag_id, "rate", None, dsaved)
            if cand[:2] < best[:2]:
                best = cand
    return best


def plan_budget(sounds: list[SrcSound], loop_ids: set[int], budget: int = SPU_BUDGET, degrade: bool = True) -> Plan:
    """Fit the resident set into ``budget`` bytes.

    Every step removes the option with the smallest *loss per byte saved*:
      * dropping a permutation of a sound that keeps n >= 2 of them loses weight * 2^-n (diminishing returns),
        and the largest permutation goes first;
      * dropping the last permutation of a sound loses its whole weight (the sound becomes silent);
      * halving the sample rate of a sound (all its permutations, once: 22050 -> 11025 Hz) loses 15 % of its weight
        (40 % for weapon sounds) and saves about half of its bytes.
    A sound's weight is its class weight divided by the square root of the number of gameplay sounds of that class
    (a class of 100 footstep variants is worth ten times a class of one sound, not a hundred times); the critical
    classes (weight >= 90: weapons, first person damage) keep their full weight on every sound.
    The class weight (CLASS_WEIGHT): weapons 100, first-person damage 90, projectile detonations 80, game events 60, projectile impacts 45,
    footsteps 40, vehicles 25, impacts 20, doors / machinery 10, ambience 8.  Ties break on the lower tag id.
    """
    plan, streamed, unsupported = make_plan_sounds(sounds, loop_ids)
    total_before = sum(p.bytes() for p in plan)
    total = total_before
    options = {ps.snd.tag_id: _best_option(ps, degrade) for ps in plan}
    by_id = {ps.snd.tag_id: ps for ps in plan}
    while total > budget:
        live = [o for o in options.values() if o is not None]
        if not live:
            break
        _, tid, kind, perm, saved = min(live, key=lambda o: o[:2])
        ps = by_id[tid]
        if kind == "perm":
            ps.kept.remove(perm)
            ps.dropped.append(perm)
            if not ps.kept:
                ps.reason = "dropped to meet the budget"
        else:
            ps.factor *= 2
        total -= saved
        options[tid] = _best_option(ps, degrade)
    return Plan(plan, streamed, unsupported, budget, total_before, total)


# ---- section writer ---------------------------------------------------------------------------------
def _name(s: str, n: int) -> bytes:
    raw = s.encode("latin1", "replace")
    if len(raw) > n - 1:
        raw = raw[-(n - 1):]
    return raw.ljust(n, b"\0")


@dataclass
class Built:
    snd0: bytes
    sndd: bytes
    plan: Plan
    stats: list[dict]                    # per encoded permutation: path, name, snr, frames, rate
    origin: dict = field(default_factory=dict)   # tag id -> first root group that reaches it (reporting only)
    spu_bytes: int = 0                   # SPU2 RAM the resident set takes: blocks + terminator blocks (SNDD also holds the 16-byte headers)


class Cache:
    def __init__(self, path: Path | None):
        self.path = path
        if path:
            path.mkdir(parents=True, exist_ok=True)

    def key(self, data: bytes, channels: int, src_rate: int, factor: int, loop: bool, fmt: int) -> str:
        h = hashlib.sha1(data)
        h.update(struct.pack("<6I", CACHE_VERSION, channels, src_rate, factor, int(loop), fmt))
        return h.hexdigest()

    def get(self, key: str):
        if not self.path:
            return None
        f = self.path / (key + ".snc")
        if not f.exists():
            return None
        raw = f.read_bytes()
        if len(raw) < 20 or raw[:4] != b"SNC1":
            return None
        frames, rate, snr10, = struct.unpack_from("<IIi", raw, 4)
        return raw[16:], frames, rate, snr10 / 10.0

    def put(self, key: str, data: bytes, frames: int, rate: int, snr: float) -> None:
        if self.path:
            snr10 = 999999 if math.isinf(snr) else int(round(snr * 10))
            (self.path / (key + ".snc")).write_bytes(b"SNC1" + struct.pack("<IIi", frames, rate, snr10) + data)


def encode_perm(s: SrcSound, data: bytes, factor: int, loop: bool, cache: Cache, exhaustive: bool = False):
    """Source bytes -> (SPU blocks, frames, rate Hz, SNR dB of the encoding against its own PCM input)."""
    key = cache.key(data, s.channels, s.rate, factor, loop, s.fmt)
    hit = cache.get(key)
    if hit:
        return hit
    mono = fold_mono(decode_source(s, data))
    pcm, ratio = convert_pcm(mono, factor, loop)
    blocks = vag.encode(pcm, loop=loop, exhaustive=exhaustive)
    snr = vag.snr_db(pcm, vag.decode(blocks)[:len(pcm)])
    rate = int(round(s.rate / factor * ratio))
    cache.put(key, blocks, len(pcm), rate, snr)
    return blocks, len(pcm), rate, snr


def build_sections(m: texconv.MapData, roots: list[tagindex.TagEntry] | None = None, budget: int = SPU_BUDGET,
                   degrade: bool = True, cache: Cache | None = None, exhaustive: bool = False,
                   progress=None) -> Built:
    """Build SND0 + SNDD for a map. ``roots`` default: every weap proj jpt! effe bipd matg tag (``--roots``)."""
    t = Tags(m)
    cache = cache or Cache(None)
    reach = closure(t, roots if roots is not None else default_roots(m))
    sounds, loops = [], []
    for tid in sorted(reach):
        e = reach[tid]
        if e.group == "snd!":
            sounds.append(read_sound(t, e))
        elif e.group == "lsnd":
            loops.append(read_loop(t, e))
    by_id = {s.tag_id: s for s in sounds}
    loop_ids = {i for lp in loops for i in lp.loop_ids() if i in by_id}
    budget &= ~63                       # SNDD is padded to 64 bytes: the padded size must still fit
    plan = plan_budget(sounds, loop_ids, budget, degrade)
    origin = root_origin(t) if roots is None else {}

    sndd = bytearray()
    out_sounds, out_ranges, out_perms, stats = [], [], [], []
    ps_by_id = {ps.snd.tag_id: ps for ps in plan.sounds}
    streamed_ids = {s.tag_id for s in plan.streamed}
    unsupported_ids = {s.tag_id for s, _ in plan.unsupported}
    done = 0
    spu_bytes = 0
    todo = sum(len(ps.kept) for ps in plan.sounds)
    with open(m.path, "rb") as f:
        for s in sorted(sounds, key=lambda s: s.tag_id):
            ps = ps_by_id.get(s.tag_id)
            flags = 0
            rec_range_first = len(out_ranges)
            rec_perm_first = len(out_perms)
            nperm = 0
            nranges = 0
            base_rate = 0
            if s.tag_id in streamed_ids:
                flags |= F_STREAMED
            elif s.tag_id in unsupported_ids:
                flags |= F_UNSUPPORTED
            elif ps is not None:
                if ps.loop:
                    flags |= F_LOOP
                base_rate = s.rate // ps.factor
                if not ps.kept:
                    flags |= F_DROPPED
                else:
                    if ps.factor != ps.base_factor:
                        flags |= F_DEGRADED
                    nranges = len(s.ranges)
                kept_idx = {p.index for p in ps.kept}
                for ri, rng in enumerate(s.ranges if ps.kept else []):
                    kept = [p for p in rng.perms if p.index in kept_idx]
                    new_pos = {p.index: k for k, p in enumerate(kept)}
                    first = nperm
                    for p in kept:
                        f.seek(p.file_offset)
                        data = f.read(p.size)
                        if len(data) != p.size:
                            raise SoundError(f"{s.path}/{p.name}: sample range outside the map file")
                        blocks, frames, rate, snr = encode_perm(s, data, ps.factor, ps.loop, cache, exhaustive)
                        if frames != plan_frames(s.frames(p), ps.factor, ps.loop) or len(blocks) + AUDSRV_TERMINATOR != ps.perm_bytes(p):
                            raise SoundError(f"{s.path}/{p.name}: planned {ps.perm_bytes(p)} bytes, encoded {len(blocks)}")
                        sample = wrap_audsrv(blocks, frames, rate, ps.loop)
                        off = len(sndd)
                        sndd += sample
                        sndd += bytes(-len(sndd) % SPU_ALIGN)
                        spu_bytes += len(blocks) + AUDSRV_TERMINATOR
                        nxt = NO_PERM
                        if p.next_index != NO_PERM:
                            nxt = new_pos.get(rng.perms[p.next_index].index, NO_PERM)
                        pf = (P_LOOP if ps.loop else 0) | (P_FOLDED if s.channels == 2 else 0)
                        out_perms.append(struct.pack(
                            "<6I2f4H16sII", off, len(sample), frames, rate, 0, len(blocks) if ps.loop else 0, p.gain,
                            p.skip, nxt, pf, ri, p.index, _name(p.name, PERM_NAME_LEN), p.size, 0))
                        stats.append({"sound": s.path, "perm": p.name, "snr": snr, "frames": frames, "rate": rate,
                                      "bytes": len(blocks), "loop": ps.loop})
                        nperm += 1
                        done += 1
                        if progress:
                            progress(done, todo)
                    out_ranges.append(struct.pack("<3fHH16s", rng.natural_pitch, rng.bend_lo, rng.bend_hi, first,
                                                  len(kept), _name(rng.name, RANGE_NAME_LEN)))
            dropped = min(len(ps.dropped), 0xFFFF) if ps is not None else 0
            out_sounds.append(struct.pack(
                "<IIBBHIHHIHHI9f6fI32s", s.tag_id, s.flags, s.klass, flags, base_rate, rec_range_first, nranges,
                nperm, rec_perm_first, dropped, min(s.promotion_count, 0xFFFF), s.promotion_tag,
                s.min_dist, s.max_dist, s.skip, s.pitch_lo, s.pitch_hi, s.inner_cone, s.outer_cone, s.outer_gain,
                s.gain_mod, *s.scale_mods, s.max_play_ms, _name(s.path, NAME_LEN)))
    sndd += bytes(-len(sndd) % 64)
    if spu_bytes > budget:
        raise SoundError(f"resident SPU2 set {spu_bytes} bytes exceeds the budget {budget}")

    # lsnd tables
    out_loops, out_tracks, out_details = [], [], []
    for lp in sorted(loops, key=lambda x: x.tag_id):
        tf, df = len(out_tracks), len(out_details)
        for tr in lp.tracks:
            out_tracks.append(struct.pack("<I3f5I12x", tr.flags, tr.gain, tr.fade_in, tr.fade_out, tr.start, tr.loop,
                                          tr.end, tr.alt_loop, tr.alt_end))
        for d in lp.details:
            out_details.append(struct.pack("<I3fI6f4x", d.sound, d.period_lo, d.period_hi, d.gain, d.flags, *d.yaw,
                                           *d.pitch, *d.dist))
        out_loops.append(struct.pack("<IIfHHII8x32s", lp.tag_id, lp.flags, lp.param, len(lp.tracks), len(lp.details), tf, df,
                                     _name(lp.path, NAME_LEN)))

    body = bytearray(HEADER_SIZE)
    offs = {}
    for name, recs in (("sounds", out_sounds), ("ranges", out_ranges), ("perms", out_perms), ("loops", out_loops),
                       ("tracks", out_tracks), ("details", out_details)):
        offs[name] = len(body)
        for r in recs:
            body += r
        body += bytes(-len(body) % 16)
    struct.pack_into("<4sHHI", body, 0, SND_MAGIC, VERSION, 0, len(body))
    struct.pack_into("<7I", body, 0x0C, len(out_sounds), len(out_ranges), len(out_perms), len(out_loops), len(out_tracks),
                     len(out_details), len(sndd))
    struct.pack_into("<6I", body, 0x28, offs["sounds"], offs["ranges"], offs["perms"], offs["loops"], offs["tracks"],
                     offs["details"])
    struct.pack_into("<5I", body, 0x40, budget, len(plan.dropped_sounds), plan.dropped_perms, len(plan.degraded),
                     len(plan.streamed))
    return Built(bytes(body), bytes(sndd), plan, stats, origin, spu_bytes)


for _fmt, _n in (("<IIBBHIHHIHHI9f6fI32s", SOUND_REC), ("<3fHH16s", RANGE_REC),
                 ("<6I2f4H16sII", PERM_REC), ("<IIfHHII8x32s", LOOP_REC), ("<I3f5I12x", TRACK_REC), ("<I3fI6f4x", DETAIL_REC)):
    assert struct.calcsize(_fmt) == _n, (_fmt, struct.calcsize(_fmt), _n)


# ---- reference reader -------------------------------------------------------------------------------
@dataclass
class Section:
    size: int
    sndd_size: int
    budget: int
    sounds: list[dict]
    ranges: list[dict]
    perms: list[dict]
    loops: list[dict]
    tracks: list[dict]
    details: list[dict]
    counts: dict


def parse_section(data: bytes, sndd_size: int | None = None) -> Section:
    """Reference reader (the C reader is engine/src/audio/snddef.c): same validation, raises SoundError."""
    if len(data) < HEADER_SIZE:
        raise SoundError("section shorter than the header")
    magic, version, _r, size = struct.unpack_from("<4sHHI", data)
    if magic != SND_MAGIC:
        raise SoundError("bad signature")
    if version != VERSION:
        raise SoundError(f"unsupported version {version}")
    if size != len(data):
        raise SoundError("inconsistent size")
    ns, nr, npm, nl, nt, nd, dsize = struct.unpack_from("<7I", data, 0x0C)
    o = struct.unpack_from("<6I", data, 0x28)
    budget, dsounds, dperms, ddeg, dstream = struct.unpack_from("<5I", data, 0x40)
    if sndd_size is not None and sndd_size != dsize:
        raise SoundError("SNDD size differs from the header")
    for off, cnt, rec in zip(o, (ns, nr, npm, nl, nt, nd), (SOUND_REC, RANGE_REC, PERM_REC, LOOP_REC, TRACK_REC, DETAIL_REC)):
        if off % 16 or off < HEADER_SIZE or off + cnt * rec > len(data):
            raise SoundError("table outside the section")
    sounds, ranges, perms, loops, tracks, details = [], [], [], [], [], []
    prev = -1
    known_sound = F_LOOP | F_DROPPED | F_STREAMED | F_DEGRADED | F_UNSUPPORTED
    for i in range(ns):
        v = struct.unpack_from("<IIBBHIHHIHHI9f6fI32s", data, o[0] + i * SOUND_REC)
        if v[0] <= prev:
            raise SoundError("sounds not strictly ascending")
        prev = v[0]
        if v[3] & ~known_sound or v[4] > 48000:
            raise SoundError("sound flags or rate")
        if any(not math.isfinite(x) for x in v[12:27]):
            raise SoundError("non-finite float in a sound")
        sounds.append(dict(tag_id=v[0], tag_flags=v[1], klass=v[2], flags=v[3], rate=v[4], range_first=v[5],
                           range_count=v[6], perm_count=v[7], perm_first=v[8], perms_dropped=v[9],
                           promotion_count=v[10], promotion_tag=v[11], min_dist=v[12], max_dist=v[13], skip=v[14],
                           pitch_lo=v[15], pitch_hi=v[16], inner_cone=v[17], outer_cone=v[18], outer_gain=v[19],
                           gain_mod=v[20], scale_mods=v[21:27], max_play_ms=v[27],
                           name=v[28].split(b"\0", 1)[0].decode("latin1")))
        if v[5] + v[6] > nr or v[8] + v[7] > npm or (v[6] == 0) != (v[7] == 0):
            raise SoundError("sound ranges/perms outside the tables")
        if v[7] and v[3] & (F_DROPPED | F_STREAMED | F_UNSUPPORTED):
            raise SoundError("flagged sound with data")
    for i in range(nr):
        v = struct.unpack_from("<3fHH16s", data, o[1] + i * RANGE_REC)
        if any(not math.isfinite(x) for x in v[:3]):
            raise SoundError("non-finite float in a range")
        ranges.append(dict(natural_pitch=v[0], bend_lo=v[1], bend_hi=v[2], perm_first=v[3], perm_count=v[4],
                           name=v[5].split(b"\0", 1)[0].decode("latin1")))
    for i in range(npm):
        v = struct.unpack_from("<6I2f4H16sII", data, o[2] + i * PERM_REC)
        off, sz, frames, rate, ls, le, gain, skip, nxt, pf, rng, src, name, srcb, _ = v
        if off % 16 or sz % 16 or sz < AUDSRV_HEADER + 2 * vag.BLOCK_BYTES or off + sz > dsize:
            raise SoundError("permutation outside SNDD")
        nblk = sz // 16 - 2                 # data blocks: the size covers the audsrv header and the terminator block
        if frames > nblk * 28 or frames <= (nblk - 1) * 28:
            raise SoundError("permutation frame count does not match its size")
        if not 0 < rate <= 48000 or pf & ~(P_LOOP | P_FOLDED):
            raise SoundError("permutation rate or flags")
        if pf & P_LOOP:
            if ls != 0 or le != nblk * 16:
                raise SoundError("loop bounds")
        elif ls or le:
            raise SoundError("loop bounds on a one-shot")
        if not (math.isfinite(gain) and math.isfinite(skip)):
            raise SoundError("non-finite float in a permutation")
        perms.append(dict(offset=off, size=sz, frames=frames, rate=rate, loop_start=ls, loop_end=le, gain=gain,
                          skip=skip, next=nxt, flags=pf, range=rng, src_index=src,
                          name=name.split(b"\0", 1)[0].decode("latin1"), src_bytes=srcb))
    for s in sounds:
        run = 0
        for k in range(s["range_count"]):
            r = ranges[s["range_first"] + k]
            if r["perm_first"] != run:
                raise SoundError("ranges do not partition the permutations in order")
            run += r["perm_count"]
        if run != s["perm_count"]:
            raise SoundError("ranges do not cover the permutations")
        for k in range(s["range_count"]):
            r = ranges[s["range_first"] + k]
            for j in range(r["perm_count"]):
                p = perms[s["perm_first"] + r["perm_first"] + j]
                if p["range"] != k or (p["next"] != NO_PERM and p["next"] >= r["perm_count"]):
                    raise SoundError("permutation range or next index")
    prev = -1
    for i in range(nl):
        v = struct.unpack_from("<IIfHHII8x32s", data, o[3] + i * LOOP_REC)
        if v[0] <= prev:
            raise SoundError("loops not strictly ascending")
        prev = v[0]
        if v[5] + v[3] > nt or v[6] + v[4] > nd or not math.isfinite(v[2]):
            raise SoundError("loop tracks/details outside the tables")
        loops.append(dict(tag_id=v[0], flags=v[1], param=v[2], track_count=v[3], detail_count=v[4], track_first=v[5],
                          detail_first=v[6], name=v[7].split(b"\0", 1)[0].decode("latin1")))
    for i in range(nt):
        v = struct.unpack_from("<I3f5I12x", data, o[4] + i * TRACK_REC)
        if any(not math.isfinite(x) for x in v[1:4]):
            raise SoundError("non-finite float in a track")
        tracks.append(dict(flags=v[0], gain=v[1], fade_in=v[2], fade_out=v[3], start=v[4], loop=v[5], end=v[6],
                           alt_loop=v[7], alt_end=v[8]))
    for i in range(nd):
        v = struct.unpack_from("<I3fI6f4x", data, o[5] + i * DETAIL_REC)
        if any(not math.isfinite(x) for x in v[1:4] + v[5:11]):
            raise SoundError("non-finite float in a detail sound")
        details.append(dict(sound=v[0], period_lo=v[1], period_hi=v[2], gain=v[3], flags=v[4], yaw=v[5:7],
                            pitch=v[7:9], dist=v[9:11]))
    return Section(size, dsize, budget, sounds, ranges, perms, loops, tracks, details,
                   dict(dropped_sounds=dsounds, dropped_perms=dperms, degraded=ddeg, streamed=dstream))


# ---- reports ----------------------------------------------------------------------------------------
def class_name(k: int) -> str:
    return CLASS_NAMES.get(k, f"class{k}")


def describe(b: Built, name: str, origin: dict[int, str] | None = None, drops: bool = False) -> list[str]:
    plan = b.plan
    L = [f"{name}: {len(plan.sounds)} gameplay sounds ({sum(len(p.snd.perms) for p in plan.sounds)} permutations), "
         f"{len(plan.streamed)} streamed (dialogue/music), {len(plan.unsupported)} unsupported"]
    mib = lambda n: f"{n / 2**20:.2f} MiB"  # noqa: E731
    L.append(f"  SPU2 set before budgeting {mib(plan.total_before)}, budget {mib(plan.budget)}, resident {mib(b.spu_bytes)} "
             f"({b.spu_bytes} bytes of blocks and terminators, {100 * b.spu_bytes / plan.budget:.1f} % of the budget; "
             f"SNDD {len(b.sndd)} bytes with the audsrv headers)")
    kept = [p for p in plan.sounds if p.kept]
    L.append(f"  kept {len(kept)} sounds with {sum(len(p.kept) for p in kept)} permutations; dropped "
             f"{len(plan.dropped_sounds)} whole sounds and {plan.dropped_perms} permutations of kept sounds; "
             f"{len(plan.degraded)} sounds halved to 11025 Hz")
    byc: dict[int, list[int]] = {}
    for p in plan.sounds:
        row = byc.setdefault(p.snd.klass, [0, 0, 0, 0, 0, 0])   # sounds, kept sounds, perms, kept perms, half rate, bytes
        row[0] += 1
        row[1] += 1 if p.kept else 0
        row[2] += len(p.snd.perms)
        row[3] += len(p.kept)
        row[4] += 1 if p.kept and p.factor != p.base_factor else 0
        row[5] += p.bytes()
    L.append("  per class (sounds kept/total, permutations kept/total, sounds at half rate, resident KiB):")
    for k in sorted(byc):
        r = byc[k]
        L.append(f"    {class_name(k):<24} sounds {r[1]:>3}/{r[0]:<3} perms {r[3]:>4}/{r[2]:<4} half-rate {r[4]:>3} {r[5] / 1024:>7.1f} KiB")
    if b.origin:
        via: dict[str, int] = {}
        for p in plan.sounds:
            via[b.origin.get(p.snd.tag_id, "?")] = via.get(b.origin.get(p.snd.tag_id, "?"), 0) + 1
        L.append("  sounds first reached from: " + ", ".join(f"{g} {via[g]}" for g in ROOT_GROUPS if g in via))
    loops = sum(1 for p in plan.sounds if p.loop)
    L.append(f"  looping sounds: {loops} ({sum(1 for p in plan.sounds if p.loop and p.kept)} resident)")
    snrs = [s["snr"] for s in b.stats if not math.isinf(s["snr"])]
    if snrs:
        L.append(f"  encoder SNR over {len(snrs)} permutations: min {min(snrs):.1f} dB, median {sorted(snrs)[len(snrs) // 2]:.1f} dB, "
                 f"mean {sum(snrs) / len(snrs):.1f} dB")
    if drops:
        L.append("  dropped sounds:")
        for p in plan.dropped_sounds:
            L.append(f"    {p.snd.tag_id:#010x} {class_name(p.snd.klass):<20} {sum(p.perm_bytes(q) for q in p.dropped):>7} B "
                     f"({len(p.dropped)} perms) {p.snd.path}")
        L.append("  dropped permutations of kept sounds:")
        for p in plan.sounds:
            if p.kept and p.dropped:
                L.append(f"    {p.snd.tag_id:#010x} {p.snd.path}: " + ", ".join(q.name for q in p.dropped))
        L.append("  degraded to 11025 Hz:")
        for p in plan.degraded:
            L.append(f"    {p.snd.tag_id:#010x} {p.snd.path}")
        L.append("  streamed (not resident):")
        for s in plan.streamed:
            L.append(f"    {s.tag_id:#010x} {class_name(s.klass)} {s.path}")
    else:
        top = sorted((p for p in plan.dropped_sounds), key=lambda p: -class_weight(p.snd.klass))[:5]
        if top:
            L.append("  most important dropped sounds: " + "; ".join(p.snd.path.rsplit("\\", 2)[-1] + f" ({class_name(p.snd.klass)})" for p in top))
    return L


def info_pack(path: Path) -> str:
    pk = hp2.parse(path.read_bytes())
    sec = parse_section(pk.read("SND0"), pk.find("SNDD").raw_size)
    flags = lambda f: ",".join(n for b, n in ((F_LOOP, "loop"), (F_DROPPED, "dropped"), (F_STREAMED, "streamed"),  # noqa: E731
                                              (F_DEGRADED, "degraded"), (F_UNSUPPORTED, "unsupported")) if f & b) or "-"
    kept = [s for s in sec.sounds if s["perm_count"]]
    spu = sum(p["size"] - AUDSRV_HEADER for p in sec.perms)
    return (f"{path.name}: SND0 {sec.size} bytes, SNDD {sec.sndd_size} bytes, SPU2 resident {spu} bytes ({100 * spu / sec.budget:.1f} % of "
            f"{sec.budget}), {len(sec.sounds)} sounds ({len(kept)} resident, {sec.counts['dropped_sounds']} dropped, "
            f"{sec.counts['streamed']} streamed), {len(sec.perms)} permutations, {sec.counts['degraded']} at half rate, "
            f"{len(sec.loops)} looping sounds with {len(sec.tracks)} tracks and {len(sec.details)} detail sounds")


# ---- checks, survey, inspection ---------------------------------------------------------------------
def check_all(m: texconv.MapData) -> list[str]:
    """Structural checks of every snd! and lsnd tag of the map; returns the problems (empty = all fine)."""
    t = Tags(m)
    problems = []
    n = 0
    for e in m.index.entries:
        try:
            if e.group == "snd!":
                read_sound(t, e)
                n += 1
            elif e.group == "lsnd":
                read_loop(t, e)
                n += 1
        except SoundError as ex:
            problems.append(str(ex))
    print(f"{m.path.stem}: {n} snd!/lsnd tags checked, {len(problems)} problems")
    return problems


def decode_perm(m: texconv.MapData, s: SrcSound, p: SrcPerm) -> list[list[int]]:
    with m.path.open("rb") as f:
        f.seek(p.file_offset)
        data = f.read(p.size)
    return decode_source(s, data)


def survey(m: texconv.MapData, limit: int, only_gameplay: bool = True) -> list[str]:
    """Decode the longest permutation of up to ``limit`` gameplay sounds (evenly spread over the sorted tag ids),
    report statistics and the plausibility verdict."""
    t = Tags(m)
    reach = closure(t, default_roots(m))
    snds = [read_sound(t, reach[i]) for i in sorted(reach) if reach[i].group == "snd!"]
    snds = [s for s in snds if not (only_gameplay and s.streamed)]
    step = max(1, len(snds) // max(1, limit))
    pick = snds[::step][:limit]
    rows, bad = [], []
    agg = {"frames": 0, "n": 0}
    for s in pick:
        p = max(s.perms, key=lambda q: q.size)
        chans = decode_perm(m, s, p)
        mono = fold_mono(chans)
        st = pcm_stats(mono, s.rate)
        st["boundary"] = boundary_ratio(_read_range(m, p), s.channels) if s.fmt == FORMAT_XBOX_ADPCM else 1.0
        problems = plausibility(st)
        rows.append((s, p, st, problems))
        agg["frames"] += st["frames"]
        agg["n"] += 1
        if problems:
            bad.append((s, problems))
    L = [f"{m.path.stem}: {len(pick)} sounds decoded ({agg['frames']} frames), {len(bad)} flagged"]
    L.append(f"  {'sound':<46}{'cls':>4}{'ch':>3}{'rate':>6}{'sec':>7}{'peak dB':>8}{'rms dB':>8}{'p99 dB':>8}{'dc%':>6}{'clip%':>7}{'hf':>6}{'bnd':>6}")
    for s, p, st, pr in rows:
        L.append(f"  {s.path[-45:]:<46}{s.klass:>4}{s.channels:>3}{s.rate:>6}{st['seconds']:>7.2f}{st['peak_db']:>8.1f}{st['rms_db']:>8.1f}"
                 f"{st['p99_db']:>8.1f}{st['dc_ratio'] * 100:>6.1f}{st['clip_fraction'] * 100:>7.2f}{st['hf_ratio']:>6.2f}{st['boundary']:>6.2f}"
                 + ("  " + "; ".join(pr) if pr else "") + ("  (noisy material)" if st["hf_ratio"] > 0.6 else ""))
    return L


def inspect(m: texconv.MapData, needle: str, wav_dir: Path | None, converted: bool, cache: Cache) -> list[str]:
    """Statistics (and wav files) of every permutation of the sounds whose path contains ``needle``: the decoded source,
    and with ``converted`` also the SPU2 ADPCM re-decoded."""
    t = Tags(m)
    L = []
    for e in m.index.entries:
        if e.group != "snd!" or needle.lower() not in e.path.lower():
            continue
        s = read_sound(t, e)
        L.append(f"{e.path}  id {e.tag_id:#010x} class {class_name(s.klass)} {s.rate} Hz x{s.channels} flags {s.flags:#x} "
                 f"dist {s.min_dist:g}..{s.max_dist:g} pitch {s.pitch_lo:g}..{s.pitch_hi:g} declared max play {s.max_play_ms} ms")
        longest = 0
        for p in s.perms:
            chans = decode_perm(m, s, p)
            mono = fold_mono(chans)
            st = pcm_stats(mono, s.rate)
            longest = max(longest, p.size)
            warn = plausibility(st)
            L.append(f"  perm {p.name!r:<22} {p.size:>7} B -> {st['frames']:>7} frames {st['seconds']:6.3f} s  peak {st['peak_db']:6.1f} dB  "
                     f"rms {st['rms_db']:6.1f} dB  dc {st['dc_ratio'] * 100:4.1f} %  clip {st['clip_fraction'] * 100:.2f} %  "
                     f"hf {st['hf_ratio']:.3f}" + ("  WARN " + "; ".join(warn) if warn else ""))
            if wav_dir:
                wav_dir.mkdir(parents=True, exist_ok=True)
                stem = re.sub(r"[^A-Za-z0-9_.-]+", "_", e.path.split("sound\\", 1)[-1]) + "." + re.sub(r"[^A-Za-z0-9_.-]+", "_", p.name)
                write_wav(wav_dir / f"{stem}.xbox.wav", chans, s.rate)
            if converted:
                factor = max(1, s.rate // 22050)
                blocks, frames, rate, snr = encode_perm(s, _read_range(m, p), factor, False, cache)
                dec = vag.decode(blocks)
                cst = pcm_stats(dec, rate)
                L.append(f"       SPU2: {len(blocks):>7} B, {frames} frames at {rate} Hz, SNR vs its PCM input {snr:.1f} dB, "
                         f"peak {cst['peak_db']:.1f} dB, rms {cst['rms_db']:.1f} dB, dc {cst['dc_ratio'] * 100:.1f} %")
                if wav_dir:
                    write_wav(wav_dir / f"{stem}.spu2.wav", [dec], rate)
        if s.max_play_ms and s.perms:
            ratio = (longest // (36 * s.channels) * 64 / s.rate * 1000) / s.max_play_ms
            L.append(f"  decoded duration of the longest permutation / declared max play length = {ratio:.4f} (64/72 = 0.8889)")
    if not L:
        L.append(f"no snd! tag with {needle!r} in its path")
    return L


def _read_range(m: texconv.MapData, p: SrcPerm) -> bytes:
    with m.path.open("rb") as f:
        f.seek(p.file_offset)
        return f.read(p.size)


# ---- command line -----------------------------------------------------------------------------------
def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("input", type=Path, help="decompressed .map (or a .snd.hp2 with --info)")
    ap.add_argument("-o", "--out", type=Path)
    ap.add_argument("--roots", nargs="+", metavar="TAG", help="tag ids (0x...) or exact tag paths to start from "
                    "(default: every weap proj jpt! effe bipd matg tag)")
    ap.add_argument("--budget", type=int, default=SPU_BUDGET, help=f"resident SPU2 bytes (default {SPU_BUDGET})")
    ap.add_argument("--no-rate-degrade", action="store_true", help="the budget may drop sounds but never halve a rate")
    ap.add_argument("--weight", nargs="+", metavar="CLASS=W", help="override budget class weights, e.g. 18=60 5=70 "
                    "(see CLASS_WEIGHT; classes at 90 or more are never normalised by population)")
    ap.add_argument("--cache", type=Path, help="directory for encoded permutations (shared between maps)")
    ap.add_argument("--exhaustive", action="store_true", help="exhaustive ADPCM filter/shift search (slow)")
    ap.add_argument("--drops", action="store_true", help="list every dropped, degraded and streamed sound")
    ap.add_argument("--json", type=Path, help="write the plan and encoder statistics as JSON")
    ap.add_argument("--info", action="store_true", help="print a summary of a .snd.hp2")
    ap.add_argument("--check", action="store_true", help="structure checks of every snd!/lsnd tag of the map")
    ap.add_argument("--survey", type=int, metavar="N", help="decode N gameplay sounds and print statistics")
    ap.add_argument("--inspect", metavar="TEXT", help="statistics of the sounds whose path contains TEXT")
    ap.add_argument("--wav-dir", type=Path, help="with --inspect: write wav files here")
    ap.add_argument("--converted", action="store_true", help="with --inspect: also encode to SPU2 ADPCM and report")
    args = ap.parse_args(argv)
    if args.info:
        print(info_pack(args.input))
        return 0
    for spec in args.weight or []:
        k, _, v = spec.partition("=")
        try:
            CLASS_WEIGHT[int(k, 0)] = float(v)
        except ValueError:
            ap.error(f"--weight {spec!r}: expected CLASS=NUMBER")
    m = texconv.MapData.open(args.input)
    if args.check:
        pr = check_all(m)
        for x in pr[:20]:
            print("  problem:", x)
        return 1 if pr else 0
    if args.survey:
        print("\n".join(survey(m, args.survey)))
        return 0
    if args.inspect:
        print("\n".join(inspect(m, args.inspect, args.wav_dir, args.converted, Cache(args.cache))))
        return 0
    if not args.out:
        ap.error("-o is required")
    roots = resolve_roots(m, args.roots) if args.roots else None
    cache = Cache(args.cache)
    last = [-1]

    def progress(done: int, total: int) -> None:
        pct = done * 100 // max(1, total)
        if pct // 10 != last[0]:
            last[0] = pct // 10
            print(f"\r  encoding {done}/{total}", end="", file=sys.stderr, flush=True)

    b = build_sections(m, roots, args.budget, not args.no_rate_degrade, cache, args.exhaustive, progress)
    print("", file=sys.stderr)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_bytes(hp2.build(args.input.stem, [("SND0", b.snd0), ("SNDD", b.sndd)]))
    print("\n".join(describe(b, args.input.stem, None, args.drops)))
    print(f"wrote {args.out} (SND0 {len(b.snd0) / 1024:.1f} KiB, SNDD {len(b.sndd) / 1024:.1f} KiB)")
    if args.json:
        plan = b.plan
        args.json.write_text(json.dumps({
            "map": args.input.stem, "budget": plan.budget, "before": plan.total_before, "resident": len(b.sndd),
            "dropped_sounds": [{"tag": f"{p.snd.tag_id:#010x}", "path": p.snd.path, "class": class_name(p.snd.klass)}
                               for p in plan.dropped_sounds],
            "dropped_perms": [{"tag": f"{p.snd.tag_id:#010x}", "path": p.snd.path, "perms": [q.name for q in p.dropped]}
                              for p in plan.sounds if p.kept and p.dropped],
            "degraded": [p.snd.path for p in plan.degraded],
            "streamed": [s.path for s in plan.streamed],
            "encoded": b.stats}, indent=1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
