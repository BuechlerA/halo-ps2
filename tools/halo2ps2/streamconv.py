#!/usr/bin/env python3
"""Convert the streamed sounds of a Xbox cache file (dialogue, music, long ambience) into the PS2 ``STRM`` / ``STRD``
sections, version 1 (docs/29-strm-section.md; the reader is engine/src/audio/strmdef.c, the player engine/src/audio/stream.c).

The source layouts (``snd!`` and ``lsnd`` tags, Xbox ADPCM) are the ones ``soundconv.py`` verified against all ten retail
campaign maps; this module imports its parsers and its Xbox ADPCM decoder and adds nothing to what they check.

What is streamed
    * every ``snd!`` tag soundconv marks streamed (classes 19 32 44 45 46 47 48 and paths below sound\\dialog and
      sound\\music): kind DIALOGUE (19 44-48), MUSIC (32 and the music paths) or AMBIENCE (anything else streamed);
    * long ambience: every other sound that a ``sound_looping`` track uses as its loop / alternate loop and whose longest
      chain lasts at least ``--ambience-seconds`` (default 5) seconds. The SPU2 pack (SND0) may hold a resident copy
      of these (or have dropped them for the budget): the engine plays the resident one when there is one.
    * ``sound_looping`` tags that name at least one of those sounds get a loop record with the track structure.

Permutations
    A streamed ``snd!`` is split in two ways. Dialogue has *alternate takes* (flat permutations, ``next`` = none): each is a
    stream of its own, a *variant* of the sound. Long music is a *chain* of permutations of 65,520 bytes linked by ``next``
    (the tag's "split long sound into permutations" flag): the chain is one signal, so it is concatenated into one stream
    (block aligned, nothing to re-synchronise). Every chain head (a permutation no ``next`` points to) is one variant.

Stream format (verified: soundconv's Xbox ADPCM, the same bytes)
    IMA ADPCM with the standard 89-entry step table, in the *Xbox container*: a block holds 64 samples per channel, mono block
    36 bytes (s16 predictor = sample 0, u8 step index, u8 0, 32 bytes of nibbles low nibble first; nibbles 0..62 give samples
    1..63, nibble 63 is padding), stereo block 72 bytes (L header, R header, eight groups of 4 bytes L / 4 bytes R). A source
    that already meets the target (mono dialogue at 22050 Hz, ambience at 22050 Hz, music at 22050 Hz) is copied *byte for
    byte*: no second generation of quantisation noise, no conversion time. Only what is above its cap is decoded, resampled
    (windowed sinc, 24 taps, 1024 phases) and re-encoded in the same container: music 44100 Hz stereo -> 32000 Hz stereo
    (``--music-rate``), anything above 22050 Hz otherwise, stereo dialogue folded to mono ((l + r) >> 1). Frame counts include
    the up to 63 padding samples of the Xbox encoder (the tag has no sample count). A resampled loop is shrunk to a whole number
    of blocks (< 64 frames; the filter wraps around, so the seam stays continuous) and keeps the nominal rate.

Usage:
    python3 -I tools/halo2ps2/streamconv.py assets/decompressed/a10.map -o assets/hp2/a10.strm.hp2 [--cache DIR] [--jobs N]
        [--music-rate 32000] [--dialogue-rate 22050] [--ambience-seconds 5] [--roots TAG ...] [--no-dialogue]
    python3 -I tools/halo2ps2/streamconv.py --info assets/hp2/a10.strm.hp2
    python3 -I tools/halo2ps2/streamconv.py --verify assets/hp2/a10.strm.hp2 [--samples N]   # parse + decode streams, reference CRCs
"""
from __future__ import annotations

import argparse
import array
import hashlib
import math
import os
import struct
import sys
import time
import zlib
from concurrent.futures import ProcessPoolExecutor
from dataclasses import dataclass, field
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from halo2ps2 import hp2, soundconv as sc, texconv  # noqa: E402

STRM_MAGIC = b"STRM"
VERSION = 1
HEADER_SIZE = 128
SOUND_REC = 64
STREAM_REC = 64
LOOP_REC = 64
TRACK_REC = 48
NAME_LEN = 24
LOOP_NAME_LEN = 32
NO_TAG = 0xFFFFFFFF
NO_PERM = 0xFFFF
DATA_ALIGN = 2048
FMT_XBOX_IMA = 1
BLOCK_FRAMES = 64
MAX_RATE = 48000

# stream.flags
SF_LOOP, SF_FOLDED, SF_RESAMPLED, SF_SHARED = 1, 2, 4, 8
# sound.kind
KIND_DIALOGUE, KIND_MUSIC, KIND_AMBIENCE = 0, 1, 2
KIND_NAMES = {KIND_DIALOGUE: "dialogue", KIND_MUSIC: "music", KIND_AMBIENCE: "ambience"}
# sound.flags
SS_LOOPING, SS_EXTRA = 1, 2

DIALOGUE_CLASSES = {19, 44, 45, 46, 47, 48}
MUSIC_CLASSES = {32}
DEFAULT_RATES = {KIND_DIALOGUE: 22050, KIND_MUSIC: 32000, KIND_AMBIENCE: 22050}
DEFAULT_AMBIENCE_SECONDS = 5.0
CACHE_VERSION = 3
CACHE_FMT = "<IIBBi"
CACHE_HEADER = 4 + struct.calcsize(CACHE_FMT)

TAPS = 12                        # filter taps on each side of the interpolation point
PHASES = 1024


class StreamError(ValueError):
    pass


