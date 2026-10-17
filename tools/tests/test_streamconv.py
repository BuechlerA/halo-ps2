"""STRM / STRD conversion on a synthetic map (no game data needed): the IMA encoder against the reference decoder, byte-copy
passthrough, the resampler, chain concatenation, folding, loops, long ambience selection, deduplication, the section writer
and its reference parser, corruption handling, the command line. Set STRM_FIXTURE=<path> to also write the .hp2 (and
<path>.crc, the reference decoder's CRC-32 of every stream's PCM) that engine/tests/test_strmdef.c and test_stream.c open;
those tests hard-code the numbers of this world (see WORLD below). With assets/decompressed/a10.map the real map is converted
(cold cache, all cores) and checked."""
import array
import contextlib
import io
import math
import os
import struct
import sys
import tempfile
import zlib
from pathlib import Path

sys.path[:0] = [str(Path(__file__).resolve().parents[1]), str(Path(__file__).resolve().parent)]
from halo2ps2 import hp2, soundconv as sc, streamconv as sv, texconv, vag  # noqa: E402
import test_soundconv as ts  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
NONE = 0xFFFFFFFF

# ---- the synthetic world -------------------------------------------------------------------------------
# Every music piece is 44100 Hz stereo with a pure tone (400 / 1000 / 2000 / 3000 Hz), the dialogue 22050 Hz mono.
#   m1\intro  class 32, 1 perm of 22016 frames, 400 Hz     (start)
#   m1\loop   class 32, chain of 4 perms x 5504 frames, 1000 Hz  (loop)
#   m1\alt    class 32, 1 perm of 22016 frames, 2000 Hz    (alternate loop)
#   m1\end    class 32, 1 perm of 11008 frames, 3000 Hz    (end)
#   m2\long   class 32, chain of 5 perms x 3200 frames, 700 Hz, not a loop
#   a\line1   class 19, 3 flat perms (a, b, a again: the third repeats the first byte for byte), 22050 Hz mono, 300 / 500 / 300 Hz
#   a\line2   class 44, 1 stereo perm 44100 Hz (folded to mono 22050)
#   ambience  air_loop class 33, 6.5 s mono 22050 Hz (long loop: streamed), hum_loop 1 s (short loop: not streamed)
SECTOR_PAD = 2048
LSND1 = sc.LSND_MAIN + sc.TRACK_STRIDE_SRC
TAGS = [
    ("scnr", "levels\\t\\t", 0x400), ("matg", "globals\\globals", 0x2000),
    ("lsnd", "sound\\music\\m1", LSND1), ("lsnd", "sound\\sfx\\ambience\\air", LSND1), ("lsnd", "sound\\sfx\\ambience\\hum", LSND1),
    ("snd!", "sound\\music\\m1\\intro", 0x2000), ("snd!", "sound\\music\\m1\\loop", 0x2000), ("snd!", "sound\\music\\m1\\alt", 0x2000),
    ("snd!", "sound\\music\\m1\\end", 0x2000), ("snd!", "sound\\music\\m2\\long", 0x2000),
    ("snd!", "sound\\dialog\\a\\line1", 0x2000), ("snd!", "sound\\dialog\\a\\line2", 0x2000),
    ("snd!", "sound\\sfx\\ambience\\air_loop", 0x2000), ("snd!", "sound\\sfx\\ambience\\hum_loop", 0x2000),
    ("snd!", "sound\\sfx\\weapons\\fire", 0x2000),
]
INTRO, LOOP, ALT, END, LONG, LINE1, LINE2, AIR, HUM, FIRE = (t[1] for t in TAGS[5:])
LOOP_HZ = 500 * 44100 / 22016           # 500 whole cycles over the loop (1001.5 Hz): the seam is continuous in the source


class BigWorld(ts.World):
    DATA_AREA = 0x100000


def tone(n: int, freq: float, amp: float, rate: int, phase: float = 0.0) -> list[int]:
    return ts.sine(n, freq, amp, rate, phase)


def stereo_tone(n: int, freq: float, rate: int = 44100) -> list[list[int]]:
    return [tone(n, freq, 9000, rate), tone(n, freq, 7000, rate, 0.5)]


