"""SND0 / SNDD conversion on a synthetic map (no game data needed): the SPU2 ADPCM encoder and reference decoder
(vag.py), the Xbox ADPCM decoder against its own reference encoder (mono and stereo layouts), the resampler, tag
parsing, reachability (what a root reaches and what it must not), looping sounds, the budget planner, the section
writer and its reference parser, corruption handling. Set SND_FIXTURE=<path> to also write the .hp2 that the C
reader test (engine/tests/test_snddef.c, which hard-codes the numbers of this world) opens. Skip-if-missing checks
convert the real a10 map when assets/decompressed/a10.map exists."""
import math
import os
import random
import struct
import sys
import tempfile
from pathlib import Path

sys.path[:0] = [str(Path(__file__).resolve().parents[1]), str(Path(__file__).resolve().parent)]
from halo2ps2 import hp2, soundconv as sc, tagindex, texconv, vag  # noqa: E402

BASE = tagindex.XBOX_TAG_BASE
ROOT = Path(__file__).resolve().parents[2]
NONE = 0xFFFFFFFF


def sine(n: int, freq: float, amp: float, rate: int, phase: float = 0.0) -> list[int]:
    return [int(round(amp * math.sin(2 * math.pi * freq * i / rate + phase))) for i in range(n)]


def noisy(n: int, seed: int = 1) -> list[int]:
    rng = random.Random(seed)
    return [int(rng.gauss(0, 3000)) for _ in range(n)]


