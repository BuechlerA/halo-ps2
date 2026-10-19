"""Hero textures (objtex.HERO, ADR-073): sizing and budget rules on plain numbers, and on the real a10 map (skipped without assets/decompressed/a10.map): the command display
layers, the uniforms and Cortana keep up to 256 texels per side inside their own allowance, and the ordinary textures keep the size their budget gives them."""
import sys
from pathlib import Path

sys.path[:0] = [str(Path(__file__).resolve().parents[1])]
from halo2ps2 import objtex, texconv  # noqa: E402


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


if __name__ == "__main__":
    test_sizes()
    test_estimate()
    test_table()
    test_real_a10()
    print("ok")