# ---- planning -------------------------------------------------------------------------------------
@dataclass
class Variant:
    range_index: int
    perms: list[sc.SrcPerm]              # the chain in play order (one entry for a flat permutation)
    gain: float
    skip: float
    name: str


@dataclass
class Item:
    sound: sc.SrcSound
    kind: int
    loop: bool                           # some sound_looping track plays it as loop / alternate loop
    extra: bool                          # included for being a long loop, not for being a streamed class
    variants: list[Variant] = field(default_factory=list)


def kind_of(s: sc.SrcSound) -> int:
    if s.klass in DIALOGUE_CLASSES:
        return KIND_DIALOGUE
    if s.klass in MUSIC_CLASSES or s.path.lower().startswith("sound\\music\\"):
        return KIND_MUSIC
    if s.path.lower().startswith("sound\\dialog\\"):
        return KIND_DIALOGUE
    return KIND_AMBIENCE


def chains(rng: sc.SrcRange) -> tuple[list[list[sc.SrcPerm]], list[str]]:
    """Split one pitch range into chains (flat permutations are chains of one). Returns (chains, problems)."""
    perms = rng.perms
    n = len(perms)
    targets = {p.next_index for p in perms if p.next_index != NO_PERM}
    out: list[list[sc.SrcPerm]] = []
    seen = [False] * n
    problems = []
    for h in range(n):
        if h in targets:
            continue
        chain = []
        i = h
        while i != NO_PERM and 0 <= i < n and not seen[i]:
            seen[i] = True
            chain.append(perms[i])
            i = perms[i].next_index
        if i != NO_PERM and not (0 <= i < n and seen[i] is False):
            problems.append(f"chain from permutation {h} runs into an already used permutation")
        out.append(chain)
    for i in range(n):
        if not seen[i]:
            problems.append(f"permutation {i} is on a cycle and unreachable from a chain head")
    return out, problems


def frames_of(s: sc.SrcSound, perms: list[sc.SrcPerm]) -> int:
    return sum(s.frames(p) for p in perms)


def plan_items(m: texconv.MapData, ambience_seconds: float = DEFAULT_AMBIENCE_SECONDS, dialogue: bool = True,
               dialogue_roots: set[int] | None = None) -> tuple[list[Item], list[sc.SrcLoop], list[str]]:
    """Select the sounds that go into the pack (sorted by tag id), their variants and the sound_looping tags that name them."""
    t = sc.Tags(m)
    sounds: dict[int, sc.SrcSound] = {}
    loops: list[sc.SrcLoop] = []
    problems: list[str] = []
    for e in m.index.entries:
        try:
            if e.group == "snd!":
                sounds[e.tag_id] = sc.read_sound(t, e)
            elif e.group == "lsnd":
                loops.append(sc.read_loop(t, e))
        except sc.SoundError as ex:
            problems.append(str(ex))
    loop_users = {i for lp in loops for i in lp.loop_ids()}
    items: list[Item] = []
    for tid in sorted(sounds):
        s = sounds[tid]
        if s.fmt != sc.FORMAT_XBOX_ADPCM or not s.perms:
            continue                          # the retail maps have no other format; stay explicit
        streamed = s.streamed
        long_loop = False
        if not streamed and tid in loop_users:
            longest = max(frames_of(s, c) for r in s.ranges for c in chains(r)[0]) if s.ranges else 0
            long_loop = longest / s.rate >= ambience_seconds
        if not (streamed or long_loop):
            continue
        kind = kind_of(s)
        if kind == KIND_DIALOGUE and (not dialogue or (dialogue_roots is not None and tid not in dialogue_roots)):
            continue
        it = Item(s, kind, tid in loop_users, not streamed)
        for ri, rng in enumerate(s.ranges):
            cs, pr = chains(rng)
            problems += [f"{s.path}: {x}" for x in pr]
            for c in cs:
                it.variants.append(Variant(ri, c, c[0].gain, c[0].skip, c[0].name))
        if it.variants:
            items.append(it)
    have = {it.sound.tag_id for it in items}
    keep_loops = [lp for lp in loops if any(i in have for i in lp.sound_ids())]
    return items, keep_loops, problems


# ---- ADPCM (Xbox container) -----------------------------------------------------------------------
def unit_bytes(channels: int) -> int:
    return 36 * channels


def check_blocks(data: bytes, channels: int) -> None:
    """Raise StreamError unless ``data`` is a whole number of blocks with valid step indices (checked at C speed)."""
    u = unit_bytes(channels)
    if not data or len(data) % u:
        raise StreamError(f"{len(data)} bytes are not a whole number of {u}-byte blocks")
    if channels == 1:
        if max(data[2::36]) > 88:
            raise StreamError("step index above 88")
    else:
        if max(data[2::72]) > 88 or max(data[6::72]) > 88:
            raise StreamError("step index above 88")


def decode_xbox(data: bytes, channels: int) -> list[array.array]:
    """Reference decoder: one array('h') per channel (soundconv.xbox_adpcm_decode, piecewise to bound memory)."""
    u = unit_bytes(channels)
    outs = [array.array("h") for _ in range(channels)]
    step = u * 1024
    for o in range(0, len(data), step):
        part = sc.xbox_adpcm_decode(data[o:o + step], channels)
        for c in range(channels):
            outs[c].extend(part[c])
    return outs


