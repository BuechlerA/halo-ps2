#!/usr/bin/env python3
"""SPU2 ADPCM ("VAG") encoder and reference decoder, stdlib only (docs/21-snd-section.md).

The PS2 SPU2 plays 4-bit ADPCM in blocks of 16 bytes holding 28 samples:

    byte 0   high nibble = predictor (filter) index 0..4, low nibble = shift 0..12
    byte 1   flags: bit 0 "loop end" (last block of the sample / of the loop), bit 1 "loop repeat" (with bit 0:
             jump back to the loop start instead of stopping), bit 2 "loop start" (remember this block as the
             jump target)
    byte 2.. 28 nibbles, low nibble first, each a signed 4-bit residual

Decoding one sample (this is the SPU's integer arithmetic as documented by the PSX/SPU2 hardware references;
the rounding term ``+ 32`` is the documented one, it is *not* verified on a console, ADR-022 defers that to
Phase 6; the encoder runs the identical decoder so any rounding difference changes the output by at most one
LSB per sample and cannot accumulate because every filter is stable):

    residual = (nibble << 12) >> shift                     (nibble sign-extended; exactly nibble * 2^(12-shift))
    sample   = clamp16(residual + ((f0 * s1 + f1 * s2 + 32) >> 6))     s1, s2 = the two previous *decoded* samples
    filters (f0, f1) = (0, 0) (60, 0) (115, -52) (98, -55) (122, -60)

Encoder
    Closed loop (it tracks the decoded values, not the source values) and per 28-sample block it picks the
    (filter, shift) pair with the smallest squared error. The default search estimates the needed shift for every
    filter from the open-loop residual and then tries that shift and its two neighbours (15 trials instead of 65);
    ``exhaustive=True`` tries all 65 and is used by the tests to bound the loss of the fast search.
    The first block of every sample is forced to filter 0, so a voice starting from a zeroed decoder state and a
    voice jumping back to the loop start from any state decode block 0 identically (filter 0 ignores the history).
    That is what makes a loop seamless in the decoder; the source waveform has to be periodic for the seam to be
    inaudible (soundconv stretches loops to a multiple of 28 samples for exactly that reason).

Flags written by ``encode``: a one-shot has 0x00 everywhere except the last block (0x01, stop); a loop has 0x04 on
the first block, 0x03 on the last and 0x00 between.

Usage (debugging aids):
    python3 -I tools/halo2ps2/vag.py selftest
    python3 -I tools/halo2ps2/vag.py wav in.vag out.wav --rate 22050     # decode a VAGp file (or raw blocks) to a wav
"""
from __future__ import annotations

import argparse
import math
import struct
import sys
import wave
from pathlib import Path

BLOCK_BYTES = 16
BLOCK_SAMPLES = 28
FILTERS = ((0, 0), (60, 0), (115, -52), (98, -55), (122, -60))
MAX_SHIFT = 12
FLAG_END, FLAG_REPEAT, FLAG_START = 1, 2, 4
FLAGS_ONESHOT_LAST = FLAG_END
FLAGS_LOOP_FIRST = FLAG_START
FLAGS_LOOP_LAST = FLAG_END | FLAG_REPEAT


class VagError(ValueError):
    pass


def _clamp16(v: int) -> int:
    return -32768 if v < -32768 else 32767 if v > 32767 else v


# ---- decoder --------------------------------------------------------------------------------------
def decode_block(block: bytes, s1: int = 0, s2: int = 0) -> tuple[list[int], int, int]:
    """Decode one 16-byte block; returns (28 samples, new s1, new s2). Raises VagError for an invalid header
    (filter > 4; shifts above 12 are accepted and treated as 9 as the hardware does)."""
    if len(block) != BLOCK_BYTES:
        raise VagError("a block is 16 bytes")
    filt, shift = block[0] >> 4, block[0] & 15
    if filt > 4:
        raise VagError(f"invalid predictor index {filt}")
    if shift > MAX_SHIFT:
        shift = 9
    f0, f1 = FILTERS[filt]
    out = []
    for i in range(BLOCK_SAMPLES):
        byte = block[2 + (i >> 1)]
        nib = (byte >> 4) if (i & 1) else (byte & 15)
        if nib >= 8:
            nib -= 16
        s = ((nib << 12) >> shift) + ((f0 * s1 + f1 * s2 + 32) >> 6)
        s = _clamp16(s)
        out.append(s)
        s2, s1 = s1, s
    return out, s1, s2


def block_flags(block: bytes) -> int:
    return block[1]