def build_synthetic(directory: Path) -> tuple[BigWorld, texconv.MapData]:
    w = BigWorld(TAGS)
    w.add_sound(INTRO, 32, 1, 2, [("p", stereo_tone(22016, 400), 0.0, 1.0, 0xFFFF)])
    w.add_sound(LOOP, 32, 1, 2, [(f"c{i}", [c[i * 5504:(i + 1) * 5504] for c in stereo_tone(4 * 5504, LOOP_HZ)], 0.0, 1.0, i + 1 if i < 3 else 0xFFFF)
                                 for i in range(4)], flags=2)
    w.add_sound(ALT, 32, 1, 2, [("p", stereo_tone(22016, 2000), 0.0, 1.0, 0xFFFF)])
    w.add_sound(END, 32, 1, 2, [("p", stereo_tone(11008, 3000), 0.0, 1.0, 0xFFFF)])
    w.add_sound(LONG, 32, 1, 2, [(f"c{i}", [c[i * 3200:(i + 1) * 3200] for c in stereo_tone(5 * 3200, 700)], 0.0, 1.0, i + 1 if i < 4 else 0xFFFF)
                                 for i in range(5)], flags=2)
    a, b = tone(64 * 70, 300, 8000, 22050), tone(64 * 40, 500, 8000, 22050)
    w.add_sound(LINE1, 19, 0, 1, [("a", [a], 0.0, 1.0, 0xFFFF), ("b", [b], 0.1, 0.5, 0xFFFF), ("a2", [a], 0.0, 1.0, 0xFFFF)],
                minmax=(2.0, 30.0))
    w.add_sound(LINE2, 44, 1, 2, [("s", stereo_tone(64 * 100, 800), 0.0, 1.0, 0xFFFF)])
    w.add_sound(AIR, 33, 0, 1, [("loop1", [tone(64 * 2240, 22050 / 64 * 3, 6000, 22050)], 0.0, 1.0, 0xFFFF)])      # 143360 frames: 6.5 s
    w.add_sound(HUM, 33, 0, 1, [("loop1", [tone(64 * 345, 220, 6000, 22050)], 0.0, 1.0, 0xFFFF)])                   # 22080 frames: 1.0 s
    w.add_sound(FIRE, 4, 0, 1, [("shot", [tone(64 * 20, 440, 9000, 22050)], 0.0, 1.0, 0xFFFF)])
    w.add_loop("sound\\music\\m1", 0, [(0, 0.75, 0.2, 2.0, [INTRO, LOOP, END, ALT, None])], [])
    w.add_loop("sound\\sfx\\ambience\\air", 0, [(0, 1.0, 0.0, 0.5, [None, AIR, None, None, None])], [])
    w.add_loop("sound\\sfx\\ambience\\hum", 0, [(0, 1.0, 0.0, 0.0, [None, HUM, None, None, None])], [])
    return w, w.map(directory)


# ---- codec -----------------------------------------------------------------------------------------------
def rms(x) -> float:
    return math.sqrt(sum(v * v for v in x) / max(1, len(x)))


def zero_crossings(x) -> int:
    return sum(1 for i in range(1, len(x)) if x[i - 1] < 0 <= x[i])