def pcm_crc(data: bytes, channels: int) -> int:
    """CRC-32 of the decoded interleaved little-endian PCM16 (what the C decoder's output must match)."""
    chans = decode_xbox(data, channels)
    crc = 0
    n = len(chans[0])
    for o in range(0, n, 65536):
        if channels == 1:
            buf = chans[0][o:o + 65536]
        else:
            seg = array.array("h", bytes(4 * len(chans[0][o:o + 65536])))
            seg[0::2] = chans[0][o:o + 65536]
            seg[1::2] = chans[1][o:o + 65536]
            buf = seg
        if sys.byteorder == "big":
            buf.byteswap()
        crc = zlib.crc32(buf.tobytes(), crc)
    return crc & 0xFFFFFFFF


def _encode_channel(x: array.array) -> tuple[bytes, bytes]:
    """Closed-loop IMA encode of ``x`` (length a multiple of 64) -> (block headers 4 bytes each, bodies 32 bytes each).
    The header predictor is the source sample 0 of the block, the step index carries over from the previous block."""
    n = len(x)
    nb = n // BLOCK_FRAMES
    hdrs = bytearray(nb * 4)
    bodies = bytearray(nb * 32)
    diff, nxt, steps = sc._DIFF, sc._NEXT, sc.STEP_TABLE
    s16 = 0
    for b in range(nb):
        base = b * BLOCK_FRAMES
        pred = x[base]
        struct.pack_into("<hBB", hdrs, b * 4, pred, s16 >> 4, 0)
        lo = b * 32
        for i in range(31):
            v = x[base + 1 + 2 * i]
            d = v - pred
            step = steps[s16 >> 4]
            if d < 0:
                m = (-d << 2) // step
                nib0 = 8 | (7 if m > 7 else m)
            else:
                m = (d << 2) // step
                nib0 = 7 if m > 7 else m
            k = s16 + nib0
            pred += diff[k]
            pred = 32767 if pred > 32767 else -32768 if pred < -32768 else pred
            s16 = nxt[k]
            v = x[base + 2 + 2 * i]
            d = v - pred
            step = steps[s16 >> 4]
            if d < 0:
                m = (-d << 2) // step
                nib1 = 8 | (7 if m > 7 else m)
            else:
                m = (d << 2) // step
                nib1 = 7 if m > 7 else m
            k = s16 + nib1
            pred += diff[k]
            pred = 32767 if pred > 32767 else -32768 if pred < -32768 else pred
            s16 = nxt[k]
            bodies[lo + i] = nib0 | (nib1 << 4)
        # sample 63 -> nibble 62 (low nibble of byte 31); nibble 63 is padding (zero)
        v = x[base + 63]
        d = v - pred
        step = steps[s16 >> 4]
        if d < 0:
            m = (-d << 2) // step
            nib = 8 | (7 if m > 7 else m)
        else:
            m = (d << 2) // step
            nib = 7 if m > 7 else m
        k = s16 + nib
        s16 = nxt[k]
        bodies[lo + 31] = nib
    return bytes(hdrs), bytes(bodies)


def encode_xbox(chans: list[array.array]) -> bytes:
    """Encode PCM channels (equal lengths, a multiple of 64) in the Xbox container."""
    n = len(chans[0])
    if n == 0 or n % BLOCK_FRAMES or any(len(c) != n for c in chans) or len(chans) not in (1, 2):
        raise StreamError("encode_xbox: channels must have equal length, a positive multiple of 64")
    enc = [_encode_channel(c) for c in chans]
    nb = n // BLOCK_FRAMES
    if len(chans) == 1:
        h, b = enc[0]
        out = bytearray(nb * 36)
        for i in range(nb):
            out[i * 36:i * 36 + 4] = h[i * 4:i * 4 + 4]
            out[i * 36 + 4:i * 36 + 36] = b[i * 32:i * 32 + 32]
        return bytes(out)
    (hl, bl), (hr, br) = enc
    out = bytearray(nb * 72)
    for i in range(nb):
        o = i * 72
        out[o:o + 4] = hl[i * 4:i * 4 + 4]
        out[o + 4:o + 8] = hr[i * 4:i * 4 + 4]
        for g in range(8):
            out[o + 8 + g * 8:o + 12 + g * 8] = bl[i * 32 + g * 4:i * 32 + g * 4 + 4]
            out[o + 12 + g * 8:o + 16 + g * 8] = br[i * 32 + g * 4:i * 32 + g * 4 + 4]
    return bytes(out)


# ---- resampling -----------------------------------------------------------------------------------
_TABLES: dict[float, list[tuple[float, ...]]] = {}


def _table(c: float) -> list[tuple[float, ...]]:
    """Windowed-sinc rows for every fractional phase; ``c`` = output rate / input rate (< 1): cutoff 0.46 * c of the input rate,
    unity DC gain per row."""
    key = round(c, 9)
    t = _TABLES.get(key)
    if t is None:
        t = []
        fc = 0.46 * c
        for ph in range(PHASES + 1):
            frac = ph / PHASES
            row = []
            for k in range(-TAPS + 1, TAPS + 1):
                d = k - frac
                arg = 2 * math.pi * fc * d
                s = 2 * fc if d == 0 else math.sin(arg) / (math.pi * d)
                row.append(s * sc._blackman_harris(d / TAPS))
            tot = sum(row)
            t.append(tuple(v / tot for v in row))
        _TABLES[key] = t
    return t