def decode(data: bytes, passes: int = 1) -> list[int]:
    """Decode a whole sample the way the voice plays it: from a zeroed state to the block with the end flag.
    If a loop start was seen and the end block repeats, the loop is played ``passes`` times in total (state
    carried across the jump, as on the hardware) and decoding stops after the last pass. A sample whose last
    block lacks the end flag is decoded to the end of the data."""
    if len(data) % BLOCK_BYTES:
        raise VagError("data is not a whole number of blocks")
    nblocks = len(data) // BLOCK_BYTES
    out: list[int] = []
    s1 = s2 = 0
    loop_start = 0
    pos = 0
    done_passes = 0
    guard = nblocks * (passes + 1) + 1
    while pos < nblocks and guard > 0:
        guard -= 1
        blk = data[pos * BLOCK_BYTES:(pos + 1) * BLOCK_BYTES]
        fl = blk[1]
        if fl & FLAG_START:
            loop_start = pos
        smp, s1, s2 = decode_block(blk, s1, s2)
        out += smp
        if fl & FLAG_END:
            if fl & FLAG_REPEAT:
                done_passes += 1
                if done_passes >= passes:
                    break
                pos = loop_start
                continue
            break
        pos += 1
    return out


def check_blocks(data: bytes) -> list[str]:
    """Structural problems of a block stream (empty list = fine): predictor/shift range and flag sequence."""
    problems: list[str] = []
    if len(data) % BLOCK_BYTES:
        return ["length is not a multiple of 16"]
    n = len(data) // BLOCK_BYTES
    if n == 0:
        return ["no blocks"]
    for i in range(n):
        b = data[i * BLOCK_BYTES:(i + 1) * BLOCK_BYTES]
        if b[0] >> 4 > 4 or (b[0] & 15) > MAX_SHIFT:
            problems.append(f"block {i}: bad header {b[0]:#04x}")
        if b[1] & ~7:
            problems.append(f"block {i}: unknown flag bits {b[1]:#04x}")
        if (b[1] & FLAG_END) and i != n - 1:
            problems.append(f"block {i}: end flag before the last block")
    last = data[-BLOCK_BYTES + 1]
    if not last & FLAG_END:
        problems.append("last block lacks the end flag")
    return problems


# ---- encoder --------------------------------------------------------------------------------------
def _encode_with(x: list[int], s1: int, s2: int, filt: int, shift: int, limit: float) -> tuple[float, list[int], int, int]:
    """Closed-loop encode of one block with a fixed (filter, shift). Returns (squared error or inf when it
    exceeds ``limit``, nibbles (0..15), s1, s2)."""
    f0, f1 = FILTERS[filt]
    step = 1 << (MAX_SHIFT - shift)
    err = 0.0
    nibs = []
    for v in x:
        pred = (f0 * s1 + f1 * s2 + 32) >> 6
        q = (v - pred + (step >> 1)) // step           # round to nearest, ties up
        if q < -8:
            q = -8
        elif q > 7:
            q = 7
        r = q * step + pred
        if r < -32768:
            r = -32768
        elif r > 32767:
            r = 32767
        d = v - r
        err += d * d
        if err >= limit:
            return math.inf, nibs, s1, s2
        nibs.append(q & 15)
        s2, s1 = s1, r
    return err, nibs, s1, s2


def _shift_estimate(x: list[int], s1: int, s2: int, filt: int) -> int:
    """Largest shift (smallest step) whose residual range [-8, 7] * step covers the open-loop residual."""
    f0, f1 = FILTERS[filt]
    p1, p2 = s1, s2
    lo = hi = 0
    for v in x:
        e = v - ((f0 * p1 + f1 * p2 + 32) >> 6)
        if e < lo:
            lo = e
        elif e > hi:
            hi = e
        p2, p1 = p1, v
    need = max(hi / 7.0, -lo / 8.0, 1.0)
    shift = MAX_SHIFT
    while shift > 0 and (1 << (MAX_SHIFT - shift)) < need:
        shift -= 1
    return shift


def encode_block(x: list[int], s1: int, s2: int, flags: int = 0, first: bool = False,
                 exhaustive: bool = False) -> tuple[bytes, int, int, float]:
    """Encode up to 28 samples (padded with zeros) given the decoder history; returns (16 bytes, s1, s2, squared
    error). ``first`` forces filter 0 (see the module docstring)."""
    if len(x) < BLOCK_SAMPLES:
        x = x + [0] * (BLOCK_SAMPLES - len(x))
    filters = (0,) if first else range(len(FILTERS))
    best = (math.inf, 0, 0, None, 0, 0)
    for filt in filters:
        if exhaustive:
            shifts = range(MAX_SHIFT + 1)
        else:
            e = _shift_estimate(x, s1, s2, filt)
            shifts = sorted({max(0, e - 1), e, min(MAX_SHIFT, e + 1)})
        for sh in shifts:
            err, nibs, a, b = _encode_with(x, s1, s2, filt, sh, best[0])
            if err < best[0]:
                best = (err, filt, sh, nibs, a, b)
    err, filt, sh, nibs, a, b = best
    if nibs is None:                      # only possible if every trial overflowed: cannot happen with limit=inf first
        raise VagError("encoder failed")
    out = bytearray(BLOCK_BYTES)
    out[0] = (filt << 4) | sh
    out[1] = flags
    for i in range(0, BLOCK_SAMPLES, 2):
        out[2 + (i >> 1)] = nibs[i] | (nibs[i + 1] << 4)
    return bytes(out), a, b, err