def check_codec() -> None:
    n = 64 * 60
    a = array.array("h", [int(v + w // 8) for v, w in zip(tone(n, 523, 12000, 22050), ts.noisy(n, 3))])
    b = array.array("h", tone(n, 1700, 9000, 22050, 1.0))
    mono = sv.encode_xbox([a])
    assert len(mono) == 36 * 60
    sv.check_blocks(mono, 1)
    dec = sv.decode_xbox(mono, 1)[0]
    assert len(dec) == n and vag.snr_db(list(a), list(dec)) > 25
    assert all(dec[64 * k] == a[64 * k] for k in range(60))                           # header predictor = sample 0, exact
    assert all(mono[36 * k + 35] >> 4 == 0 for k in range(60))                       # nibble 63 is padding
    st = sv.encode_xbox([a, b])
    sv.check_blocks(st, 2)
    left, right = sv.decode_xbox(st, 2)
    assert left == dec and vag.snr_db(list(b), list(right)) > 18                      # L of the stereo stream = the mono stream
    assert sc.index_continuity(mono, 1) == (59, 59) and sc.index_continuity(st, 2) == (118, 118)
    # the faster encoder here produces the same container as soundconv's reference encoder, decoded by the same decoder
    ref = sc.xbox_adpcm_encode([list(a)])
    ref_dec = sc.xbox_adpcm_decode(ref, 1)[0]
    assert abs(vag.snr_db(list(a), ref_dec) - vag.snr_db(list(a), list(dec))) < 1.5
    # decoder = soundconv reference, piecewise
    big = mono * 30
    assert list(sv.decode_xbox(big, 1)[0]) == sc.xbox_adpcm_decode(big, 1)[0]
    assert sv.pcm_crc(st, 2) == zlib.crc32(array.array("h", [v for pair in zip(left, right) for v in pair]).tobytes()) & 0xFFFFFFFF
    # bad blocks are rejected
    bad = bytearray(mono)
    bad[2 + 36] = 89
    for data, ch in ((bytes(bad), 1), (mono[:-1], 1), (b"", 1), (st[:72 * 3 + 4], 2)):
        try:
            sv.check_blocks(data, ch)
            raise AssertionError("bad stream accepted")
        except sv.StreamError:
            pass
    for args in (([a[:63]],), ([],), ([a, b[:64]],)):
        try:
            sv.encode_xbox(*args)
            raise AssertionError("bad encode accepted")
        except (sv.StreamError, IndexError):
            pass
    # full-scale and silence stay inside int16 / exact
    sq = array.array("h", [32767 if (i // 9) % 2 else -32768 for i in range(64 * 8)])
    assert all(-32768 <= v <= 32767 for v in sv.decode_xbox(sv.encode_xbox([sq]), 1)[0])
    assert list(sv.decode_xbox(sv.encode_xbox([array.array("h", [0] * 128)]), 1)[0]) == [0] * 128


def check_resample() -> None:
    n = 44100
    x = array.array("h", tone(n, 1000, 10000, 44100))
    y = sv.resample(x, 32000)
    assert len(y) == 32000
    mid = y[200:-200]
    assert abs(rms(mid) / (10000 / math.sqrt(2)) - 1) < 0.01                          # passband gain 1 within 0.09 dB
    assert abs(zero_crossings(y) - 1000) <= 1                                         # frequency preserved: 1 kHz for 1 s
    hi = array.array("h", tone(n, 20000, 10000, 44100))                              # above the output Nyquist (16 kHz)
    assert rms(sv.resample(hi, 32000)[200:-200]) < rms(hi) * 0.08                     # >= 22 dB down (24 taps)
    assert list(sv.resample(array.array("h", [5000] * 4000), 2900)[40:-40]) == [5000] * (2900 - 80)    # unity DC gain
    # periodic mode: a tiled signal resamples identically in the middle (seamless wrap)
    per = array.array("h", [int(round(8000 * math.sin(2 * math.pi * 8 * i / 512))) for i in range(512)])
    tiled = sv.resample(per * 3, 3 * 372)
    assert list(sv.resample(per, 372, periodic=True)) == list(tiled[372:744])
    assert list(sv.resample(per, 372)[:2]) != list(tiled[372:374])                    # not periodic: fades in from zero
    for bad in (0, 513):
        try:
            sv.resample(per, bad)
            raise AssertionError("bad resample accepted")
        except sv.StreamError:
            pass
    # output planning
    assert sv.plan_output(22016, 44100, 32000, False) == (15975, 16000, 32000)
    assert sv.plan_output(22016, 44100, 32000, True) == (15975, 15936, 32000)
    assert sv.plan_output(100, 44100, 32000, True) == (73, 128, 32000)                # too short to shrink: padded
    assert sv.plan_output(1280, 22050, 22050, True) == (1280, 1280, 22050)
    pcm, rate = sv.convert_pcm([array.array("h", tone(22016, 1000, 8000, 44100))], 44100, 32000, False)
    assert rate == 32000 and len(pcm[0]) == 16000 and pcm[0][-1] == pcm[0][15974]       # held to the next block


# ---- planning --------------------------------------------------------------------------------------------
def check_chains() -> None:
    mk = lambda nxt: sc.SrcRange("r", 1.0, 0.0, 1.0, [sc.SrcPerm(i, 0, f"p{i}", 0.0, 1.0, n, 36, 0, 1) for i, n in enumerate(nxt)])
    cs, pr = sv.chains(mk([1, 2, 0xFFFF, 0xFFFF]))
    assert [[p.index for p in c] for c in cs] == [[0, 1, 2], [3]] and not pr
    cs, pr = sv.chains(mk([0xFFFF] * 3))
    assert [[p.index for p in c] for c in cs] == [[0], [1], [2]]
    cs, pr = sv.chains(mk([1, 0]))                                                    # a cycle has no head
    assert cs == [] and any("cycle" in p for p in pr)
    cs, pr = sv.chains(mk([2, 2, 0xFFFF]))                                            # two chains run into the same permutation
    assert any("already used" in p for p in pr)


def check_plan(m: texconv.MapData) -> None:
    items, loops, problems = sv.plan_items(m)
    assert not problems
    by = {it.sound.path: it for it in items}
    assert set(by) == {INTRO, LOOP, ALT, END, LONG, LINE1, LINE2, AIR}               # fire (resident) and hum_loop (1 s) stay out
    assert [it.sound.tag_id for it in items] == sorted(it.sound.tag_id for it in items)
    assert by[LINE1].kind == sv.KIND_DIALOGUE and by[LINE2].kind == sv.KIND_DIALOGUE and by[INTRO].kind == sv.KIND_MUSIC
    assert by[AIR].kind == sv.KIND_AMBIENCE and by[AIR].extra and by[AIR].loop and not by[INTRO].extra
    assert by[LOOP].loop and by[ALT].loop and not by[INTRO].loop and not by[END].loop and not by[LONG].loop
    assert [len(v.perms) for v in by[LOOP].variants] == [4] and [len(v.perms) for v in by[LONG].variants] == [5]
    assert [len(v.perms) for v in by[LINE1].variants] == [1, 1, 1] and [v.gain for v in by[LINE1].variants] == [1.0, 0.5, 1.0]
    assert [lp.path for lp in loops] == sorted((lp.path for lp in loops), key=lambda p: next(l.tag_id for l in loops if l.path == p)) and len(loops) == 2
    # options: no dialogue, a root list, a long-ambience threshold
    nd, _, _ = sv.plan_items(m, dialogue=False)
    assert {it.sound.path for it in nd} == {INTRO, LOOP, ALT, END, LONG, AIR}
    l1 = by[LINE1].sound.tag_id
    only, _, _ = sv.plan_items(m, dialogue_roots={l1})
    assert {it.sound.path for it in only} == {INTRO, LOOP, ALT, END, LONG, LINE1, AIR}
    amb, _, _ = sv.plan_items(m, ambience_seconds=0.5)
    assert HUM in {it.sound.path for it in amb}
    none, _, _ = sv.plan_items(m, ambience_seconds=60)
    assert AIR not in {it.sound.path for it in none}


# ---- conversion ------------------------------------------------------------------------------------------
def src_bytes(w: BigWorld, path: str, names: list[str], ch: int) -> bytes:
    return b"".join(sc.xbox_adpcm_encode(w.pcm[f"{path}/{n}"]) for n in names)


def check_conversion(w: BigWorld, m: texconv.MapData, td: Path) -> sv.Built:
    cache = td / "cache"
    b = sv.build_sections(m, cache_dir=cache, jobs=2)
    sec = sv.parse_section(b.strm, len(b.strd))
    ids = {e.path: e.tag_id for e in m.index.entries if e.group in ("snd!", "lsnd")}
    snd = {p: next(s for s in sec.sounds if s["tag"] == ids[p]) for p in (INTRO, LOOP, ALT, END, LONG, LINE1, LINE2, AIR)}
    assert len(sec.sounds) == 8
    assert len(sec.streams) == 1 + 1 + 1 + 1 + 1 + 3 + 1 + 1 == 10
    assert len(sec.loops) == 2 and len(sec.tracks) == 2 and b.stats["streams"] == 10 and b.stats["sounds"] == 8
    assert sec.data_size == len(b.strd) and all(s["offset"] % sv.DATA_ALIGN == 0 for s in sec.streams)
    st = lambda p, k=0: sec.streams[snd[p]["first"] + k]
    # dialogue: three variants, flat; the third is the first one's twin: same bytes, shared data
    d = snd[LINE1]
    assert d["klass"] == 19 and d["kind"] == sv.KIND_DIALOGUE and d["count"] == 3 and (d["min_dist"], d["max_dist"]) == (2.0, 30.0)
    a0, a1, a2 = (st(LINE1, k) for k in range(3))
    assert a0["rate"] == 22050 and a0["channels"] == 1 and a0["frames"] == 64 * 70 and not a0["flags"] & sv.SF_RESAMPLED
    assert a1["frames"] == 64 * 40 and a1["gain"] == 0.5 and abs(a1["skip"] - 0.1) < 1e-6 and a1["snr10"] == 0x7FFFFFFF
    assert a2["offset"] == a0["offset"] and a2["flags"] & sv.SF_SHARED and not a0["flags"] & sv.SF_SHARED and a1["offset"] != a0["offset"]
    assert b.stats["shared"] == 1
    # a byte copy: the stream is the source, byte for byte, and decodes to the reference
    raw = sv.stream_bytes(b.strd, 0, a0)
    assert raw == src_bytes(w, LINE1, ["a"], 1) and a0["src_bytes"] == len(raw)
    assert sv.stream_bytes(b.strd, 0, a1) == src_bytes(w, LINE1, ["b"], 1)
    # stereo 44100 dialogue is folded and decoded to 22050 mono
    l2 = st(LINE2)
    assert snd[LINE2]["klass"] == 44 and l2["channels"] == 1 and l2["rate"] == 22050 and l2["flags"] & sv.SF_FOLDED and l2["flags"] & sv.SF_RESAMPLED
    assert l2["frames"] == 3200 and l2["snr10"] != 0x7FFFFFFF and l2["snr10"] > 200
    src = sc.xbox_adpcm_decode(sc.xbox_adpcm_encode(w.pcm[f"{LINE2}/s"]), 2)
    fold = array.array("h", sc.fold_mono(src))
    got = sv.decode_xbox(sv.stream_bytes(b.strd, 0, l2), 1)[0]
    assert vag.snr_db(list(sv.resample(fold, 3200)), list(got)) > 20
    assert abs(zero_crossings(got) - 800 * 3200 / 22050) <= 2
    # a music chain: the permutations are concatenated into one stream (resampled 44100 -> 32000 stereo); gain/pitch survive
    lg = st(LONG)
    assert snd[LONG]["klass"] == 32 and snd[LONG]["count"] == 1 and lg["chunks"] == 5 and lg["channels"] == 2 and lg["rate"] == 32000
    assert lg["flags"] == sv.SF_RESAMPLED and lg["loop_start"] == lg["loop_end"] == 0
    assert lg["frames"] == 11648 and lg["src_bytes"] == 5 * 50 * 72       # 5 x 3200 frames = 16000 = 11636 resampled, padded to 182 blocks
    left, right = sv.decode_xbox(sv.stream_bytes(b.strd, 0, lg), 2)
    assert abs(zero_crossings(left) - 700 * 16000 / 44100) <= 2 and abs(zero_crossings(right) - zero_crossings(left)) <= 1
    assert 0.95 < rms(left[100:-100]) / (9000 / math.sqrt(2)) < 1.05 and abs(left[-1] - left[11636]) < 200      # the padding holds the last sample (ADPCM noise aside)
    # a looping chain: shrunk to whole blocks, loop points = the whole stream, nominal rate; the seam is continuous
    lo = st(LOOP)
    assert lo["flags"] == sv.SF_RESAMPLED | sv.SF_LOOP and lo["rate"] == 32000 and lo["frames"] == 15936 and lo["frames"] % 64 == 0
    assert (lo["loop_start"], lo["loop_end"]) == (0, 15936) and snd[LOOP]["flags"] == sv.SS_LOOPING
    le, _ = sv.decode_xbox(sv.stream_bytes(b.strd, 0, lo), 2)
    assert max(abs(le[0] - le[-1]), abs(le[1] - le[0])) < 2400        # about the largest step of the tone (1767) plus codec noise
    # byte-copied loop / one-shot are identical to the source, the loop flag is set where the track plays it as a loop
    assert st(INTRO)["flags"] == sv.SF_RESAMPLED and st(ALT)["flags"] == sv.SF_RESAMPLED | sv.SF_LOOP and st(END)["frames"] == 8000
    # long ambience: mono 22050 byte copy, loop, marked extra
    air = st(AIR)
    assert snd[AIR]["flags"] == sv.SS_LOOPING | sv.SS_EXTRA and snd[AIR]["kind"] == sv.KIND_AMBIENCE
    assert air["frames"] == 64 * 2240 and air["flags"] == sv.SF_LOOP and (air["loop_start"], air["loop_end"]) == (0, 64 * 2240)
    assert sv.stream_bytes(b.strd, 0, air) == sc.xbox_adpcm_encode(w.pcm[f"{AIR}/loop1"])
    # sound_looping: tracks keep gains and fades and the five sound ids
    lp = {x["tag"]: x for x in sec.loops}
    music = lp[ids["sound\\music\\m1"]]
    tr = sec.tracks[music["track_first"]]
    assert music["track_count"] == 1 and (tr["start"], tr["loop"], tr["end"], tr["alt_loop"], tr["alt_end"]) == (
        ids[INTRO], ids[LOOP], ids[END], ids[ALT], NONE)
    assert tr["gain"] == 0.75 and abs(tr["fade_in"] - 0.2) < 1e-6 and tr["fade_out"] == 2.0
    assert ids["sound\\sfx\\ambience\\hum"] not in lp                                # names no streamed sound: no record
    assert not sv.track_format_problems(sec)
    # statistics and the by-kind split
    assert b.stats["by_kind"]["dialogue"]["streams"] == 4 and b.stats["by_kind"]["music"]["streams"] == 5
    assert b.stats["passthrough"] == 4 and b.stats["transcoded"] == 6
    # cold run = cached run, byte for byte; transcodes appear in the cache
    assert len(list(cache.glob("*.stc"))) == 6
    b2 = sv.build_sections(m, cache_dir=cache, jobs=1)
    assert (b2.strm, b2.strd) == (b.strm, b.strd)
    b3 = sv.build_sections(m, jobs=3)                                                # no cache, process pool
    assert (b3.strm, b3.strd) == (b.strm, b.strd)
    # a higher cap keeps everything (44100 is above 32000, so the music is still resampled) and a lower one changes the rate
    b4 = sv.build_sections(m, rates={sv.KIND_MUSIC: 22050}, jobs=1)
    assert {s["rate"] for s in sv.parse_section(b4.strm, len(b4.strd)).streams} == {22050}
    return b


def check_pack_io(b: sv.Built, td: Path) -> None:
    out = td / "w.strm.hp2"
    sv.write_pack(out, "synthetic", b)
    pk = hp2.parse(out.read_bytes())
    assert [s.tag for s in pk.sections] == ["STRM", "STRD"] and all(s.codec == hp2.CODEC_STORED and s.offset % 2048 == 0 for s in pk.sections)
    assert pk.read("STRD") == b.strd and pk.read("STRM") == b.strm
    sec, raw, base = sv.load_pack(out)
    assert base == pk.find("STRD").offset and len(sec.streams) == 10
    assert "10 streams" in sv.info_pack(out)
    crc = td / "w.crc"
    assert sv.verify_pack(out, 40, crc) == []
    lines = crc.read_text().split("\n")[:-1]
    assert len(lines) == 10 and lines[0].split()[0] == "0"
    # a damaged block header is reported
    bad = bytearray(raw)
    bad[base + sec.streams[0]["offset"] + 2] = 200
    (td / "bad.strm.hp2").write_bytes(bytes(bad))
    probs = sv.verify_pack(td / "bad.strm.hp2")
    assert len(probs) == 1 and "step index" in probs[0]


def check_corruption(strm: bytes, data_size: int) -> None:
    sv.parse_section(strm, data_size)
    for cut in (0, 10, sv.HEADER_SIZE - 1, len(strm) - 1):
        try:
            sv.parse_section(strm[:cut], data_size)
            raise AssertionError("truncated section accepted")
        except (sv.StreamError, struct.error):
            pass
    so, vo, lo, to = struct.unpack_from("<4I", strm, 0x1C)
    cases = {"magic": (0, "<I", 0), "version": (4, "<H", 2), "size": (8, "<I", 1), "data size": (0x2C, "<I", data_size + 1),
             "align": (0x30, "<I", 3), "stream offset": (vo, "<I", data_size), "stream size": (vo + 4, "<I", 35),
             "frames": (vo + 8, "<I", 64), "rate": (vo + 12, "<I", 0), "loop end": (vo + 20, "<I", 5), "format": (vo + 33, "<B", 9),
             "channels": (vo + 32, "<B", 3), "flags": (vo + 34, "<H", 16), "sound order": (so + sv.SOUND_REC, "<I", 0),
             "sound count": (so + 8, "<H", 0), "sound first": (so + 12, "<I", 99), "kind": (so + 5, "<B", 9),
             "loop tracks": (lo + 16, "<I", 5), "NaN": (vo + 24, "<I", 0x7FC00000)}
    for what, (at, fmt, val) in cases.items():
        c = bytearray(strm)
        struct.pack_into(fmt, c, at, val)
        try:
            sv.parse_section(bytes(c), data_size)
            raise AssertionError(what + " accepted")
        except sv.StreamError:
            pass


def quiet_main(args: list[str]) -> int:
    with contextlib.redirect_stdout(io.StringIO()):
        return sv.main(args)


def check_cli(m: texconv.MapData, td: Path) -> bytes:
    out = td / "o.strm.hp2"
    assert quiet_main([str(m.path), "-o", str(out), "--cache", str(td / "c2"), "--jobs", "2"]) == 0
    assert quiet_main(["--info", str(out)]) == 0 and quiet_main(["--verify", str(out), "--samples", "4", "--crc-out", str(td / "o.crc")]) == 0
    nod = td / "nd.strm.hp2"
    assert quiet_main([str(m.path), "-o", str(nod), "--no-dialogue", "--jobs", "1"]) == 0
    assert len(sv.load_pack(nod)[0].sounds) == 6
    roots = td / "roots.txt"
    roots.write_text(f"{next(e.tag_id for e in m.index.entries if e.path == LINE1):#x}\n")
    ro = td / "ro.strm.hp2"
    assert quiet_main([str(m.path), "-o", str(ro), "--roots", f"@{roots}", "--jobs", "1"]) == 0
    assert quiet_main([str(m.path), "-o", str(td / "ro2.strm.hp2"), "--roots", LINE2, "--jobs", "1"]) == 0
    assert len(sv.load_pack(ro)[0].sounds) == 7
    try:
        quiet_main([str(m.path), "-o", str(td / "x.hp2"), "--roots", "no\\such\\tag"])
        raise AssertionError("unknown root accepted")
    except sv.StreamError:
        pass
    return out.read_bytes()


# ---- real map (skipped without game data) ------------------------------------------------------------------
def check_real_maps() -> None:
    path = ROOT / "assets" / "decompressed" / "a10.map"
    if not path.exists():
        print("  (a10.map not present: real-map checks skipped)")
        return
    m = texconv.MapData.open(path)
    with tempfile.TemporaryDirectory() as td:
        td = Path(td)
        b = sv.build_sections(m, cache_dir=td / "cache")
        sec = sv.parse_section(b.strm, len(b.strd))
        assert len(sec.sounds) > 500 and len(sec.streams) > 3000 and len(b.strd) > 50 << 20
        assert not sv.track_format_problems(sec)
        by_kind = {k: sum(1 for s in sec.sounds if s["kind"] == k) for k in (0, 1, 2)}
        assert by_kind[0] > 500 and by_kind[1] > 20 and by_kind[2] > 0
        assert all(st["rate"] <= 32000 for st in sec.streams) and all(st["channels"] == 1 for s in sec.sounds if s["kind"] == 0
                                                                       for st in sec.streams[s["first"]:s["first"] + s["count"]])
        out = td / "a10.strm.hp2"
        sv.write_pack(out, "a10", b)
        assert sv.verify_pack(out, 20) == []
        # every music stream that was re-encoded keeps its duration (within a block) and reaches a sane SNR
        src = {e.tag_id: sc.read_sound(sc.Tags(m), e) for e in m.index.entries if e.group == "snd!"}
        done = 0
        for s in sec.sounds:
            if s["kind"] != 1 or done >= 3:
                continue
            ss = src[s["tag"]]
            st = sec.streams[s["first"]]
            if st["flags"] & sv.SF_RESAMPLED:
                total = sum(ss.frames(p) for p in ss.perms[:st["chunks"]]) if not any(p.next_index != 0xFFFF for p in ss.perms) else None
                assert abs(st["frames"] / st["rate"] - st["src_bytes"] // 72 * 64 / ss.rate) < 0.05
                assert st["snr10"] > 150
                done += 1
        assert done


def main() -> None:
    check_codec()
    check_resample()
    check_chains()
    with tempfile.TemporaryDirectory() as td:
        td = Path(td)
        w, m = build_synthetic(td)
        check_plan(m)
        b = check_conversion(w, m, td)
        check_pack_io(b, td)
        check_corruption(b.strm, len(b.strd))
        pack = check_cli(m, td)
        assert hp2.parse(pack).read("STRM") == b.strm and hp2.parse(pack).read("STRD") == b.strd
        if os.environ.get("STRM_FIXTURE"):
            fx = Path(os.environ["STRM_FIXTURE"])
            fx.write_bytes(pack)
            assert sv.verify_pack(fx, 100, Path(str(fx) + ".crc")) == []
    check_real_maps()
    print("ok")


if __name__ == "__main__":
    main()