def resample(x: array.array, n_out: int, periodic: bool = False) -> array.array:
    """Resample ``x`` to ``n_out`` samples (< len(x)): output sample j sits at input position j * len(x) / n_out. The kernel
    low-passes at 0.46 of the output rate, so the same code handles any downsampling ratio. ``periodic`` wraps around (loops)."""
    n = len(x)
    if not 0 < n_out <= n:
        raise StreamError("resample only reduces the sample count")
    if n_out == n:
        return array.array("h", x)
    table = _table(n_out / n)
    if periodic:
        pad_l = [x[(i - TAPS + 1) % n] for i in range(TAPS - 1)]
        pad_r = [x[i % n] for i in range(TAPS + 1)]
    else:
        pad_l = [0] * (TAPS - 1)
        pad_r = [0] * (TAPS + 1)
    xp = array.array("h", pad_l)
    xp.extend(x)
    xp.extend(pad_r)
    out = array.array("h", bytes(2 * n_out))
    half = n_out // 2
    mul = sc.operator.mul
    width = 2 * TAPS
    pos = 0                      # j * n, advanced incrementally
    i0 = 0
    rem = 0
    for j in range(n_out):
        ph = (rem * PHASES + half) // n_out
        v = sum(map(mul, table[ph], xp[i0:i0 + width]))
        v = int(v + 0.5) if v >= 0 else -int(0.5 - v)
        out[j] = 32767 if v > 32767 else -32768 if v < -32768 else v
        rem += n
        while rem >= n_out:
            rem -= n_out
            i0 += 1
        pos += n
    return out


def plan_output(frames_in: int, src_rate: int, dst_rate: int, loop: bool) -> tuple[int, int, int]:
    """(exact resampled length, stored length, recorded rate). Stored lengths are multiples of 64: padded (last sample held) for
    one-shots, shrunk by less than 64 frames for loops, so the seam stays continuous (the periodic filter wraps around). The
    recorded rate stays the nominal one: the pitch error of a shrunk loop is below 64 / length (0.4 % for a half second loop,
    0.002 % for a two minute track) and every segment of one sound_looping track keeps the same rate, which the player needs."""
    if dst_rate >= src_rate:
        return frames_in, frames_in, src_rate
    exact = max(1, int(round(frames_in * dst_rate / src_rate)))
    if loop and exact >= 2 * BLOCK_FRAMES:
        return exact, exact // BLOCK_FRAMES * BLOCK_FRAMES, dst_rate
    return exact, (exact + BLOCK_FRAMES - 1) // BLOCK_FRAMES * BLOCK_FRAMES, dst_rate


def convert_pcm(chans: list[array.array], src_rate: int, dst_rate: int, loop: bool) -> tuple[list[array.array], int]:
    """PCM channels at src_rate -> channels ready to encode (a multiple of 64 frames) and the rate to record."""
    n = len(chans[0])
    exact, stored, rate = plan_output(n, src_rate, dst_rate, loop)
    out = []
    for c in chans:
        r = resample(c, stored if loop else exact, periodic=loop)
        if len(r) < stored:
            r.extend(array.array("h", [r[-1]] * (stored - len(r))))      # hold the last sample
        out.append(r)
    return out, rate


# ---- jobs -----------------------------------------------------------------------------------------
@dataclass
class Job:
    ranges: list[tuple[int, int]]        # (file offset, bytes) of the chain's permutations
    channels: int                        # source channels
    src_rate: int
    fold: bool                           # stereo -> mono
    dst_rate: int                        # cap
    loop: bool

    @property
    def src_bytes(self) -> int:
        return sum(n for _, n in self.ranges)

    @property
    def out_channels(self) -> int:
        return 1 if self.fold else self.channels

    @property
    def passthrough(self) -> bool:
        return not self.fold and self.src_rate <= self.dst_rate


@dataclass
class Result:
    data: bytes
    frames: int
    rate: int
    channels: int
    snr: float                           # dB against the PCM fed to the encoder; inf for a byte copy
    flags: int


def read_ranges(path: Path, ranges: list[tuple[int, int]]) -> bytes:
    out = bytearray()
    with path.open("rb") as f:
        for off, n in ranges:
            f.seek(off)
            b = f.read(n)
            if len(b) != n:
                raise StreamError(f"{path.name}: range {off:#x}+{n} runs past the end of the file")
            out += b
    return bytes(out)


def job_key(blob_hash: str, j: Job) -> str:
    h = hashlib.sha1(blob_hash.encode())
    h.update(struct.pack("<7I", CACHE_VERSION, j.channels, j.src_rate, int(j.fold), j.dst_rate, int(j.loop), TAPS * 4096 + PHASES))
    return h.hexdigest()


def run_job(map_path: str, j: Job, cache_dir: str | None) -> Result:
    """Source bytes -> stored stream. Passthrough jobs are a verified byte copy; the others are decoded, resampled, encoded."""
    blob = read_ranges(Path(map_path), j.ranges)
    check_blocks(blob, j.channels)
    if j.passthrough:
        return Result(blob, len(blob) // unit_bytes(j.channels) * BLOCK_FRAMES, j.src_rate, j.channels, math.inf,
                      SF_LOOP if j.loop else 0)
    key = job_key(hashlib.sha1(blob).hexdigest(), j)
    cf = Path(cache_dir) / (key + ".stc") if cache_dir else None
    if cf and cf.exists():
        raw = cf.read_bytes()
        if len(raw) > CACHE_HEADER and raw[:4] == b"STC1":
            frames, rate, ch, flags, snr10 = struct.unpack_from(CACHE_FMT, raw, 4)
            return Result(raw[CACHE_HEADER:], frames, rate, ch, math.inf if snr10 == 999999 else snr10 / 10.0, flags)
    chans = decode_xbox(blob, j.channels)
    flags = SF_LOOP if j.loop else 0
    if j.fold and len(chans) == 2:
        mono = array.array("h", ((a + b) >> 1 for a, b in zip(*chans)))
        chans = [mono]
        flags |= SF_FOLDED
    pcm, rate = chans, j.src_rate
    if j.src_rate > j.dst_rate:
        pcm, rate = convert_pcm(chans, j.src_rate, j.dst_rate, j.loop)
        flags |= SF_RESAMPLED
    data = encode_xbox(pcm)
    dec = decode_xbox(data, len(pcm))
    num = sum(sum(v * v for v in c) for c in pcm)
    den = sum(sum((a - b) ** 2 for a, b in zip(c, d)) for c, d in zip(pcm, dec))
    snr = math.inf if den == 0 else (-math.inf if num == 0 else 10 * math.log10(num / den))
    res = Result(data, len(pcm[0]), rate, len(pcm), snr, flags)
    if cf:
        cf.parent.mkdir(parents=True, exist_ok=True)
        s10 = 999999 if math.isinf(snr) else int(round(snr * 10))
        tmp = cf.with_suffix(".tmp%d" % os.getpid())
        tmp.write_bytes(b"STC1" + struct.pack(CACHE_FMT, res.frames, res.rate, res.channels, res.flags, s10) + res.data)
        os.replace(tmp, cf)
    return res