def encode(samples: list[int], loop: bool = False, exhaustive: bool = False) -> bytes:
    """Encode a whole sample. ``samples`` is padded with zeros to a multiple of 28 (callers that loop must hand in
    a multiple of 28 themselves). One-shot: last block flagged END. Loop: first block START, last END|REPEAT."""
    n = len(samples)
    nblocks = max(1, (n + BLOCK_SAMPLES - 1) // BLOCK_SAMPLES)
    out = bytearray()
    s1 = s2 = 0
    for b in range(nblocks):
        chunk = [int(v) for v in samples[b * BLOCK_SAMPLES:(b + 1) * BLOCK_SAMPLES]]
        flags = 0
        if b == nblocks - 1:
            flags = FLAGS_LOOP_LAST if loop else FLAGS_ONESHOT_LAST
        elif b == 0 and loop:
            flags = FLAGS_LOOP_FIRST
        if b == 0 and nblocks == 1 and loop:
            flags = FLAGS_LOOP_LAST | FLAGS_LOOP_FIRST
        blk, s1, s2, _ = encode_block(chunk, s1, s2, flags, first=(b == 0), exhaustive=exhaustive)
        out += blk
    return bytes(out)


def encoded_size(nsamples: int) -> int:
    """Bytes ``encode`` produces for ``nsamples`` samples."""
    return max(1, (nsamples + BLOCK_SAMPLES - 1) // BLOCK_SAMPLES) * BLOCK_BYTES


# ---- measurements ---------------------------------------------------------------------------------
def snr_db(ref: list[int], test: list[int]) -> float:
    """10 log10(sum ref^2 / sum (ref - test)^2) over the common length; inf for an exact match, -inf if ref is silent
    and test is not."""
    n = min(len(ref), len(test))
    sig = sum(v * v for v in ref[:n])
    noise = sum((a - b) ** 2 for a, b in zip(ref[:n], test[:n]))
    if noise == 0:
        return math.inf
    if sig == 0:
        return -math.inf
    return 10.0 * math.log10(sig / noise)


# ---- files ----------------------------------------------------------------------------------------
def vag_file(data: bytes, rate: int, name: str = "") -> bytes:
    """A classic ``VAGp`` file (48-byte big-endian header, then the blocks). Debugging aid: most PS2 sound tools open it."""
    hdr = struct.pack(">4sIIII12s16s", b"VAGp", 0x20, 0, len(data), rate, b"", name.encode("ascii", "replace")[:16])
    return hdr + data


def write_wav(path: Path, samples: list[int], rate: int) -> None:
    """16-bit mono wav (stdlib wave)."""
    with wave.open(str(path), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(struct.pack(f"<{len(samples)}h", *samples))


def read_wav(path: Path) -> tuple[list[int], int]:
    with wave.open(str(path), "rb") as w:
        if w.getsampwidth() != 2:
            raise VagError("only 16-bit wav")
        ch = w.getnchannels()
        raw = w.readframes(w.getnframes())
        v = struct.unpack(f"<{len(raw) // 2}h", raw)
        return list(v[::ch]), w.getframerate()


# ---- self test ------------------------------------------------------------------------------------
def _selftest() -> int:
    import random
    rng = random.Random(7)
    rate = 22050
    n = 28 * 400
    sig = [int(9000 * math.sin(2 * math.pi * 440 * i / rate) + 3000 * math.sin(2 * math.pi * 3100 * i / rate)
               + rng.gauss(0, 150)) for i in range(n)]
    data = encode(sig)
    dec = decode(data)
    assert len(dec) == n and not check_blocks(data)
    snr = snr_db(sig, dec)
    snr_x = snr_db(sig, decode(encode(sig, exhaustive=True)))
    loop = encode(sig, loop=True)
    two = decode(loop, passes=2)
    assert two[:n] == two[n:], "loop pass 2 differs from pass 1 (block 0 is not history independent)"
    print(f"vag selftest: SNR {snr:.1f} dB (exhaustive {snr_x:.1f} dB), loop pass identical, {len(data)} bytes for {n} samples")
    return 0 if snr > 25 else 1


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("selftest")
    aw = sub.add_parser("wav", help="decode a .vag (VAGp header detected) or a raw block file to a wav")
    aw.add_argument("src", type=Path)
    aw.add_argument("dst", type=Path)
    aw.add_argument("--rate", type=int, default=22050)
    args = ap.parse_args(argv)
    if args.cmd == "selftest":
        return _selftest()
    raw = args.src.read_bytes()
    rate = args.rate
    if raw[:4] == b"VAGp":
        rate = struct.unpack_from(">I", raw, 16)[0]
        raw = raw[48:]
    samples = decode(raw)
    write_wav(args.dst, samples, rate)
    print(f"{args.dst}: {len(samples)} samples at {rate} Hz")
    return 0


if __name__ == "__main__":
    sys.exit(main())