# ---- SPU2 ADPCM ---------------------------------------------------------------------------------------
def check_vag() -> None:
    rate = 22050
    sig = [a + b + c for a, b, c in zip(sine(28 * 300, 440, 9000, rate), sine(28 * 300, 3100, 3000, rate), noisy(28 * 300))]
    blocks = vag.encode(sig)
    assert len(blocks) == vag.encoded_size(len(sig)) == 300 * 16 and not vag.check_blocks(blocks)
    dec = vag.decode(blocks)
    assert len(dec) == len(sig)
    snr = vag.snr_db(sig, dec)
    assert snr > 20, snr
    # the fast search loses little against the exhaustive one
    snr_x = vag.snr_db(sig, vag.decode(vag.encode(sig, exhaustive=True)))
    assert snr_x >= snr and snr_x - snr < 0.6, (snr, snr_x)
    # flags: one-shot 0 ... 1, first block filter 0
    assert blocks[1] == 0 and blocks[-15] == vag.FLAG_END and blocks[0] >> 4 == 0
    # a loop: start flag, end+repeat flag, and the second pass decodes exactly like the first (block 0 ignores history)
    loop = vag.encode(sig, loop=True)
    assert loop[1] == vag.FLAG_START and loop[-15] == vag.FLAG_END | vag.FLAG_REPEAT
    two = vag.decode(loop, passes=2)
    assert len(two) == 2 * len(sig) and two[:len(sig)] == two[len(sig):]
    assert vag.decode(loop, passes=1) == dec[:len(sig)] or vag.snr_db(sig, vag.decode(loop)) > 20
    # silence is exact, a full-scale square wave never leaves int16
    z = vag.encode([0] * 100)
    assert vag.decode(z) == [0] * 112 and len(z) == 4 * 16
    sq = [32767 if (i // 20) % 2 else -32768 for i in range(28 * 40)]
    d = vag.decode(vag.encode(sq))
    assert all(-32768 <= v <= 32767 for v in d) and vag.snr_db(sq, d) > 15
    # partial last block is zero padded, a one-block sample works
    assert len(vag.encode([100] * 5)) == 16 and vag.decode(vag.encode([100] * 5))[:5] != [0] * 5
    # every filter and shift the encoder can emit decodes without error; every block header is valid
    used = {(b[0] >> 4, b[0] & 15) for b in (blocks[i:i + 16] for i in range(0, len(blocks), 16))}
    assert all(f <= 4 and s <= 12 for f, s in used) and len({f for f, _ in used}) >= 3
    # decoder arithmetic on a hand-made block: filter 0, shift 12, nibbles 1, -1, 7, -8 => 1, -1, 7, -8 (step 1)
    blk = bytearray(16)
    blk[0] = 0x0C
    blk[2] = 0x1 | (0xF << 4)
    blk[3] = 0x7 | (0x8 << 4)
    out, s1, s2 = vag.decode_block(bytes(blk))
    assert out[:4] == [1, -1, 7, -8] and out[4:] == [0] * 24 and (s1, s2) == (0, 0)
    # filter 1 (60/64): a step of 4096 >> 4 = 256 then the prediction decays: (60 * 256 + 32) >> 6 = 240
    blk = bytearray(16)
    blk[0] = 0x10 | 4
    blk[2] = 0x1
    out, _, _ = vag.decode_block(bytes(blk))
    assert out[0] == 256 and out[1] == 240
    # malformed streams
    try:
        vag.decode_block(bytes([0x50]) + bytes(15))
        raise AssertionError("filter 5 accepted")
    except vag.VagError:
        pass
    assert vag.check_blocks(bytes([0x50]) + bytes(15)) and vag.check_blocks(b"\0" * 15) and vag.check_blocks(b"")
    bad = bytearray(blocks)
    bad[16 + 1] = vag.FLAG_END
    assert any("end flag" in p for p in vag.check_blocks(bytes(bad)))
    # VAGp file wrapper and wav writer
    v = vag.vag_file(blocks, 22050, "test")
    assert v[:4] == b"VAGp" and struct.unpack_from(">I", v, 12)[0] == len(blocks) and struct.unpack_from(">I", v, 16)[0] == 22050
    with tempfile.TemporaryDirectory() as td:
        w = Path(td) / "a.wav"
        vag.write_wav(w, dec, 22050)
        back, r = vag.read_wav(w)
        assert back == dec and r == 22050
        f = Path(td) / "a.vag"
        f.write_bytes(v)
        assert vag.main(["wav", str(f), str(Path(td) / "b.wav")]) == 0
        assert vag.read_wav(Path(td) / "b.wav")[0] == dec


# ---- Xbox ADPCM ---------------------------------------------------------------------------------------
def check_xbox_adpcm() -> None:
    n = 64 * 50
    a = [x + y // 10 for x, y in zip(sine(n, 523, 12000, 22050), noisy(n, 3))]
    b = sine(n, 1700, 9000, 22050, 1.0)
    mono = sc.xbox_adpcm_encode([a])
    assert len(mono) == 36 * 50
    dec = sc.xbox_adpcm_decode(mono, 1)[0]
    assert len(dec) == n
    assert vag.snr_db(a, dec) > 25
    assert all(dec[64 * k] == a[64 * k] for k in range(50))                # header predictor = sample 0, exact
    assert all(mono[36 * k + 35] >> 4 == 0 for k in range(50))             # the 64th nibble is padding
    st = sc.xbox_adpcm_encode([a, b])
    assert len(st) == 72 * 50
    left, right = sc.xbox_adpcm_decode(st, 2)
    assert vag.snr_db(a, left) > 25 and vag.snr_db(b, right) > 18
    assert left == dec                                                      # L of a stereo stream = the mono stream
    # the step index in a block header is the index reached after exactly 63 nibbles of the previous block
    assert sc.index_continuity(mono, 1) == (49, 49) and sc.index_continuity(st, 2) == (98, 98)
    shifted = bytearray(mono)                                               # damaged headers no longer match
    for k in range(1, 50, 2):
        shifted[36 * k + 2] = (shifted[36 * k + 2] + 7) % 80
    assert sc.index_continuity(bytes(shifted), 1)[0] < 15
    bad = bytearray(mono)
    bad[2] = 200
    for data, ch in ((bytes(bad), 1), (mono[:-1], 1), (b"", 1), (mono, 3), (mono[:36 * 3 + 5], 2)):
        try:
            sc.xbox_adpcm_decode(data, ch)
            raise AssertionError("bad Xbox ADPCM accepted")
        except sc.SoundError:
            pass
    assert 0.2 < sc.boundary_ratio(mono) < 3.0                              # self-consistent encoder output
    # clipping behaviour: a full-scale signal stays inside int16
    big = [32767 if (i // 9) % 2 else -32768 for i in range(64 * 8)]
    assert all(-32768 <= v <= 32767 for v in sc.xbox_adpcm_decode(sc.xbox_adpcm_encode([big]), 1)[0])


# ---- resampling -----------------------------------------------------------------------------------------
def rms(x: list[int]) -> float:
    return math.sqrt(sum(v * v for v in x) / max(1, len(x)))


def check_resample() -> None:
    rate = 44100
    n = 4096
    lo = sine(n, 1000, 10000, rate)
    d = sc.decimate(lo, 2)
    assert len(d) == n // 2 and len(sc.decimate(lo[:101], 2)) == 51
    mid = d[100:-100]
    ref = sine(n // 2, 1000, 10000, rate // 2)
    assert abs(rms(mid) / rms(ref[100:-100]) - 1) < 0.01                    # passband gain 1 within 0.09 dB
    # no time shift: the decimated tone lines up with the tone sampled at the output rate
    assert max(abs(a - b) for a, b in zip(d[100:-100], ref[100:-100])) < 200
    hi = sine(n, 15000, 10000, rate)                                        # above the output Nyquist (11025 Hz)
    assert rms(sc.decimate(hi, 2)[100:-100]) < rms(hi) * 0.01               # > 40 dB down
    assert sc.decimate([5000] * 500, 2)[50:-50] == [5000] * 150             # unity DC gain
    assert sc.decimate(lo, 1) == lo and sc.decimate(lo, 4)[10] is not None and len(sc.decimate(lo, 4)) == n // 4
    # periodic mode: the wrap-around is seamless (decimating a tiled signal gives the same samples in the middle)
    per = sine(512, 22050 * 8 / 512 * 0.1, 8000, 22050)                     # 8 periods in 512 samples
    per = [int(round(8000 * math.sin(2 * math.pi * 8 * i / 512))) for i in range(512)]
    tiled = sc.decimate(per * 3, 2)
    assert sc.decimate(per, 2, periodic=True) == tiled[256:512]
    assert sc.decimate(per, 2)[0:3] != tiled[256:259]                       # non periodic: fades in from zero
    # loop shrink keeps frequency (in cycles per loop), amplitude and seam
    sh = sc.periodic_shrink(per, 500)
    assert len(sh) == 500
    cyc = sum(1 for i in range(500) if sh[i - 1] < 0 <= sh[i])
    assert cyc == 8 and abs(rms(sh) / rms(per) - 1) < 0.01
    assert max(abs(sh[i] - sh[i - 1]) for i in range(500)) < 8000 * 2 * math.pi * 8 / 500 * 1.1   # no step at the seam
    assert sc.periodic_shrink(per, 512) == per
    for bad in (0, 513):
        try:
            sc.periodic_shrink(per, bad)
            raise AssertionError("bad shrink accepted")
        except sc.SoundError:
            pass
    # frame planning
    assert sc.plan_frames(1000, 1, False) == 1000 and sc.plan_frames(1000, 2, False) == 500
    assert sc.plan_frames(2560, 1, True) == 2548 and sc.plan_frames(100, 1, True) == 84 and sc.plan_frames(40, 1, True) == 40
    pcm, ratio = sc.convert_pcm(per * 5, 1, True)
    assert len(pcm) == 2548 // 28 * 28 and 0.99 < ratio < 1.0
    pcm, ratio = sc.convert_pcm(per, 2, False)
    assert len(pcm) == 256 and ratio == 1.0


# ---- audsrv wrapper -------------------------------------------------------------------------------------
def check_audsrv() -> None:
    pcm = [int(9000 * math.sin(2 * math.pi * 440 * i / 22050) + 2000 * math.sin(2 * math.pi * 2300 * i / 22050)) for i in range(2800)]
    blocks = vag.encode(pcm)
    one = sc.wrap_audsrv(blocks, 2800, 22050, False)
    # the 16 header bytes ps2sdk's adpenc writes for a 22050 Hz mono one-shot / loop of 2800 samples (compared on the real tool)
    assert one[:16] == bytes.fromhex("4150434d0101000059070000f00a0000") and len(one) == 16 + 100 * 16 + 16
    lp = sc.wrap_audsrv(blocks, 2800, 22050, True)
    assert lp[:16] == bytes.fromhex("4150434d0101010059070000f00a0000")
    assert sc.audsrv_pitch(22050) == 0x759 == 1881 and sc.audsrv_pitch(11025) == 940 and sc.audsrv_pitch(48000) == 4096
    for w, loop in ((one, False), (lp, True)):
        h, bl = sc.unwrap_audsrv(w)
        assert h["loop"] == loop and h["frames"] == 2800 and len(bl) == 1600
        assert w[-16:] == bytes([bl[-16], 7]) + bytes(14)                  # terminator: last predictor byte, flags 7, zero nibbles
        assert [bl[i * 16 + 1] for i in range(100)] == ([6] + [0] * 98 + [3] if loop else [0] * 99 + [1])
        assert [bl[i * 16] for i in range(100)] == [blocks[i * 16] for i in range(100)]     # nibbles untouched
    assert vag.decode(sc.unwrap_audsrv(lp)[1], passes=2)[:2800] == vag.decode(sc.unwrap_audsrv(lp)[1], passes=2)[2800:]
    single = sc.wrap_audsrv(vag.encode(pcm[:20]), 20, 22050, True)
    assert sc.unwrap_audsrv(single)[1][1] == 7
    for bad in (one[:-16], one[:20], b"XPCM" + one[4:], one[:4] + b"\x02" + one[5:], one[:-15] + b"\x00" + one[-14:],
                one[:-1] + b"\x01", lp[:16 + 1] + b"\x00" + lp[18:], one[:6] + b"\x01" + one[7:]):
        try:
            sc.unwrap_audsrv(bad)
            raise AssertionError("bad audsrv sample accepted")
        except sc.SoundError:
            pass
    try:
        sc.wrap_audsrv(b"\0" * 17, 28, 22050, False)
        raise AssertionError("ragged blocks accepted")
    except sc.SoundError:
        pass


# ---- statistics -----------------------------------------------------------------------------------------
def check_stats() -> None:
    st = sc.pcm_stats(sine(22050, 440, 10000, 22050), 22050)
    assert abs(st["seconds"] - 1.0) < 1e-9 and abs(st["peak"] - 10000) <= 1 and abs(st["rms"] - 7071) < 5
    assert st["dc_ratio"] < 0.01 and st["clip_fraction"] == 0 and st["hf_ratio"] < 0.01 and not sc.plausibility(st)
    clip = sc.pcm_stats([32767, -32768] * 100, 22050)
    assert clip["clip_fraction"] == 1.0 and sc.plausibility(clip)
    dc = sc.pcm_stats([20000 + v for v in noisy(2000)], 22050)
    assert any("DC" in p for p in sc.plausibility(dc))
    white = sc.pcm_stats(noisy(5000), 22050)
    assert 0.8 < white["hf_ratio"] < 1.2 and not sc.plausibility(white)    # noise alone is not a failure
    assert sc.plausibility(dict(sc.pcm_stats(noisy(100), 22050), boundary=9.0))
    assert sc.pcm_stats([], 22050) == {"frames": 0} and sc.pcm_stats([0] * 10, 22050)["silent"]


# ---- a synthetic map --------------------------------------------------------------------------------------
class World:
    """Tag region + map file image: tags with zero-filled blocks, sound structs written the way the Xbox compiler lays them
    out, sample data in a file area in front of the tag region."""

    HEADER = 2048
    DATA_AREA = 0x20000                  # the sample area between the cache header and the tag region

    def __init__(self, tags: list[tuple[str, str, int]]):
        self.region = bytearray(tagindex.TagIndex.build(tags, scenario_index=0))
        self.index = tagindex.TagIndex.parse_region(bytes(self.region), self.HEADER + self.DATA_AREA)
        self.by_path = {(e.group, e.path): e for e in self.index.entries}
        self.cursor: dict[tuple, int] = {}
        self.samples = bytearray()
        self.pcm: dict[str, list[list[int]]] = {}

    def e(self, group: str, path: str) -> tagindex.TagEntry:
        return self.by_path[(group, path)]

    def ref(self, group: str, path: str | None) -> bytes:
        if path is None:
            return struct.pack("<4sIII", group[::-1].encode(), 0, 0, NONE)
        t = self.e(group, path)
        return struct.pack("<4sIII", group[::-1].encode(), t.path_address, 0, t.tag_id)

    def put_ref(self, owner: tagindex.TagEntry, at: int, group: str, path: str | None) -> None:
        o = owner.data_address - BASE + at
        self.region[o:o + 16] = self.ref(group, path)

    def alloc(self, owner: tagindex.TagEntry, data: bytes) -> int:
        cur = (self.cursor.get(owner.tag_id, 0x400) + 3) & ~3
        off = owner.data_address - BASE + cur
        self.region[off:off + len(data)] = data
        self.cursor[owner.tag_id] = cur + len(data)
        return BASE + off

    def add_sound(self, path: str, klass: int, rate_code: int, channels: int, perms: list, *, ranges: list[int] | None = None,
                  minmax=(0.0, 0.0), pitch=(1.0, 1.0), promotion: str | None = None, flags: int = 0) -> None:
        """perms: (name, [channel sample lists], skip, gain, next index); ranges: perm count per pitch range."""
        e = self.e("snd!", path)
        b = e.data_address - BASE
        ranges = ranges or [len(perms)]
        rate = 22050 if rate_code == 0 else 44100
        maxlen = 0
        built = []
        for name, chans, skip, gain, nxt in perms:
            data = sc.xbox_adpcm_encode(chans)
            foff = self.HEADER + len(self.samples)
            self.samples += data
            self.samples += bytes(-len(self.samples) % 2048)
            built.append((name, foff, len(data), skip, gain, nxt))
            maxlen = max(maxlen, len(data))
            self.pcm[f"{path}/{name}"] = chans
        struct.pack_into("<IHH", self.region, b, flags, klass, rate_code)
        struct.pack_into("<9f", self.region, b + 8, minmax[0], minmax[1], 0.0, pitch[0], pitch[1], 6.2831855, 6.2831855, 1.0, 0.85)
        struct.pack_into("<3f", self.region, b + 0x3C, 1.0, 0.0, 1.0)
        struct.pack_into("<3f", self.region, b + 0x54, 1.0, 1.0, 1.0)
        struct.pack_into("<HH", self.region, b + 0x6C, channels - 1, 1)
        self.region[b + 0x70:b + 0x80] = self.ref("snd!", promotion)
        struct.pack_into("<HxxI", self.region, b + 0x80, 5 if promotion else 0, maxlen * 2 * 1000 // (rate * channels))
        prs = bytearray()
        pos = 0
        for ri, count in enumerate(ranges):
            plist = bytearray()
            for name, foff, size, skip, gain, nxt in built[pos:pos + count]:
                q = bytearray(124)
                q[:len(name)] = name.encode()
                struct.pack_into("<2f", q, 0x20, skip, gain)
                struct.pack_into("<HH", q, 0x28, 1, nxt)
                struct.pack_into("<I", q, 0x34, e.tag_id)
                struct.pack_into("<I", q, 0x3C, e.tag_id)
                struct.pack_into("<III", q, 0x40, size, 0, foff)
                plist += q
            paddr = self.alloc(e, bytes(plist)) if count else 0
            pr = bytearray(72)
            pname = b"default" if ri == 0 else b"range%d" % ri
            pr[:len(pname)] = pname
            struct.pack_into("<3fH", pr, 0x20, 1.0 + ri * 0.5, 0.0, 1.0, count)
            struct.pack_into("<III", pr, 0x3C, count, paddr, 0)
            prs += pr
            pos += count
        struct.pack_into("<III", self.region, b + 0x98, len(ranges), self.alloc(e, bytes(prs)), 0)

    def add_loop(self, path: str, flags: int, tracks: list[tuple], details: list[tuple]) -> None:
        """tracks: (flags, gain, fade in, fade out, [start, loop, end, alt loop, alt end] paths or None);
        details: (path, period lo, period hi, gain, flags, yaw, pitch, dist)."""
        e = self.e("lsnd", path)
        b = e.data_address - BASE
        struct.pack_into("<I", self.region, b, flags)
        struct.pack_into("<f", self.region, b + 0x20, 5.0)
        self.region[b + 0x2C:b + 0x3C] = struct.pack("<4sIII", b"gmdc", 0, 0, NONE)
        struct.pack_into("<III", self.region, b + 0x3C, len(tracks), BASE + b + sc.LSND_MAIN, 0)
        struct.pack_into("<III", self.region, b + 0x48, len(details),
                         BASE + b + sc.LSND_MAIN + len(tracks) * sc.TRACK_STRIDE_SRC, 0)
        o = b + sc.LSND_MAIN
        for fl, gain, fin, fout, refs in tracks:
            struct.pack_into("<Ifff", self.region, o, fl, gain, fin, fout)
            for k, off in enumerate((0x30, 0x40, 0x50, 0x80, 0x90)):
                self.region[o + off:o + off + 16] = self.ref("snd!", refs[k])
            o += sc.TRACK_STRIDE_SRC
        for sp, plo, phi, gain, fl, yaw, pitch, dist in details:
            self.region[o:o + 16] = self.ref("snd!", sp)
            struct.pack_into("<3fI", self.region, o + 0x10, plo, phi, gain, fl)
            struct.pack_into("<6f", self.region, o + 0x50, *yaw, *pitch, *dist)
            o += sc.DETAIL_STRIDE_SRC

    def map(self, directory: Path) -> texconv.MapData:
        """Write a decompressed cache file (valid header) and open it the way the converter does."""
        path = directory / "synthetic.map"
        assert len(self.samples) <= self.DATA_AREA
        body = bytes(self.samples) + bytes(self.DATA_AREA - len(self.samples)) + bytes(self.region)
        hdr = bytearray(self.HEADER)
        struct.pack_into("<6I", hdr, 0, 0x68656164, 5, self.HEADER + len(body), 0, self.HEADER + self.DATA_AREA, len(self.region))
        hdr[0x20:0x28] = b"synthetc"
        struct.pack_into("<I", hdr, 0x7FC, 0x666F6F74)
        path.write_bytes(bytes(hdr) + body)
        return texconv.MapData.open(path)


LOOP_SIZE = sc.LSND_MAIN + sc.TRACK_STRIDE_SRC + sc.DETAIL_STRIDE_SRC
TAGS = [
    ("scnr", "levels\\t\\t", 0x400), ("matg", "globals\\globals", 0x2000), ("weap", "weapons\\rifle\\rifle", 0x2000),
    ("effe", "effects\\rifle\\fire", 0x2000), ("bipd", "characters\\cyborg\\cyborg", 0x2000),
    ("antr", "characters\\cyborg\\cyborg", 0x2000), ("lsnd", "sound\\sfx\\ambience\\hum", LOOP_SIZE),
    ("scen", "scenery\\speaker", 0x2000), ("mode", "scenery\\speaker", 0x2000),
    ("snd!", "sound\\sfx\\weapons\\rifle\\fire", 0x2000), ("snd!", "sound\\sfx\\weapons\\rifle\\reload", 0x2000),
    ("snd!", "sound\\sfx\\ambience\\hum_loop", 0x2000), ("snd!", "sound\\sfx\\ambience\\hum_detail", 0x2000),
    ("snd!", "sound\\sfx\\footsteps\\walk", 0x2000), ("snd!", "sound\\sfx\\animations\\roll", 0x2000),
    ("snd!", "sound\\dialog\\chief\\hello", 0x2000), ("snd!", "sound\\sfx\\unused\\never", 0x2000),
    ("snd!", "sound\\sfx\\weapons\\rifle\\fire_far", 0x2000),
]
F, RELOAD, HUM, DETAIL, STEP, ROLL, DIALOG, NEVER, FAR = (t[1] for t in TAGS[9:])
N64 = 64


def build_synthetic(directory: Path) -> tuple[World, texconv.MapData]:
    w = World(TAGS)
    w.add_sound(F, 4, 0, 1, [("shot1", [sine(N64 * 20, 440, 9000, 22050)], 0.0, 1.0, 1),
                             ("shot2", [sine(N64 * 30, 660, 7000, 22050)], 0.25, 0.5, 0xFFFF),
                             ("shot3", [sine(N64 * 25, 880, 8000, 22050)], 0.0, 0.75, 0xFFFF)],
                promotion=FAR, minmax=(1.5, 12.0), pitch=(0.9, 1.1))
    w.add_sound(FAR, 4, 0, 1, [("far", [sine(N64 * 12, 300, 6000, 22050)], 0.0, 1.0, 0xFFFF)])
    w.add_sound(RELOAD, 6, 0, 1, [("r", [[(v * 3) // 4 for v in sine(N64 * 40, 250, 12000, 22050)]], 0.0, 1.0, 0xFFFF)])
    w.add_sound(HUM, 28, 0, 1, [("loop1", [sine(N64 * 40, 22050 / 128 * 4, 6000, 22050)], 0.0, 1.0, 0xFFFF)])   # 2560 frames
    w.add_sound(DETAIL, 14, 0, 1, [("d", [noisy(N64 * 10, 5)], 0.0, 1.0, 0xFFFF)])
    w.add_sound(STEP, 18, 1, 2, [("a", [sine(N64 * 30, 1000, 9000, 44100), sine(N64 * 30, 1000, 9000, 44100, 0.3)], 0.0, 1.0, 0xFFFF),
                                 ("b", [sine(N64 * 30, 1200, 9000, 44100), sine(N64 * 30, 1200, 9000, 44100)], 0.0, 1.0, 0xFFFF)])
    w.add_sound(ROLL, 13, 0, 1, [("p", [noisy(N64 * 16, 7)], 0.0, 1.0, 0xFFFF), ("q", [noisy(N64 * 8, 8)], 0.0, 1.0, 0xFFFF),
                                 ("r", [noisy(N64 * 8, 9)], 0.0, 1.0, 0xFFFF)], ranges=[2, 1])
    w.add_sound(DIALOG, 19, 0, 1, [("line", [sine(N64 * 100, 300, 5000, 22050)], 0.0, 1.0, 0xFFFF)])
    w.add_sound(NEVER, 13, 0, 1, [("n", [noisy(N64 * 4, 11)], 0.0, 1.0, 0xFFFF)])
    w.add_loop("sound\\sfx\\ambience\\hum", 1, [(3, 1.0, 0.1, 0.2, [None, HUM, None, None, None])],
               [(DETAIL, 0.5, 3.0, 0.8, 0, (-3.14, 3.14), (-1.5, 1.5), (0.0, 20.0))])
    # references: weapon -> effect -> rifle sounds, weapon -> loop; globals -> footsteps; biped -> animations -> roll, dialog
    w.put_ref(w.e("weap", "weapons\\rifle\\rifle"), 0x100, "effe", "effects\\rifle\\fire")
    w.put_ref(w.e("weap", "weapons\\rifle\\rifle"), 0x110, "lsnd", "sound\\sfx\\ambience\\hum")
    w.put_ref(w.e("weap", "weapons\\rifle\\rifle"), 0x120, "snd!", RELOAD)
    w.put_ref(w.e("effe", "effects\\rifle\\fire"), 0x200, "snd!", F)
    w.put_ref(w.e("matg", "globals\\globals"), 0x300, "snd!", STEP)
    w.put_ref(w.e("bipd", "characters\\cyborg\\cyborg"), 0x80, "antr", "characters\\cyborg\\cyborg")
    w.put_ref(w.e("antr", "characters\\cyborg\\cyborg"), 0x500, "snd!", ROLL)
    w.put_ref(w.e("antr", "characters\\cyborg\\cyborg"), 0x510, "snd!", DIALOG)
    w.put_ref(w.e("scen", "scenery\\speaker"), 0x40, "snd!", NEVER)        # a group the traversal does not enter
    # a reference whose path pointer is wrong must not be followed, nor one with a mismatching group
    bad = bytearray(w.ref("snd!", NEVER))
    struct.pack_into("<I", bad, 4, 0x12345678)
    o = w.e("weap", "weapons\\rifle\\rifle").data_address - BASE + 0x130
    w.region[o:o + 16] = bad
    bad2 = bytearray(w.ref("snd!", NEVER))
    bad2[:4] = b"rtna"
    w.region[o + 16:o + 32] = bad2
    return w, w.map(directory)


# ---- tests on the synthetic map -------------------------------------------------------------------------
def check_parsing(m: texconv.MapData) -> None:
    t = sc.Tags(m)
    s = sc.read_sound(t, next(e for e in m.index.entries if e.path == F))
    assert s.klass == 4 and s.rate == 22050 and s.channels == 1 and s.fmt == 1 and s.min_dist == 1.5 and s.max_dist == 12.0
    assert (round(s.pitch_lo, 3), round(s.pitch_hi, 3)) == (0.9, 1.1) and s.promotion_count == 5
    assert s.promotion_tag == next(e.tag_id for e in m.index.entries if e.path == FAR)
    assert [len(r.perms) for r in s.ranges] == [1 + 2] and [p.next_index for p in s.perms] == [1, 0xFFFF, 0xFFFF]
    assert s.perms[1].skip == 0.25 and s.perms[1].gain == 0.5 and s.frames(s.perms[0]) == 1280
    assert s.max_play_ms == s.perms[1].size * 2 * 1000 // 22050
    st = sc.read_sound(t, next(e for e in m.index.entries if e.path == STEP))
    assert st.rate == 44100 and st.channels == 2 and st.frames(st.perms[0]) == 1920 and st.block_bytes == 72
    roll = sc.read_sound(t, next(e for e in m.index.entries if e.path == ROLL))
    assert [len(r.perms) for r in roll.ranges] == [2, 1] and roll.ranges[1].natural_pitch == 1.5
    lp = sc.read_loop(t, next(e for e in m.index.entries if e.group == "lsnd"))
    assert len(lp.tracks) == 1 and len(lp.details) == 1 and lp.tracks[0].start == NONE and lp.tracks[0].end == NONE
    assert lp.tracks[0].loop == next(e.tag_id for e in m.index.entries if e.path == HUM) and abs(lp.tracks[0].fade_out - 0.2) < 1e-6
    assert lp.details[0].period_hi == 3.0 and lp.details[0].dist == (0.0, 20.0) and len(lp.sound_ids()) == 2
    # a structurally wrong tag is rejected
    for patch, what in ((lambda r, b: struct.pack_into("<H", r, b + 6, 2), "rate code"),
                        (lambda r, b: struct.pack_into("<H", r, b + 0x6C, 5), "channel code"),
                        (lambda r, b: struct.pack_into("<I", r, b + 0x98, 5000), "reflexive count")):
        e = next(e for e in m.index.entries if e.path == FAR)
        region = bytearray(m.region)
        patch(region, e.data_address - BASE)
        mm = texconv.MapData(m.path, m.header, m.index, bytes(region), m.by_id)
        try:
            sc.read_sound(sc.Tags(mm), e)
            raise AssertionError(f"{what} accepted")
        except sc.SoundError:
            pass
    check_all_problems = sc.check_all(m)
    assert check_all_problems == []


def check_reachability(m: texconv.MapData) -> None:
    t = sc.Tags(m)
    ids = {e.path: e.tag_id for e in m.index.entries if e.group == "snd!"}
    reach = sc.closure(t, sc.default_roots(m))
    got = {e.path for e in reach.values() if e.group == "snd!"}
    assert got == {F, FAR, RELOAD, HUM, DETAIL, STEP, ROLL, DIALOG}, got        # not NEVER (scen is a leaf)
    assert NEVER not in got
    # explicit roots: a sound id, a path, a non-traversed group entered because it was named
    r = sc.resolve_roots(m, [hex(ids[STEP]), "scenery\\speaker"])
    assert {e.path for e in sc.closure(t, r).values() if e.group == "snd!"} == {STEP, NEVER}
    assert {e.path for e in sc.closure(t, sc.resolve_roots(m, [F])).values() if e.group == "snd!"} == {F, FAR}
    try:
        sc.resolve_roots(m, ["no\\such\\tag"])
        raise AssertionError("unknown root accepted")
    except sc.SoundError:
        pass
    origin = sc.root_origin(t)
    assert origin[ids[F]] == "weap" and origin[ids[STEP]] == "matg" and origin[ids[ROLL]] == "bipd"


def sample_blocks(b: sc.Built, p: dict) -> bytes:
    """The ADPCM blocks of a permutation (audsrv header and terminator checked and removed)."""
    return sc.unwrap_audsrv(b.sndd[p["offset"]:p["offset"] + p["size"]])[1]


def decoded(b: sc.Built, sec: sc.Section, snd: dict, k: int) -> list[int]:
    return vag.decode(sample_blocks(b, sec.perms[snd["perm_first"] + k]))


def check_conversion(w: World, m: texconv.MapData, td: Path) -> bytes:
    b = sc.build_sections(m, cache=sc.Cache(td / "cache"))
    sec = sc.parse_section(b.snd0, len(b.sndd))
    by_id = {e.path: e.tag_id for e in m.index.entries if e.group == "snd!"}
    sounds = {full: next(s for s in sec.sounds if s["tag_id"] == by_id[full]) for full in by_id if full != NEVER}
    assert len(sec.sounds) == 8 and by_id[NEVER] not in {s["tag_id"] for s in sec.sounds}
    assert [s["tag_id"] for s in sec.sounds] == sorted(s["tag_id"] for s in sec.sounds)
    assert len(b.sndd) % 64 == 0 and b.spu_bytes <= sc.SPU_BUDGET and sec.budget == sc.SPU_BUDGET
    assert b.spu_bytes == sum(p["size"] - 16 for p in sec.perms) and len(b.sndd) >= sum(p["size"] for p in sec.perms)
    # streamed dialogue: recorded, no data
    d = sounds[DIALOG]
    assert d["flags"] == sc.F_STREAMED and d["perm_count"] == 0 and d["range_count"] == 0 and sec.counts["streamed"] == 1
    # weapon sound: three permutations, chain and gain preserved, original sample counts, rate 22050
    f = sounds[F]
    assert f["klass"] == 4 and f["flags"] == 0 and f["perm_count"] == 3 and f["rate"] == 22050 and f["range_count"] == 1
    assert (f["min_dist"], f["max_dist"]) == (1.5, 12.0) and f["promotion_tag"] == by_id[FAR] and f["promotion_count"] == 5
    assert f["name"] == "sound\\sfx\\weapons\\rifle\\fire"
    perms = [sec.perms[f["perm_first"] + k] for k in range(3)]
    assert [p["frames"] for p in perms] == [1280, 1920, 1600] and [p["size"] for p in perms] == [46 * 16 + 32, 69 * 16 + 32, 58 * 16 + 32]
    assert [p["next"] for p in perms] == [1, 0xFFFF, 0xFFFF] and [p["gain"] for p in perms] == [1.0, 0.5, 0.75]
    assert perms[1]["skip"] == 0.25 and all(p["rate"] == 22050 and p["flags"] == 0 for p in perms)
    assert [p["name"] for p in perms] == ["shot1", "shot2", "shot3"] and perms[0]["src_bytes"] == 20 * 36
    # the SPU2 data decodes to what the source plays: SNR against the decimated source, exact length
    for k, p in enumerate(perms):
        hdr, blk = sc.unwrap_audsrv(b.sndd[p["offset"]:p["offset"] + p["size"]])
        assert hdr == {"loop": False, "pitch": 1881, "frames": p["frames"]} and p["offset"] % 16 == 0
        src = vag.decode(blk)
        ref = sc.xbox_adpcm_decode(sc.xbox_adpcm_encode(w.pcm[f"{F}/{p['name']}"]), 1)[0]
        assert len(src) == (p["size"] // 16 - 2) * 28 and vag.snr_db(ref, src[:len(ref)]) > 24, vag.snr_db(ref, src[:len(ref)])
        assert not vag.check_blocks(blk)
    # stereo 44100 -> mono 22050, folded flag, half the frames, SNR against the fold
    s = sounds[STEP]
    sp = [sec.perms[s["perm_first"] + k] for k in range(2)]
    assert s["rate"] == 22050 and all(p["frames"] == 960 and p["flags"] == sc.P_FOLDED and p["rate"] == 22050 for p in sp)
    chans = w.pcm[f"{STEP}/b"]
    ref = sc.decimate(sc.fold_mono([list(c) for c in sc.xbox_adpcm_decode(sc.xbox_adpcm_encode(chans), 2)]), 2)
    assert vag.snr_db(ref, decoded(b, sec, s, 1)[:960]) > 24
    # the looping sound: loop flag, multiple of 28 frames, lowered rate, flags in the blocks, seamless second pass
    h = sounds[HUM]
    hp = sec.perms[h["perm_first"]]
    assert h["flags"] == sc.F_LOOP and hp["flags"] == sc.P_LOOP and hp["frames"] == 2548 and hp["frames"] % 28 == 0
    assert hp["loop_start"] == 0 and hp["loop_end"] == 91 * 16 and hp["size"] == 91 * 16 + 32
    assert hp["rate"] == round(22050 * 2548 / 2560) < 22050
    hh, blob = sc.unwrap_audsrv(b.sndd[hp["offset"]:hp["offset"] + hp["size"]])
    assert hh["loop"] and hh["pitch"] == 21947 * 4096 // 48000
    assert blob[1] == 6 and blob[-15] == vag.FLAG_END | vag.FLAG_REPEAT
    two = vag.decode(blob, passes=2)
    assert two[:2548] == two[2548:]
    assert max(abs(two[2548 + i] - two[2547 + i]) for i in range(1, 6)) < 2000      # no click at the seam
    assert max(abs(two[2548] - two[2547]), abs(two[0] - two[2547])) < 2500
    # pitch ranges: 2 + 1 permutations in two ranges, consecutive runs, range index per permutation
    r = sounds[ROLL]
    assert r["range_count"] == 2 and r["perm_count"] == 3
    rr = [sec.ranges[r["range_first"] + i] for i in range(2)]
    assert [(x["perm_first"], x["perm_count"]) for x in rr] == [(0, 2), (2, 1)] and rr[1]["natural_pitch"] == 1.5
    assert [sec.perms[r["perm_first"] + k]["range"] for k in range(3)] == [0, 0, 1]
    # the sound_looping table: one loop, one track (loop = the hum), one detail sound
    assert len(sec.loops) == 1 and len(sec.tracks) == 1 and len(sec.details) == 1
    lp = sec.loops[0]
    assert lp["flags"] == 1 and lp["param"] == 5.0 and lp["track_count"] == 1 and lp["detail_count"] == 1
    tr = sec.tracks[0]
    assert tr["loop"] == by_id[HUM] and tr["start"] == tr["end"] == tr["alt_loop"] == tr["alt_end"] == NONE
    assert abs(tr["fade_in"] - 0.1) < 1e-6 and tr["flags"] == 3
    de = sec.details[0]
    assert de["sound"] == by_id[DETAIL] and de["period_lo"] == 0.5 and de["period_hi"] == 3.0 and de["dist"] == (0.0, 20.0)
    # every permutation block stream is valid, and (rates <= 22050) the contract holds
    assert all(p["rate"] <= 22050 for p in sec.perms)
    assert sec.counts == dict(dropped_sounds=0, dropped_perms=0, degraded=0, streamed=1)
    assert b.origin[by_id[F]] == "weap" and len(b.stats) == len(sec.perms)
    # a second run through the cache gives identical bytes
    b2 = sc.build_sections(m, cache=sc.Cache(td / "cache"))
    assert (b2.snd0, b2.sndd) == (b.snd0, b.sndd)
    # --roots restricts to a closure
    one = sc.build_sections(m, roots=sc.resolve_roots(m, [STEP]))
    assert [s["name"] for s in sc.parse_section(one.snd0).sounds] == [STEP]
    # exhaustive search is available and not worse
    ex = sc.build_sections(m, roots=sc.resolve_roots(m, [RELOAD]), exhaustive=True)
    fast = sc.build_sections(m, roots=sc.resolve_roots(m, [RELOAD]))
    assert len(ex.sndd) == len(fast.sndd) and ex.stats[0]["snr"] >= fast.stats[0]["snr"]
    return hp2.build("synthetic", [("SND0", b.snd0), ("SNDD", b.sndd)])


def check_budget(w: World, m: texconv.MapData) -> None:
    full = sc.build_sections(m)
    total = full.spu_bytes
    ids = {e.path: e.tag_id for e in m.index.entries if e.group == "snd!"}
    # a budget that fits: nothing changes
    same = sc.build_sections(m, budget=total)
    assert same.sndd == full.sndd and same.plan.dropped_perms == 0 and not same.plan.dropped_sounds
    seen_sizes = []
    for budget in (total - 16, total * 3 // 4, total // 2, total // 4, 2048, 1024, 160, 0):
        b = sc.build_sections(m, budget=budget)
        sec = sc.parse_section(b.snd0, len(b.sndd))
        assert b.spu_bytes <= budget // 64 * 64 and len(b.sndd) % 64 == 0
        assert sum(p["size"] - 16 for p in sec.perms) == b.spu_bytes <= budget
        seen_sizes.append(b.spu_bytes)
        kept = {s["tag_id"] for s in sec.sounds if s["perm_count"]}
        # importance: with room for half of everything the weapon sounds are all there, and the least valuable sound
        # (an ambient hum, class weight 10) is never kept while another gameplay sound is dropped
        if budget >= total // 2:
            assert {ids[F], ids[RELOAD], ids[FAR]} <= kept
        if ids[HUM] in kept:
            assert kept >= {ids[x] for x in (F, RELOAD, FAR, DETAIL, STEP, ROLL)}
        for s in sec.sounds:
            if s["perm_count"] == 0 and not s["flags"] & sc.F_STREAMED:
                assert s["flags"] & sc.F_DROPPED
        assert sec.counts["dropped_sounds"] == len(b.plan.dropped_sounds)
    assert seen_sizes == sorted(seen_sizes, reverse=True)
    # budget zero: everything dropped, the table still lists every sound, flagged
    z = sc.parse_section(sc.build_sections(m, budget=0).snd0, 0)
    assert len(z.sounds) == 8 and all(s["perm_count"] == 0 for s in z.sounds) and len(z.perms) == 0
    assert sum(1 for s in z.sounds if s["flags"] & sc.F_DROPPED) == 7
    # rate degradation: without it the planner can only drop; with it a sound may end up at half rate
    half = sc.build_sections(m, budget=total * 3 // 4)
    nodeg = sc.build_sections(m, budget=total * 3 // 4, degrade=False)
    assert not nodeg.plan.degraded
    sp = sc.parse_section(half.snd0, len(half.sndd))
    for s in sp.sounds:
        if s["flags"] & sc.F_DEGRADED:
            assert s["rate"] == 11025
            for k in range(s["perm_count"]):     # one-shots play at exactly 11025 Hz, loops at slightly less (stretch to 28 frames)
                r = sp.perms[s["perm_first"] + k]["rate"]
                assert r == 11025 or (s["flags"] & sc.F_LOOP and 10500 < r < 11025)
    # perm dropping keeps the `next` chain consistent: drop shot1 (chained to shot2) by hand through the plan
    srcs = [sc.read_sound(sc.Tags(m), e) for e in m.index.entries if e.group == "snd!" and e.path == F]
    plan = sc.plan_budget(srcs, set(), budget=srcs[0].perms and 1500)
    assert plan.total <= 1500 and plan.sounds[0].kept
    sc_describe = sc.describe(half, "synthetic", None, drops=True)
    assert any("budget" in line for line in sc_describe)
    # planner determinism and monotonic loss: weights follow the class table
    assert sc.class_weight(4) > sc.class_weight(18) > sc.class_weight(13) > sc.class_weight(34) and sc.class_weight(99) == sc.DEFAULT_WEIGHT


def check_section_corruption(snd0: bytes, sndd_size: int) -> None:
    sc.parse_section(snd0, sndd_size)
    for cut in (0, 10, sc.HEADER_SIZE - 1, len(snd0) - 1):
        try:
            sc.parse_section(snd0[:cut], sndd_size)
            raise AssertionError("truncated section accepted")
        except (sc.SoundError, struct.error):
            pass
    sec = sc.parse_section(snd0, sndd_size)
    o_sounds, o_ranges, o_perms = struct.unpack_from("<3I", snd0, 0x28)
    bad = {"magic": (0, b"X"), "version": (4, b"\x02"), "size": (8, b"\x01"), "sndd size": (0x24, b"\x00\x00\x01")}
    for what, (off, val) in bad.items():
        c = bytearray(snd0)
        c[off:off + len(val)] = val
        try:
            sc.parse_section(bytes(c), sndd_size)
            raise AssertionError(what + " accepted")
        except sc.SoundError:
            pass
    # perm offset outside SNDD, perm size not a multiple of 16, loop bounds on a one-shot, next out of range
    for desc, at, fmt, val in (("offset", o_perms, "<I", sndd_size), ("size", o_perms + 4, "<I", 17), ("loop end", o_perms + 20, "<I", 16),
                               ("next", o_perms + 32, "<H", 7), ("rate", o_perms + 12, "<I", 0), ("range idx", o_perms + 36, "<H", 5),
                               ("sound order", o_sounds + sc.SOUND_REC, "<I", 0), ("NaN", o_sounds + 32, "<I", 0x7FC00000)):
        c = bytearray(snd0)
        struct.pack_into(fmt, c, at, val)
        try:
            sc.parse_section(bytes(c), sndd_size)
            raise AssertionError(desc + " accepted")
        except sc.SoundError:
            pass
    assert sec.sounds


# ---- real map (skipped without game data) ------------------------------------------------------------------
def check_real_maps() -> None:
    path = ROOT / "assets" / "decompressed" / "a10.map"
    if not path.exists():
        print("  (a10.map not present: real-map checks skipped)")
        return
    m = texconv.MapData.open(path)
    assert sc.check_all(m) == []
    t = sc.Tags(m)
    snds = [sc.read_sound(t, e) for e in m.index.entries if e.group == "snd!"]
    assert len(snds) == 1468
    fire = next(s for s in snds if s.klass == 4 and s.channels == 1)
    assert fire.fmt == 1 and fire.rate == 22050 and fire.perms
    # Xbox ADPCM verification numbers on real data: a mono and a stereo sound
    for s in (fire, next(s for s in snds if s.channels == 2)):
        data = sc._read_range(m, s.perms[0])
        assert 0.2 < sc.boundary_ratio(data, s.channels) < 3.0
        st = sc.pcm_stats(sc.fold_mono(sc.decode_source(s, data)), s.rate)
        assert not sc.plausibility(dict(st, boundary=1.0)) or st["clip_fraction"] <= 0.01
    # a conversion at a small budget: valid sections, every permutation decodes, budget and rate contracts hold
    with tempfile.TemporaryDirectory() as td:
        b = sc.build_sections(m, budget=250_000, cache=sc.Cache(Path(td)))
        sec = sc.parse_section(b.snd0, len(b.sndd))
        assert b.spu_bytes <= 250_000 and sec.perms and all(p["rate"] <= 22050 for p in sec.perms)
        for p in sec.perms:
            blk = sample_blocks(b, p)
            assert not vag.check_blocks(blk)
            assert len(vag.decode(blk)) >= p["frames"]
        kept_classes = {s["klass"] for s in sec.sounds if s["perm_count"]}
        assert 4 in kept_classes                                                # weapon fire survives a tight budget
        snrs = [s["snr"] for s in b.stats if not math.isinf(s["snr"])]
        assert min(snrs) > 12 and sum(snrs) / len(snrs) > 20


def main() -> None:
    check_vag()
    check_xbox_adpcm()
    check_resample()
    check_stats()
    check_audsrv()
    with tempfile.TemporaryDirectory() as td:
        w, m = build_synthetic(Path(td))
        check_parsing(m)
        check_reachability(m)
        pack = check_conversion(w, m, Path(td))
        check_budget(w, m)
        pk = hp2.parse(pack)
        check_section_corruption(pk.read("SND0"), pk.find("SNDD").raw_size)
        (Path(td) / "w.snd.hp2").write_bytes(pack)
        assert "SNDD" in sc.info_pack(Path(td) / "w.snd.hp2")
        # command line: convert, info, check, survey, inspect with wav output
        out = Path(td) / "o.snd.hp2"
        assert sc.main([str(m.path), "-o", str(out), "--cache", str(Path(td) / "c2"), "--drops", "--json", str(Path(td) / "j.json")]) == 0
        assert hp2.parse(out.read_bytes()).read("SND0") == pk.read("SND0")
        saved = dict(sc.CLASS_WEIGHT)
        try:
            assert sc.main([str(m.path), "-o", str(Path(td) / "w.hp2"), "--weight", "13=500", "--budget", "4000"]) == 0
            assert sc.CLASS_WEIGHT[13] == 500.0
        finally:
            sc.CLASS_WEIGHT.clear()
            sc.CLASS_WEIGHT.update(saved)
        assert sc.main(["--info", str(out)]) == 0 and sc.main([str(m.path), "--check"]) == 0
        assert sc.main([str(m.path), "--survey", "5"]) == 0
        wavs = Path(td) / "wavs"
        assert sc.main([str(m.path), "--inspect", "rifle\\fire", "--wav-dir", str(wavs), "--converted"]) == 0
        assert len(list(wavs.glob("*.xbox.wav"))) >= 3 and len(list(wavs.glob("*.spu2.wav"))) >= 3
        back, rate = vag.read_wav(next(iter(sorted(wavs.glob("*shot1.spu2.wav")))))
        assert rate == 22050 and len(back) == 1280 // 28 * 28 + 28
        if os.environ.get("SND_FIXTURE"):
            Path(os.environ["SND_FIXTURE"]).write_bytes(pack)
    check_real_maps()
    print("ok")


if __name__ == "__main__":
    main()
