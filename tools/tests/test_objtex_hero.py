"""Hero textures (objtex.HERO, ADR-073): sizing and budget rules on plain numbers, and on the real a10 map (skipped without assets/decompressed/a10.map): the command display
layers, the uniforms and Cortana keep up to 256 texels per side inside their own allowance, and the ordinary textures keep the size their budget gives them."""
import os
import struct
import sys
from pathlib import Path

sys.path[:0] = [str(Path(__file__).resolve().parents[1])]
from halo2ps2 import hp2, objtex, texconv  # noqa: E402


def test_sizes():
    assert objtex.hero_size(512, 512, 256) == (256, 256)          # a display map: half the source, not a quarter
    assert objtex.hero_size(512, 256, 256) == (256, 128)
    assert objtex.hero_size(128, 256, 256) == (128, 256)          # never above the source
    assert objtex.hero_size(128, 32, 256) == (128, 32)
    assert objtex.hero_size(512, 512, 64) == (64, 64)
    assert objtex.hero_size(1024, 128, 256) == (256, 32)          # the aspect ratio survives the halving
    assert objtex.target_size(512, 512, 128) == (128, 128)        # an ordinary one: half, then the budget's dim


def test_estimate():
    sizes = [(512, 512, 0, 0), (512, 512, 256, 4), (512, 256, 256, 0)]
    plain = objtex.estimate_bytes(sizes, 128)
    hero = objtex.estimate_bytes(sizes, 128, 256, True)
    both = plain + hero
    assert plain > 0 and hero > plain
    assert objtex.estimate_bytes(sizes, 128, 256, True) > objtex.estimate_bytes(sizes, 128, 64, True)       # a lower hero cap costs less
    assert objtex.estimate_bytes(sizes, 64) < plain                                                          # the plain ones follow dim
    assert objtex.estimate_bytes(sizes, 64, 256, True) == hero                                               # the hero ones do not
    four = objtex.estimate_bytes([(512, 512, 256, 4)], 128, 256, True)
    eight = objtex.estimate_bytes([(512, 512, 256, 0)], 128, 256, True)
    assert 0.45 < four / eight < 0.55, (four, eight)                                                         # 16 colours = half the bytes
    assert both == objtex.estimate_bytes(sizes, 128, 256, False) + hero


def test_table():
    assert all(1 <= d <= texconv.MAX_TEX and b in (0, 4) for d, b in objtex.HERO.values())
    assert objtex.hero_dim(texconv.ShaderInfo(1, "sotr", "levels\\a10\\devices\\command display\\shaders\\indicators")) == 256
    assert objtex.hero_bits(texconv.ShaderInfo(1, "sotr", "levels\\a10\\devices\\command display\\shaders\\indicators")) == 4
    assert objtex.hero_dim(texconv.ShaderInfo(1, "soso", "characters\\cyborg\\shaders\\armor")) == 0


def test_real_a10():
    mp = Path(__file__).resolve().parents[2] / "assets" / "decompressed" / "a10.map"
    if not mp.exists():
        print("real a10: skipped (no map)")
        return
    from halo2ps2 import hp2  # noqa: E402
    obj = Path(__file__).resolve().parents[2] / "assets" / "hp2" / "a10.obj.hp2"
    if not obj.exists():
        print("real a10: skipped (no object pack)")
        return
    m = texconv.MapData.open(mp)
    v = texconv.parse_texs(hp2.parse(obj.read_bytes()).read("OTEX"))
    byb = {b["shader_id"]: b for b in v.bindings}
    hero_bytes, seen = 0, set()
    for e in m.index.entries:
        if e.tag_id in byb and e.path in objtex.HERO and byb[e.tag_id]["texture"] < len(v.textures):
            t = v.textures[byb[e.tag_id]["texture"]]
            assert max(t["width"], t["height"]) <= objtex.HERO[e.path][0]
            if "command display" in e.path and "no 1s" not in e.path:
                assert (t["width"], t["height"]) == (256, 256) and t["psm"] == texconv.PSMT4, (e.path, t["width"], t["height"], t["psm"])
            if byb[e.tag_id]["texture"] not in seen:
                seen.add(byb[e.tag_id]["texture"])
                hero_bytes += t["pixel_bytes"]
    assert seen and hero_bytes <= objtex.HERO_BUDGET, (len(seen), hero_bytes)
    ordinary = [t for i, t in enumerate(v.textures) if i not in seen]
    assert max(max(t["width"], t["height"]) for t in ordinary if t["source_id"] not in objtex.SKY_GAIN) <= 256
    big = sum(1 for t in ordinary if (t["width"], t["height"]) == (128, 128))
    assert big > 50, big                                                                                       # the ordinary textures were not squeezed to make room
    print(f"real a10: {len(seen)} hero textures, {hero_bytes / 1024:.0f} KiB (allowance {objtex.HERO_BUDGET // 1024} KiB), {big} ordinary 128 x 128")