def make_job(it: Item, v: Variant, rates: dict[int, int]) -> Job:
    s = it.sound
    ranges = [(p.file_offset, p.size) for p in v.perms]
    return Job(ranges, s.channels, s.rate, it.kind == KIND_DIALOGUE and s.channels == 2, rates[it.kind], it.loop)


# ---- section writer -------------------------------------------------------------------------------
def _name(s: str, n: int) -> bytes:
    raw = s.encode("latin1", "replace")
    if len(raw) > n - 1:
        raw = raw[-(n - 1):]
    return raw.ljust(n, b"\0")


@dataclass
class Built:
    strm: bytes
    strd: bytes
    stats: dict


def build_sections(m: texconv.MapData, rates: dict[int, int] | None = None, cache_dir: Path | None = None, jobs: int | None = None,
                   ambience_seconds: float = DEFAULT_AMBIENCE_SECONDS, dialogue: bool = True,
                   dialogue_roots: set[int] | None = None, progress=None) -> Built:
    rates = {**DEFAULT_RATES, **(rates or {})}
    items, loops, problems = plan_items(m, ambience_seconds, dialogue, dialogue_roots)
    if problems:
        raise StreamError("; ".join(problems[:5]))
    flat = [(it, v) for it in items for v in it.variants]
    jl = [make_job(it, v, rates) for it, v in flat]
    t0 = time.time()
    results: list[Result | None] = [None] * len(jl)
    heavy = [i for i, j in enumerate(jl) if not j.passthrough]
    # byte copies first (I/O bound), transcodes on a process pool
    for i in (i for i, j in enumerate(jl) if j.passthrough):
        results[i] = run_job(str(m.path), jl[i], None)
    if heavy:
        workers = jobs if jobs else os.cpu_count() or 1
        cd = str(cache_dir) if cache_dir else None
        if workers <= 1 or len(heavy) == 1:
            for n, i in enumerate(heavy):
                results[i] = run_job(str(m.path), jl[i], cd)
                if progress:
                    progress(n + 1, len(heavy))
        else:
            order = sorted(heavy, key=lambda i: -jl[i].src_bytes)       # longest first for a balanced pool
            with ProcessPoolExecutor(max_workers=workers) as ex:
                futs = {i: ex.submit(run_job, str(m.path), jl[i], cd) for i in order}
                for n, i in enumerate(order):
                    results[i] = futs[i].result()
                    if progress:
                        progress(n + 1, len(heavy))
    # layout: shared data (same bytes, same format) is stored once
    data_chunks: list[bytes] = []
    cursor = 0
    seen: dict[tuple, int] = {}
    srecs, vrecs = bytearray(), bytearray()
    shared = 0
    stats = {"streams": len(flat), "sounds": len(items), "src_bytes": 0, "passthrough": 0, "transcoded": 0, "by_kind": {}}
    snr_num = snr_den = 0
    stream_index = 0
    sound_first: dict[int, int] = {}
    for (it, v), j, r in zip(flat, jl, results):
        assert r is not None
        sound_first.setdefault(it.sound.tag_id, stream_index)
        key = (hashlib.sha1(r.data).digest(), r.channels, r.rate)
        flags = r.flags
        if key in seen:
            off = seen[key]
            flags |= SF_SHARED
            shared += 1
        else:
            pad = (-cursor) % DATA_ALIGN
            if pad:
                data_chunks.append(bytes(pad))
                cursor += pad
            off = cursor
            seen[key] = off
            data_chunks.append(r.data)
            cursor += len(r.data)
        loop_start, loop_end = (0, r.frames) if flags & SF_LOOP else (0, 0)
        snr10 = 0x7FFFFFFF if math.isinf(r.snr) else int(round(r.snr * 10))
        vrecs += struct.pack("<IIIIIIffBBHHHIIi12x", off, len(r.data), r.frames, r.rate, loop_start, loop_end, v.gain, v.skip,
                             r.channels, FMT_XBOX_IMA, flags, len(v.perms), v.range_index, it.sound.tag_id, j.src_bytes, snr10)
        assert len(vrecs) == (stream_index + 1) * STREAM_REC
        stats["src_bytes"] += j.src_bytes
        k = KIND_NAMES[it.kind]
        bk = stats["by_kind"].setdefault(k, {"sounds": set(), "streams": 0, "src": 0, "out": 0, "frames": 0})
        bk["sounds"].add(it.sound.tag_id)
        bk["streams"] += 1
        bk["src"] += j.src_bytes
        bk["out"] += len(r.data)
        bk["frames"] += r.frames / r.rate
        if j.passthrough:
            stats["passthrough"] += 1
        else:
            stats["transcoded"] += 1
            if not math.isinf(r.snr):
                snr_num += r.snr
                snr_den += 1
        stream_index += 1
    stats["shared"] = shared
    stats["snr_mean"] = snr_num / snr_den if snr_den else None
    for it in items:
        s = it.sound
        flags = (SS_LOOPING if it.loop else 0) | (SS_EXTRA if it.extra else 0)
        srecs += struct.pack("<IBBHHHIffffII24s", s.tag_id, s.klass, it.kind, flags, len(it.variants), 0, sound_first[s.tag_id],
                             s.min_dist, s.max_dist, s.skip, s.gain_mod, s.max_play_ms, s.flags, _name(s.path, NAME_LEN))
    assert len(srecs) == len(items) * SOUND_REC
    # loops
    sound_ids = {it.sound.tag_id for it in items}
    lrecs, trecs = bytearray(), bytearray()
    for lp in loops:
        lrecs += struct.pack("<IIfHHII8x32s", lp.tag_id, lp.flags, lp.param, len(lp.tracks), 0, len(trecs) // TRACK_REC, 0,
                             _name(lp.path, LOOP_NAME_LEN))
        for tr in lp.tracks:
            trecs += struct.pack("<IfffIIIII12x", tr.flags, tr.gain, tr.fade_in, tr.fade_out, tr.start, tr.loop, tr.end,
                                 tr.alt_loop, tr.alt_end)
    assert len(lrecs) == len(loops) * LOOP_REC and len(trecs) == sum(len(lp.tracks) for lp in loops) * TRACK_REC
    # tables, each 16-byte aligned, after the 128-byte header
    parts = [("sounds", bytes(srecs)), ("streams", bytes(vrecs)), ("loops", bytes(lrecs)), ("tracks", bytes(trecs))]
    offs = {}
    cur = HEADER_SIZE
    body = bytearray()
    for name, blob in parts:
        offs[name] = cur
        body += blob
        pad = (-len(blob)) % 16
        body += bytes(pad)
        cur += len(blob) + pad
    total = HEADER_SIZE + len(body)
    data_size = cursor
    by = stats["by_kind"]
    hdr = bytearray(HEADER_SIZE)
    struct.pack_into("<4sHHI", hdr, 0, STRM_MAGIC, VERSION, 0, total)
    struct.pack_into("<4I", hdr, 0x0C, len(items), len(flat), len(loops), len(trecs) // TRACK_REC)
    struct.pack_into("<4I", hdr, 0x1C, offs["sounds"], offs["streams"], offs["loops"], offs["tracks"])
    struct.pack_into("<II", hdr, 0x2C, data_size, DATA_ALIGN)
    struct.pack_into("<IIII", hdr, 0x34, min(stats["src_bytes"], 0xFFFFFFFF), stats["passthrough"], stats["transcoded"], shared)
    struct.pack_into("<III", hdr, 0x44, by.get("dialogue", {}).get("streams", 0), by.get("music", {}).get("streams", 0),
                     by.get("ambience", {}).get("streams", 0))
    strm = bytes(hdr) + bytes(body)
    assert len(strm) == total
    stats["strm_size"] = len(strm)
    stats["data_size"] = data_size
    stats["seconds"] = time.time() - t0
    stats["loops"] = len(loops)
    stats["tracks"] = len(trecs) // TRACK_REC
    return Built(strm, b"".join(data_chunks), stats)


def write_pack(path: Path, name: str, b: Built) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    data = hp2.build(name, [("STRM", b.strm), ("STRD", b.strd)], compress=False)
    path.write_bytes(data)


# ---- reference parser -----------------------------------------------------------------------------
@dataclass
class Section:
    sounds: list[dict]
    streams: list[dict]
    loops: list[dict]
    tracks: list[dict]
    header: dict
    data_size: int


def _cstr(b: bytes) -> str:
    return b.split(b"\0", 1)[0].decode("latin1")


def parse_section(strm: bytes, data_size: int) -> Section:
    """Parse and validate an STRM section (raises StreamError). Mirrors what engine/src/audio/strmdef.c checks."""
    if len(strm) < HEADER_SIZE or strm[:4] != STRM_MAGIC:
        raise StreamError("bad magic")
    ver, _res, total = struct.unpack_from("<HHI", strm, 4)
    if ver != VERSION or total != len(strm):
        raise StreamError("bad version or size")
    ns, nv, nl, nt = struct.unpack_from("<4I", strm, 0x0C)
    so, vo, lo, to = struct.unpack_from("<4I", strm, 0x1C)
    dsz, dal = struct.unpack_from("<II", strm, 0x2C)
    if dsz != data_size or dal == 0 or dal & (dal - 1):
        raise StreamError("data size / alignment mismatch")
    for off, cnt, rec in ((so, ns, SOUND_REC), (vo, nv, STREAM_REC), (lo, nl, LOOP_REC), (to, nt, TRACK_REC)):
        if off % 16 or off < HEADER_SIZE or off + cnt * rec > len(strm):
            raise StreamError("table outside the section")
    sounds = []
    prev = -1
    for i in range(ns):
        tag, klass, kind, flags, cnt, _r, first, mind, maxd, skip, gmod, maxms, tflags, name = struct.unpack_from(
            "<IBBHHHIffffII24s", strm, so + i * SOUND_REC)
        if tag <= prev and i:
            raise StreamError("sound ids not ascending")
        prev = tag
        if first + cnt > nv or cnt == 0 or kind > KIND_AMBIENCE or flags & ~3 or not all(map(math.isfinite, (mind, maxd, skip, gmod))):
            raise StreamError(f"sound {i}: bad stream range, kind or float")
        sounds.append(dict(tag=tag, klass=klass, kind=kind, flags=flags, count=cnt, first=first, min_dist=mind, max_dist=maxd,
                           skip=skip, gain_mod=gmod, max_play_ms=maxms, tag_flags=tflags, name=_cstr(name)))
    streams = []
    for i in range(nv):
        (off, size, frames, rate, ls, le, gain, skip, ch, fmt, flags, chunks, rng, owner, src, snr) = struct.unpack_from(
            "<IIIIIIffBBHHHIIi12x", strm, vo + i * STREAM_REC)
        u = unit_bytes(ch) if ch in (1, 2) else 0
        if not u or fmt != FMT_XBOX_IMA or size == 0 or size % u or frames != size // u * BLOCK_FRAMES:
            raise StreamError(f"stream {i}: bad format/size")
        if off % dal or off + size > data_size or not 0 < rate <= MAX_RATE or flags & ~15 or not math.isfinite(gain + skip):
            raise StreamError(f"stream {i}: bad offset/rate/flags/float")
        if (flags & SF_LOOP) != (le != 0) or (le and not (ls < le <= frames)):
            raise StreamError(f"stream {i}: bad loop points")
        streams.append(dict(offset=off, size=size, frames=frames, rate=rate, loop_start=ls, loop_end=le, gain=gain, skip=skip,
                            channels=ch, fmt=fmt, flags=flags, chunks=chunks, range=rng, owner=owner, src_bytes=src, snr10=snr))
    loops = []
    for i in range(nl):
        tag, flags, param, tc, _dc, tf, _df, name = struct.unpack_from("<IIfHHII8x32s", strm, lo + i * LOOP_REC)
        if tf + tc > nt or not math.isfinite(param):
            raise StreamError(f"loop {i}: bad track range")
        loops.append(dict(tag=tag, flags=flags, param=param, track_count=tc, track_first=tf, name=_cstr(name)))
    tracks = []
    for i in range(nt):
        fl, gain, fin, fout, s, l, e, al, ae = struct.unpack_from("<IfffIIIII12x", strm, to + i * TRACK_REC)
        if not math.isfinite(gain + fin + fout):
            raise StreamError(f"track {i}: bad float")
        tracks.append(dict(flags=fl, gain=gain, fade_in=fin, fade_out=fout, start=s, loop=l, end=e, alt_loop=al, alt_end=ae))
    return Section(sounds, streams, loops, tracks, dict(total=total, data_align=dal), dsz)


def load_pack(path: Path) -> tuple[Section, bytes, int]:
    """Parse a .strm.hp2: (section, whole file bytes, file offset of STRD)."""
    raw = path.read_bytes()
    pk = hp2.parse(raw)
    sec = pk.find("STRD")
    strm = pk.read("STRM")
    if sec.codec != hp2.CODEC_STORED:
        raise StreamError("STRD must be stored")
    return parse_section(strm, sec.raw_size), raw, sec.offset


def stream_bytes(raw: bytes, base: int, st: dict) -> bytes:
    return raw[base + st["offset"]:base + st["offset"] + st["size"]]


# ---- reporting ------------------------------------------------------------------------------------
def describe(name: str, b: Built, rates: dict[int, int]) -> list[str]:
    st = b.stats
    L = [f"{name}: {st['sounds']} sounds, {st['streams']} streams ({st['passthrough']} copied, {st['transcoded']} re-encoded, "
         f"{st['shared']} shared), {st['loops']} sound_looping with {st['tracks']} tracks"]
    for k in ("dialogue", "music", "ambience"):
        bk = st["by_kind"].get(k)
        if bk:
            L.append(f"  {k:<9} {len(bk['sounds']):>5} sounds {bk['streams']:>5} streams {bk['frames'] / 60:>8.1f} min  "
                     f"source {bk['src'] / 2**20:>7.2f} MiB -> {bk['out'] / 2**20:>7.2f} MiB")
    snr = f", mean SNR of the re-encoded {st['snr_mean']:.1f} dB" if st["snr_mean"] is not None else ""
    L.append(f"  STRM {st['strm_size'] / 1024:.1f} KiB, STRD {st['data_size'] / 2**20:.2f} MiB (source {st['src_bytes'] / 2**20:.2f} MiB, "
             f"sector padding included){snr}, {st['seconds']:.1f} s")
    return L


def info_pack(path: Path) -> str:
    sec, raw, base = load_pack(path)
    kinds: dict[int, list[int]] = {}
    byid = {s["tag"]: s for s in sec.sounds}
    for s in sec.sounds:
        kinds.setdefault(s["kind"], [0, 0, 0])
        k = kinds[s["kind"]]
        k[0] += 1
        k[1] += s["count"]
        k[2] += sum(sec.streams[s["first"] + i]["size"] for i in range(s["count"]))
    parts = ", ".join(f"{KIND_NAMES[k]} {v[0]} sounds / {v[1]} streams / {v[2] / 2**20:.2f} MiB" for k, v in sorted(kinds.items()))
    missing = sum(1 for tr in sec.tracks for i in (tr["start"], tr["loop"], tr["end"], tr["alt_loop"], tr["alt_end"])
                  if i != NO_TAG and i not in byid)
    return (f"{path.name}: {len(sec.sounds)} sounds, {len(sec.streams)} streams, {len(sec.loops)} loops, {len(sec.tracks)} tracks, "
            f"STRD {sec.data_size / 2**20:.2f} MiB ({parts}); track sounds resolved in SND0 only: {missing}")


def track_format_problems(sec: Section) -> list[str]:
    """Every streamed segment of one sound_looping track (start, loop, end, alternates; every variant) must have the same channel count
    and rate: the player renders a track through one voice."""
    byid = {s["tag"]: s for s in sec.sounds}
    problems = []
    for lp in sec.loops:
        for k in range(lp["track_count"]):
            tr = sec.tracks[lp["track_first"] + k]
            fmts = set()
            for key in ("start", "loop", "end", "alt_loop", "alt_end"):
                s = byid.get(tr[key])
                if s:
                    fmts |= {(sec.streams[s["first"] + i]["channels"], sec.streams[s["first"] + i]["rate"]) for i in range(s["count"])}
            if len(fmts) > 1:
                problems.append(f"loop {lp['tag']:#x} track {k}: formats {sorted(fmts)}")
    return problems


def verify_pack(path: Path, samples: int = 40, crc_out: Path | None = None) -> list[str]:
    """Parse the pack, check every stream's block headers and decode up to ``samples`` evenly spread streams with the reference
    decoder (CRC-32 of the PCM goes to ``crc_out`` for the C decoder test). Returns problems."""
    sec, raw, base = load_pack(path)
    problems = track_format_problems(sec)
    bad = set()
    for i, st in enumerate(sec.streams):
        try:
            check_blocks(stream_bytes(raw, base, st), st["channels"])
        except StreamError as ex:
            problems.append(f"stream {i}: {ex}")
            bad.add(i)
    step = max(1, len(sec.streams) // max(1, samples))
    lines = []
    for i in list(range(0, len(sec.streams), step))[:samples]:
        if i in bad:
            continue
        st = sec.streams[i]
        lines.append(f"{i} {pcm_crc(stream_bytes(raw, base, st), st['channels']):08x}")
    if crc_out:
        crc_out.write_text("\n".join(lines) + "\n")
    return problems


# ---- command line ---------------------------------------------------------------------------------
def parse_roots(m: texconv.MapData, specs: list[str]) -> set[int]:
    out: set[int] = set()
    for sp in specs:
        if sp.startswith("@"):
            for line in Path(sp[1:]).read_text().split():
                out.add(int(line, 0))
            continue
        try:
            out.add(int(sp, 0))
        except ValueError:
            e = next((x for x in m.index.entries if x.path == sp or x.path.lower() == sp.lower()), None)
            if e is None:
                raise StreamError(f"--roots: no tag {sp!r} in {m.path.stem}")
            out.add(e.tag_id)
    return out


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("input", type=Path, help="decompressed .map (or a .strm.hp2 with --info / --verify)")
    ap.add_argument("-o", "--out", type=Path)
    ap.add_argument("--cache", type=Path, help="directory for re-encoded streams (shared between maps)")
    ap.add_argument("--jobs", type=int, help="worker processes for the re-encodes (default: all cores)")
    ap.add_argument("--music-rate", type=int, default=DEFAULT_RATES[KIND_MUSIC])
    ap.add_argument("--dialogue-rate", type=int, default=DEFAULT_RATES[KIND_DIALOGUE])
    ap.add_argument("--ambience-rate", type=int, default=DEFAULT_RATES[KIND_AMBIENCE])
    ap.add_argument("--ambience-seconds", type=float, default=DEFAULT_AMBIENCE_SECONDS,
                    help="loop sounds at least this long (s) are added as ambience streams")
    ap.add_argument("--roots", nargs="+", metavar="TAG", help="keep only these dialogue sounds (tag ids, exact paths, or @file of ids); "
                    "music, ambience and sound_looping are always kept")
    ap.add_argument("--no-dialogue", action="store_true", help="music and ambience only")
    ap.add_argument("--info", action="store_true", help="summary of a .strm.hp2")
    ap.add_argument("--verify", action="store_true", help="parse and decode a .strm.hp2 with the reference decoder")
    ap.add_argument("--samples", type=int, default=40, help="with --verify: streams to decode")
    ap.add_argument("--crc-out", type=Path, help="with --verify: write 'index crc32' lines of the decoded PCM")
    args = ap.parse_args(argv)
    if args.info:
        print(info_pack(args.input))
        return 0
    if args.verify:
        pr = verify_pack(args.input, args.samples, args.crc_out)
        for x in pr[:20]:
            print("  problem:", x)
        print(f"{args.input.name}: {'ok' if not pr else str(len(pr)) + ' problems'}")
        return 1 if pr else 0
    if not args.out:
        ap.error("-o is required")
    m = texconv.MapData.open(args.input)
    roots = parse_roots(m, args.roots) if args.roots else None
    rates = {KIND_DIALOGUE: args.dialogue_rate, KIND_MUSIC: args.music_rate, KIND_AMBIENCE: args.ambience_rate}
    last = [-1]

    def progress(done: int, total: int) -> None:
        if done * 10 // total != last[0]:
            last[0] = done * 10 // total
            print(f"\r  re-encoding {done}/{total}", end="", file=sys.stderr, flush=True)

    b = build_sections(m, rates, args.cache, args.jobs, args.ambience_seconds, not args.no_dialogue, roots, progress)
    print("", file=sys.stderr)
    write_pack(args.out, args.input.stem, b)
    print("\n".join(describe(args.input.stem, b, rates)))
    print(f"wrote {args.out} ({args.out.stat().st_size / 2**20:.2f} MiB)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