def synthetic_hires():
    """An OtexResult-shaped object: 5 base textures, variants for 1, 3 and 4 (one PSMT4 at 256 x 256, one PSMT8 128 x 64, one PSMT8 256 x 128), pixels a known pattern."""
    class R:
        pass
    r = R()
    r.textures = [object()] * 5
    r.hires = {}
    for base, (w, h, psm) in ((1, (256, 256, texconv.PSMT4)), (3, (128, 64, texconv.PSMT8)), (4, (256, 128, texconv.PSMT8))):
        n = w * h // (2 if psm == texconv.PSMT4 else 1)
        entries = 16 if psm == texconv.PSMT4 else 256
        pix = bytes((base * 31 + i) & 0xFF for i in range(n))
        clut = bytes((base * 7 + i) & 0xFF for i in range(entries * 4))
        r.hires[base] = texconv.Ps2Texture(0x1000 + base, w, h, psm, 0, clut, entries, [pix], 0x80808080)
    return r


def test_hires_blob():
    r = synthetic_hires()
    texs, hmap = objtex.build_hires(r)
    v = texconv.parse_texs(texs)
    assert [(t["width"], t["height"], t["mips"]) for t in v.textures] == [(256, 256, 1), (128, 64, 1), (256, 128, 1)] and not v.bindings
    assert [t["source_id"] for t in v.textures] == [0x1001, 0x1003, 0x1004]
    count, *m = struct.unpack_from("<I5H", hmap)
    assert count == 5 and m == [0xFFFF, 0, 0xFFFF, 1, 2] and len(hmap) % 4 == 0
    for t, base in zip(v.textures, (1, 3, 4)):                      # clut then pixels, contiguous (the engine reads them in one go)
        assert t["pixel_offset"] - t["clut_offset"] == t["clut_entries"] * 4
        assert v.data[t["pixel_offset"]:t["pixel_offset"] + t["pixel_bytes"]] == r.hires[base].mips[0]
    pack = hp2.build("t", [("TEXS", texs), ("HMAP", hmap)], compress=False)
    pk = hp2.parse(pack)
    assert all(s.stored_size == s.raw_size for s in pk.sections)
    if os.environ.get("HIRES_FIXTURE"):
        Path(os.environ["HIRES_FIXTURE"]).write_bytes(pack)


def test_real_hires():
    mp = Path(__file__).resolve().parents[2] / "assets" / "hp2" / "a10.hires.hp2"
    if not mp.exists():
        print("real a10 hires: skipped")
        return
    pk = hp2.parse(mp.read_bytes())
    v = texconv.parse_texs(pk.read("TEXS"))
    hm = pk.read("HMAP")
    count = struct.unpack_from("<I", hm)[0]
    maps = struct.unpack_from("<%dH" % count, hm, 4)
    used = [m for m in maps if m != 0xFFFF]
    assert len(used) == len(v.textures) > 50 and len(set(used)) == len(used)
    assert all(t["mips"] == 1 and t["pixel_bytes"] <= objtex.HIRES_MAX_BYTES for t in v.textures)
    assert max(t["width"] for t in v.textures) == 512 and all(t["width"] <= 512 and t["height"] <= 512 for t in v.textures)
    print(f"real a10 hires: {len(v.textures)} variants of {count} textures, {sum(t['pixel_bytes'] for t in v.textures) / 2**20:.1f} MiB")


if __name__ == "__main__":
    test_sizes()
    test_estimate()
    test_table()
    test_hires_blob()
    test_real_a10()
    test_real_hires()
    print("ok")
