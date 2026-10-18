"""Texture conversion on synthetic inputs (no game data needed): quantiser, CLUT order, mip chains,
alpha classes, lightmaps and vertex chroma, shader resolution, BSP scanning, TEXS / LMAP sections.
Set TEX_FIXTURE=<path> to also write the .hp2 the C reader test (engine/tests/test_tex.c) opens.
A skip-if-missing check converts a real map when assets/decompressed/a10.map exists."""
import os
import struct
import sys
import tempfile
from pathlib import Path

sys.path[:0] = [str(Path(__file__).resolve().parents[1])]
from halo2ps2 import bitmaps, bsp, hp2, tagindex, texconv as tc  # noqa: E402

BASE = tagindex.XBOX_TAG_BASE
ROOT = Path(__file__).resolve().parents[2]


def rgba_image(w: int, h: int, fn) -> bytes:
    out = bytearray()
    for y in range(h):
        for x in range(w):
            out += bytes(fn(x, y))
    return bytes(out)


def fails(fn, exc=tc.TexError) -> bool:
    try:
        fn()
    except exc:
        return True
    return False


def decode_texture(t: tc.Ps2Texture, level: int = 0) -> list[tuple[int, int, int, int]]:
    """Palette-resolved RGBA (alpha on the 0x80 scale) of one mip of a converted texture."""
    pal = tc.parse_clut(t.clut, t.clut_entries)
    dims = tc.mip_dims(t.width, t.height)
    w, h = dims[level]
    raw = t.mips[level]
    idx = tc.unpack_nibbles(raw) if t.psm == tc.PSMT4 else raw
    return [pal[i] for i in idx[:w * h]]


# ---- CLUT / packing ------------------------------------------------------------------------
def test_clut() -> None:
    assert [tc.clut_swap(i) for i in (0, 7, 8, 15, 16, 23, 24, 31, 40, 48)] == [0, 7, 16, 23, 8, 15, 24, 31, 48, 40]
    assert all(tc.clut_swap(tc.clut_swap(i)) == i for i in range(256))
    assert sorted(tc.clut_swap(i) for i in range(256)) == list(range(256))
    pal = [(i, 255 - i, i // 2, 255 if i % 2 else 0) for i in range(256)]
    blob = tc.build_clut(pal, 256)
    assert len(blob) == 1024
    words = struct.unpack("<256I", blob)
    # logical entry 8 is stored at raster position 16 (and vice versa): upload as a 16x16 PSMCT32 image
    assert words[16] & 255 == 8 and words[8] & 255 == 16 and words[0] & 255 == 0 and words[24] & 255 == 24
    assert words[5] >> 24 == 128 and words[4] >> 24 == 0                   # alpha 255 -> 0x80, 0 -> 0
    assert tc.parse_clut(blob, 256) == [(r, g, b, 128 if a else 0) for r, g, b, a in pal]
    small = tc.build_clut([(1, 2, 3, 255), (4, 5, 6, 255)], 16)
    assert len(small) == 64 and struct.unpack("<I", small[8:12])[0] == 4 | 5 << 8 | 6 << 16 | 128 << 24
    assert tc.parse_clut(small, 16)[1] == (4, 5, 6, 128)                    # 4-bit CLUT is not swizzled
    assert [tc.ps2_alpha(a) for a in (0, 1, 127, 128, 254, 255)] == [0, 1, 64, 64, 127, 128]
    ramp = struct.unpack("<256I", tc.grey_ramp_clut())
    assert ramp[16] == 8 * 0x01010101 and ramp[8] == 16 * 0x01010101 and ramp[255] == 0xFFFFFFFF
    assert tc.unpack_nibbles(tc.pack_nibbles(bytes(range(16)))) == bytes(range(16))
    assert tc.pack_nibbles(bytes([1, 2, 3, 4])) == bytes([0x21, 0x43])      # even pixel in the low nibble


# ---- resampling ----------------------------------------------------------------------------
def test_sizes() -> None:
    assert [tc.target_size(*s) for s in ((512, 512), (256, 256), (64, 64), (16, 16), (8, 8), (4, 4))] == [
        (256, 256), (128, 128), (32, 32), (8, 8), (8, 8), (8, 8)]
    assert tc.target_size(1024, 512) == (256, 128) and tc.target_size(2048, 64) == (256, 8)
    assert tc.target_size(1024, 1024) == (256, 256) and tc.target_size(512, 16) == (256, 8)
    assert tc.mip_dims(256, 256) == [(256, 256), (128, 128), (64, 64), (32, 32)]       # max 4 levels
    assert tc.mip_dims(64, 64) == [(64, 64), (32, 32), (16, 16), (8, 8)]
    assert tc.mip_dims(128, 8) == [(128, 8)] and tc.mip_dims(16, 16) == [(16, 16), (8, 8)]
    assert tc.mip_dims(8, 8) == [(8, 8)] and tc.mip_dims(32, 16) == [(32, 16), (16, 8)]
    # box filter and the other resamplers
    img = rgba_image(4, 2, lambda x, y: (x * 10 + y * 40, 0, 255, 255))
    half = tc.box2(img, 4, 2, False)
    assert len(half) == 8 and half[0] == (0 + 10 + 40 + 50 + 2) // 4 and half[4] == (20 + 30 + 60 + 70 + 2) // 4
    up = tc.resample_rgba(half, 2, 1, 8, 4, False)
    assert len(up) == 8 * 4 * 4 and up[0:4] == up[4:8] == up[4 * 8 * 3:4 * 8 * 3 + 4]
    down = tc.resample_rgba(img, 4, 2, 2, 2, False)           # one axis only: horizontal average
    assert len(down) == 16 and down[0] == (0 + 10 + 1) >> 1
    # alpha weighting: a transparent black texel must not darken or tint the opaque ones
    px4 = bytes([200, 100, 50, 255, 0, 0, 0, 0, 200, 100, 50, 255, 0, 0, 0, 0])
    w = tc.box2(px4, 2, 2, True)
    assert w[:3] == bytes([200, 100, 50]) and w[3] == 128
    assert tc.box2(px4, 2, 2, False)[0] == 100


def test_alpha() -> None:
    assert tc.alpha_class(bytes([1, 2, 3, 255] * 4)) == tc.CLASS_OPAQUE
    assert tc.alpha_class(bytes([1, 2, 3, 255, 0, 0, 0, 0])) == tc.CLASS_PUNCH
    assert tc.alpha_class(bytes([1, 2, 3, 255, 0, 0, 0, 77])) == tc.CLASS_BLEND
    # retail alpha-tested maps have holes at 0 and solid texels well below 128: Otsu finds the gap
    assert 16 <= tc.alpha_threshold(bytes([0] * 40 + [80] * 60)) <= 80
    assert tc.alpha_threshold(bytes([0] * 50 + [255] * 50)) in range(16, 129)
    assert tc.alpha_threshold(bytes([255] * 10)) == 128
    assert tc.alpha_threshold(bytes([0, 0, 0, 10, 12, 14] * 5)) >= 16


# ---- quantiser / texture conversion --------------------------------------------------------
def many_colours(x: int, y: int) -> tuple[int, int, int, int]:
    i = y * 16 + x                                           # 256 distinct 5/6/5 keys
    return ((i & 31) << 3, (i >> 5) << 2, 0, 255)


def test_quantiser() -> None:
    from collections import Counter
    img = rgba_image(16, 16, many_colours)
    px = tc._to_px(img)
    hist = Counter(p & tc.MASK_RGB for p in px)
    assert len(hist) == 256
    q = tc.Quantiser(hist, 256, tc.MASK_RGB)
    assert len(q.palette) == 256 and q.rms == 0.0
    idx = q.indices([p & tc.MASK_RGB for p in px])
    assert len(set(idx)) == 256
    for i, (r, g, b, a) in enumerate(idx and [q.palette[j] for j in idx]):
        x, y = i % 16, i // 16
        er, eg, _, _ = many_colours(x, y)
        assert abs(r - (er | er >> 5)) == 0 and abs(g - (eg | eg >> 6)) == 0 and b == 0 and a == 255
    q16 = tc.Quantiser(hist, 16, tc.MASK_RGB)
    assert len(q16.palette) == 16 and q16.rms > 5.0
    again = tc.Quantiser(hist, 16, tc.MASK_RGB)
    assert again.palette == q16.palette and again.indices([p & tc.MASK_RGB for p in px]) == \
        q16.indices([p & tc.MASK_RGB for p in px])                                       # deterministic
    # colours that were not in the histogram still classify to a valid entry
    assert 0 <= q16.indices([0x00F8FCF8])[0] < 16
    one = tc.Quantiser(Counter({0x00F8FCF8: 5}), 256, tc.MASK_RGB)
    assert one.palette == [(255, 255, 255, 255)] and one.rms == 0.0 and one.indices([0x00F8FCF8]) == b"\0"
    # alpha dimension: four alpha levels of one colour need four entries
    hist_a = Counter({0x00F8FCF8 | a << 24: 10 for a in (0x00, 0x40, 0x80, 0xF0)})
    qa = tc.Quantiser(hist_a, 4, tc.MASK_RGBA)
    assert sorted(p[3] for p in qa.palette) == [0, 68, 136, 255]


def test_convert() -> None:
    # 256 colours, 16x16: PSMT8 forced, exact, two mips (16, 8), CLUT swap applied, round trip
    t = tc.convert_rgba(0x11, rgba_image(16, 16, many_colours), 16, 16, tc.CLASS_OPAQUE, 8)
    assert (t.psm, t.width, t.height, len(t.mips), t.clut_entries, len(t.clut)) == (tc.PSMT8, 16, 16, 2, 256, 1024)
    assert [len(m) for m in t.mips] == [256, 64] and t.flags == 0 and t.rms == 0.0
    out = decode_texture(t)
    for i, (r, g, b, a) in enumerate(out):
        er, eg, _, _ = many_colours(i % 16, i // 16)
        assert (r, g, b, a) == (er | er >> 5, eg | eg >> 6, 0, 128)
    assert t.pixel_bytes == 320 and t.total_bytes == 1344
    # two flat colours: PSMT4 is chosen by the error metric, pixels are packed two per byte
    flat = rgba_image(32, 32, lambda x, y: (200, 30, 30, 255) if x < 16 else (30, 30, 200, 255))
    t = tc.convert_rgba(1, flat, 32, 32, tc.CLASS_OPAQUE)
    assert t.psm == tc.PSMT4 and len(t.clut) == 64 and len(t.mips) == 3
    assert [len(m) for m in t.mips] == [512, 128, 32]
    out = decode_texture(t)
    assert abs(out[0][0] - 200) <= 8 and abs(out[31][2] - 200) <= 8 and out[0][2] < 60
    assert out[0][:3] != out[31][:3] and out[0] == out[16 * 32] and out[31] == out[31 * 32 + 31]
    # noisy colours fall back to PSMT8
    noisy = rgba_image(32, 32, lambda x, y: ((x * 37 + y * 11) & 255, (x * 91 + y * 53) & 255, (x ^ y) * 8 & 255, 255))
    assert tc.convert_rgba(2, noisy, 32, 32, tc.CLASS_OPAQUE).psm == tc.PSMT8
    # opaque class discards alpha
    t = tc.convert_rgba(3, rgba_image(8, 8, lambda x, y: (10, 20, 30, 7)), 8, 8, tc.CLASS_OPAQUE)
    assert all(p[3] == 128 for p in decode_texture(t)) and len(t.mips) == 1
    # punch-through: entry 0 is the transparent one, opaque texels keep their colour, mips keep the holes
    punch = rgba_image(16, 16, lambda x, y: (0, 0, 0, 0) if x < 8 else (240, 120, 60, 255))
    t = tc.convert_rgba(4, punch, 16, 16, tc.CLASS_PUNCH, 8)
    assert t.flags & tc.TF_PUNCH
    out = decode_texture(t)
    assert out[0][3] == 0 and out[15][3] == 128 and abs(out[15][0] - 240) <= 8
    m1 = decode_texture(t, 1)
    assert m1[0][3] == 0 and m1[7][3] == 128                                          # hole edge stays crisp
    assert tc.parse_clut(t.clut, 256)[0][3] == 0
    t4 = tc.convert_rgba(4, punch, 16, 16, tc.CLASS_PUNCH, 4)
    assert t4.psm == tc.PSMT4 and decode_texture(t4)[0][3] == 0 and decode_texture(t4)[15][3] == 128
    blank = tc.convert_rgba(5, bytes(8 * 8 * 4), 8, 8, tc.CLASS_PUNCH, 8)             # fully transparent
    assert all(p[3] == 0 for p in decode_texture(blank))
    # blend: alpha is a fourth palette dimension
    ramp = rgba_image(16, 16, lambda x, y: (255, 255, 255, x * 16))
    t = tc.convert_rgba(6, ramp, 16, 16, tc.CLASS_BLEND, 8)
    assert t.flags & tc.TF_BLEND
    out = decode_texture(t)
    assert out[0][3] == 0 and out[15][3] == 128 and out[0][3] < out[8][3] < out[15][3]   # alpha keeps 4 bits
    assert tc.convert_rgba(7, ramp, 16, 16, tc.CLASS_BLEND, 8).clut == t.clut                     # deterministic
    # average colour is alpha weighted with GS alpha scale
    avg = tc.convert_rgba(8, punch, 16, 16, tc.CLASS_PUNCH, 8).avg_rgba
    assert (avg & 255, avg >> 8 & 255, avg >> 16 & 255) == (240, 120, 60) and avg >> 24 == 64


# ---- lightmaps -----------------------------------------------------------------------------
def test_lightmaps() -> None:
    assert tc.luminance(bytes([255, 255, 255, 255, 0, 0, 0, 255, 255, 0, 0, 255])) == bytes([255, 0, 77])
    assert tc.lightmap_size(128, 64) == (64, 32) and tc.lightmap_size(16, 8) == (8, 8)
    assert tc.lightmap_size(256, 256, 2) == (32, 32) and tc.lightmap_size(32, 32, 3) == (8, 8)
    page = tc.Rgba(64, 32, rgba_image(64, 32, lambda x, y: (x * 4, x * 4, x * 4, 255)))
    lp = tc.convert_lightmap_page(3, page, 0, 1.0, 1.0)      # explicit linear: the tool's default gain and curve are tunables
    assert (lp.index, lp.width, lp.height, lp.src_width) == (3, 32, 16, 64) and len(lp.pixels) == 512
    assert lp.pixels[0] <= 4
    assert lp.pixels[31] >= 240 and lp.pixels[0] < lp.pixels[16] < lp.pixels[31]
    assert lp.avg_luma == sum(lp.pixels) // len(lp.pixels) and lp.avg_rgba >> 24 == 255
    boosted = tc.convert_lightmap_page(0, page, 0, 2.0, 1.0)
    assert abs(boosted.pixels[8] - 2 * lp.pixels[8]) <= 1 and max(boosted.pixels) == 255
    tiny = tc.convert_lightmap_page(0, tc.Rgba(8, 8, bytes([9, 9, 9, 255]) * 64), 0, 1.0, 1.0)
    assert (tiny.width, tiny.height, len(tiny.pixels)) == (8, 8, 64) and set(tiny.pixels) == {9}


def test_lightmap_curve() -> None:
    assert tc.lightmap_curve(100, 1.0, 1.0) == 100 and tc.lightmap_curve(200, 1.5, 1.0) == 255
    # the curve: the white point maps to 127 x gain, darker texels follow a power law and stay above the linear ramp at low light
    assert tc.lightmap_curve(int(tc.LM_WHITE), 1.0) == 127 and tc.lightmap_curve(255, 1.0) == 127
    assert tc.lightmap_curve(int(tc.LM_WHITE), 1.5) in (190, 191) and tc.lightmap_curve(0, 1.5) == 0
    quarter = tc.lightmap_curve(int(tc.LM_WHITE / 4), 1.0)
    assert abs(quarter - 127 * 0.25 ** tc.LM_GAMMA) <= 1 and quarter > 127 // 4


def test_vertex_chroma() -> None:
    full = lambda page, uv: tc.vertex_chroma(page, uv, 1.0)     # strength 1: the exact page chroma
    grey = tc.Rgba(4, 4, bytes([100, 100, 100, 255]) * 16)
    assert full(grey, (0.3, 0.7)) == (128, 128, 128)
    red = tc.Rgba(2, 2, bytes([160, 100, 100, 255]) * 4)
    r, g, b = full(red, (0.5, 0.5))
    y = 0.299 * 160 + 0.587 * 100 + 0.114 * 100
    assert all(abs(a - e) <= 1 for a, e in zip((r, g, b), (160 / y * 128, 100 / y * 128, 100 / y * 128)))
    assert r > 128 > g == b
    # the tint is luminance independent: halving the brightness leaves the chroma unchanged
    dark = tc.Rgba(2, 2, bytes([80, 50, 50, 255]) * 4)
    assert all(abs(a - b) <= 1 for a, b in zip(full(dark, (0.5, 0.5)), (r, g, b)))
    # saturated colours clamp to 255, black is neutral
    assert full(tc.Rgba(1, 1, bytes([200, 50, 50, 255])), (0.5, 0.5))[0] == 255
    blue = tc.Rgba(2, 2, bytes([0, 0, 255, 255]) * 4)
    assert full(blue, (0.5, 0.5)) == (0, 0, 255)
    assert full(tc.Rgba(2, 2, bytes(16)), (0.5, 0.5)) == (128, 128, 128)
    # bilinear between a black-free pair of texels, and uv outside 0..1 clamps to the edge texel
    two = tc.Rgba(2, 1, bytes([200, 100, 100, 255, 100, 100, 200, 255]))
    left, right = full(two, (-5.0, 0.5)), full(two, (9.0, 0.5))
    assert left[0] > left[2] and right[2] > right[0] and left == full(two, (0.0, 0.0))
    mid = full(two, (0.5, 0.5))                     # exactly between the texel centres
    assert abs(mid[0] - mid[2]) <= 1 and right[2] > mid[2] > left[2] and left[0] > mid[0] > right[0]
    assert full((2, 2, bytes([90, 90, 90, 255]) * 4), (0.1, 0.9)) == (128, 128, 128)   # plain tuple


# ---- sections ------------------------------------------------------------------------------
    # the default strength pulls the tint towards neutral (the a10 lightmaps are amber, the Xbox walls are not)
    soft = tc.vertex_chroma(red, (0.5, 0.5))
    assert 128 < soft[0] < r and g < soft[1] <= 128 and tc.CHROMA_STRENGTH < 1.0
    assert abs(soft[0] - (128 + tc.CHROMA_STRENGTH * (r - 128))) <= 1

def fixture() -> tuple[bytes, bytes, list[tc.Ps2Texture], list[tc.ShaderInfo], list[tc.LightmapPage]]:
    t0 = tc.convert_rgba(0xE1780001, rgba_image(16, 16, many_colours), 16, 16, tc.CLASS_OPAQUE, 8)
    punch = rgba_image(32, 32, lambda x, y: (0, 0, 0, 0) if x < 16 else (200, 40, 40, 255))
    t1 = tc.convert_rgba(0xE1780002, punch, 32, 32, tc.CLASS_PUNCH, 4)
    t2 = tc.convert_rgba(0xE1780003, rgba_image(8, 8, lambda x, y: (255, 200, 0, y * 32)), 8, 8, tc.CLASS_BLEND, 8)
    shaders = [
        tc.ShaderInfo(0xE1740030, "sotr", "b", bitmap_id=0xE1780001, blend=tc.BLEND_ADD, uv_scale=(8.0, 4.0), texture=0),
        tc.ShaderInfo(0xE1740010, "senv", "a", bitmap_id=0xE1780002, blend=tc.BLEND_ALPHA_TEST,
                      flags=tc.BF_ALPHA_TEST | tc.BF_TWO_SIDED, texture=1),
        tc.ShaderInfo(0xE1740002, None, "c", flags=tc.BF_UNTEXTURED),
        tc.ShaderInfo(0xE1740020, "sgla", "d", bitmap_id=0xE1780003, blend=tc.BLEND_ALPHA, texture=2),
    ]
    pages = [tc.convert_lightmap_page(0, tc.Rgba(64, 32, rgba_image(64, 32, lambda x, y: (x * 4, x * 4, x * 4, 255)))),
             tc.convert_lightmap_page(2, tc.Rgba(16, 16, bytes([90, 90, 90, 255]) * 256), 0, 1.0, 1.0)]
    return tc.build_texs([t0, t1, t2], shaders), tc.build_lmap(pages, 1.0), [t0, t1, t2], shaders, pages


def test_sections() -> None:
    blob, lmblob, textures, shaders, pages = fixture()
    assert len(blob) % 16 == 0 and len(lmblob) % 16 == 0
    v = tc.parse_texs(blob)
    assert len(v.textures) == 3 and [b["shader_id"] for b in v.bindings] == [0xE1740002, 0xE1740010, 0xE1740020, 0xE1740030]
    untex, alpha, glass, glow = v.bindings
    assert untex["texture"] == tc.NO_TEXTURE and untex["flags"] & tc.BF_UNTEXTURED and untex["flat_rgba"] == tc.FLAT_GREY
    assert alpha["texture"] == 1 and alpha["flags"] == tc.BF_ALPHA_TEST | tc.BF_TWO_SIDED and alpha["blend"] == tc.BLEND_ALPHA_TEST
    assert glow["uv_scale"] == (8.0, 4.0) and glow["group"] == tc.GROUP_CODES["sotr"] and glow["texture"] == 0
    assert glass["blend"] == tc.BLEND_ALPHA and glass["flat_rgba"] == textures[2].avg_rgba
    t0, t1, t2 = v.textures
    assert (t0["psm"], t0["width"], t0["mips"], t0["clut_entries"]) == (tc.PSMT8, 16, 2, 256)
    assert (t1["psm"], t1["width"], t1["mips"], t1["clut_entries"], t1["flags"]) == (tc.PSMT4, 32, 3, 16, tc.TF_PUNCH)
    assert t2["flags"] == tc.TF_BLEND
    for t, src in zip(v.textures, textures):
        assert v.data[t["clut_offset"]:t["clut_offset"] + len(src.clut)] == src.clut
        for lvl, mip in enumerate(src.mips):
            w, h, data = v.mip(t, lvl)
            assert data == mip and (w, h) == tc.mip_dims(src.width, src.height)[lvl]
        assert t["clut_offset"] % 16 == 0 and t["pixel_offset"] % 16 == 0
    # corruption of every table is rejected
    def flip(off: int, val: int, size: int = 1, data: bytes = blob) -> bytes:
        b = bytearray(data)
        b[off:off + size] = val.to_bytes(size, "little")
        return bytes(b)
    to, bo = tc.HEADER_SIZE, tc.HEADER_SIZE + 3 * tc.TEXTURE_REC
    bad = [blob[:30], blob[:-16], b"NOPE" + blob[4:], flip(4, 2, 2), flip(8, 0x20000, 4), flip(12, 0x20000, 4),
           flip(16, 4, 4), flip(20, 4000, 4), flip(24, 4, 4), flip(28, len(blob) + 16, 4),
           flip(to + 8, 0x12), flip(to + 4, 17, 2), flip(to + 4, 512, 2), flip(to + 9, 0), flip(to + 9, 5),
           flip(to + 12, 1024, 4), flip(to + 16, 0, 4), flip(to + 20, len(blob), 4), flip(to + 24, 7, 4),
           flip(bo, 0xE1740030, 4), flip(bo + 32 + 4, 9, 2), flip(bo + 4, 7, 2)]
    for n, b in enumerate(bad):
        assert fails(lambda b=b: tc.parse_texs(b)), f"corrupt TEXS #{n} accepted"
    assert tc.parse_texs(flip(bo + 6, 0x80, 2)).bindings[0]["flags"] == 0x80       # flags are free-form

    lm = tc.parse_lmap(lmblob)
    assert len(lm["pages"]) == 3 and lm["pages"][1] is None and lm["used"] == 2 and lm["gain"] == 1.0
    p0, p2 = lm["pages"][0], lm["pages"][2]
    assert (p0["width"], p0["height"], p0["src_width"]) == (32, 16, 64) and (p2["width"], p2["height"]) == (8, 8)
    assert lm["data"][p0["pixel_offset"]:p0["pixel_offset"] + p0["pixel_bytes"]] == pages[0].pixels
    assert lm["clut"] == tc.grey_ramp_clut() and p2["avg_luma"] == 90
    lo, lt = tc.HEADER_SIZE, tc.HEADER_SIZE + 3 * tc.PAGE_REC
    badlm = [lmblob[:30], lmblob[:-16], b"NOPE" + lmblob[4:], flip(4, 2, 2, lmblob), flip(8, 99, 4, lmblob),
             flip(16, 100, 4, lmblob), flip(24, 5, 4, lmblob), flip(lo, 33, 2, lmblob),
             flip(lo + 4, tc.PSMT4, 1, lmblob), flip(lo + 8, 4, 4, lmblob), flip(lo + 12, 9999, 4, lmblob),
             flip(lt + 100, 1, 1, lmblob)[:-16]]
    for b in badlm:
        assert fails(lambda b=b: tc.parse_lmap(b)), "corrupt LMAP accepted"
    # packs round-trip through the container
    pack = hp2.build("t", [("TEXS", blob), ("LM0", lmblob)])
    pk = hp2.parse(pack)
    assert tc.parse_texs(pk.read("TEXS")).bindings == v.bindings and tc.parse_lmap(pk.read("LM0"))["used"] == 2
    if os.environ.get("TEX_FIXTURE"):
        Path(os.environ["TEX_FIXTURE"]).write_bytes(pack)


# ---- shader resolution on a synthetic tag region -------------------------------------------------
def synthetic_map(tmp: Path) -> tuple[tc.MapData, dict[str, int]]:
    tags = [("scnr", "levels\\t\\t", 0x600), ("bitm", "levels\\t\\bitmaps\\base", 0x6C + 48),
            ("bitm", "levels\\t\\bitmaps\\cube", 0x6C + 48),
            ("senv", "levels\\t\\shaders\\wall", 836), ("senv", "levels\\t\\shaders\\grate", 836),
            ("senv", "levels\\t\\shaders\\selfillum", 836),
            ("sotr", "levels\\t\\shaders\\light", 108 + 100), ("schi", "levels\\t\\shaders\\river", 108 + 220),
            ("sgla", "levels\\t\\shaders\\glass", 480), ("swat", "levels\\t\\shaders\\water", 624),
            ("smet", "levels\\t\\shaders\\meter", 100), ("sotr", "levels\\t\\shaders\\nomaps", 108),
            ("schi", "levels\\t\\shaders\\sky", 108 + 3 * 220), ("schi", "levels\\t\\shaders\\river2", 108 + 2 * 220)]
    region = bytearray(tagindex.TagIndex.build(tags))
    idx = tagindex.TagIndex.parse_region(bytes(region), 0)
    ent = {e.path.split("\\")[-1]: e for e in idx.entries}
    off = lambda name: ent[name].data_address - BASE  # noqa: E731

    def ref(at: int, name: str | None) -> None:
        tid = ent[name].tag_id if name else 0xFFFFFFFF
        struct.pack_into("<4sIII", region, at, b"mtib", 0x80000000, 0, tid)
    # bitmap "base": one dxt1 image 64x64 (+ 6 mips) whose pixels sit in the temp file at offset 1000
    for name, typ in (("base", 0), ("cube", 2)):
        b = off(name)
        struct.pack_into("<II", region, b + 0x60, 1, ent[name].data_address + 0x6C)
        struct.pack_into("<4s6HH", region, b + 0x6C, b"mtib", 64, 64, 1, typ, 14, 0x83, 0)
        struct.pack_into("<H", region, b + 0x6C + 0x14, 6)
        struct.pack_into("<II", region, b + 0x6C + 0x18, 1000, 2816)
    w = off("wall")
    struct.pack_into("<HH", region, w + 0x28, 0, 1)
    ref(w + 0x88, "base")
    g = off("grate")
    struct.pack_into("<HH", region, g + 0x28, 1, 0)
    ref(g + 0x88, "base")
    ref(off("selfillum") + 0x88, None)
    s = off("light")                       # sotr: flags decal|two-sided, blend add, map scale 3 x 5
    struct.pack_into("<BBHH", region, s + 0x28, 0, 0b110, 0, 3)
    struct.pack_into("<II", region, s + 0x54, 1, ent["light"].data_address + 108)
    struct.pack_into("<ff", region, s + 108 + 0x04, 3.0, 5.0)
    ref(s + 108 + 0x1C, "base")
    r = off("river")                       # schi: map scale 2 x 4 at +0x54/+0x58, bitmap at +0x6C
    struct.pack_into("<BBHH", region, r + 0x28, 0, 0b10000, 0, 0)
    struct.pack_into("<II", region, r + 0x54, 1, ent["river"].data_address + 108)
    struct.pack_into("<ff", region, r + 108 + 0x54, 2.0, 4.0)
    ref(r + 108 + 0x6C, "base")
    k = off("sky")                         # schi, 3 maps: mask x3 (multiply, alpha current), stars x16x10 (add), backdrop x1
    struct.pack_into("<BBHH", region, k + 0x28, 0, 0, 0, 0)
    struct.pack_into("<II", region, k + 0x54, 3, ent["sky"].data_address + 108)
    for i, (cf, af, su, sv, bm) in enumerate(((2, 0, 3.0, 3.0, "base"), (4, 4, 16.0, 10.0, "cube"), (0, 0, 1.0, 1.0, "base"))):
        mo = k + 108 + i * 220
        struct.pack_into("<HH", region, mo + 0x2C, cf, af)
        struct.pack_into("<ff", region, mo + 0x54, su, sv)
        ref(mo + 0x6C, bm)
    r2 = off("river2")                     # schi, 2 maps: colour+alpha mask (multiply, alpha multiply) x1, water x1x4
    struct.pack_into("<BBHH", region, r2 + 0x28, 0, 0, 0, 0)
    struct.pack_into("<II", region, r2 + 0x54, 2, ent["river2"].data_address + 108)
    for i, (cf, af, su, sv, bm) in enumerate(((2, 2, 1.0, 1.0, "cube"), (3, 3, 1.0, 4.0, "base"))):
        mo = r2 + 108 + i * 220
        struct.pack_into("<HH", region, mo + 0x2C, cf, af)
        struct.pack_into("<ff", region, mo + 0x54, su, sv)
        ref(mo + 0x6C, bm)
    gl = off("glass")
    struct.pack_into("<f", region, gl + 0x154, 6.0)
    ref(gl + 0x158, "base")
    ref(off("water") + 0x4C, "cube")       # cube map base: resolved, but not decodable later
    struct.pack_into("<II", region, off("nomaps") + 0x54, 0, 0)
    data = bytes(region)
    mapfile = tmp / "t.map"
    # pixels: a flat red DXT1 mip chain at file offset 1000
    red = struct.pack("<HHI", 0xF800, 0xF800, 0)
    mapfile.write_bytes(bytes(1000) + red * 256 + struct.pack("<HHI", 0x001F, 0x001F, 0) * 64 + bytes(768))
    m = tc.MapData(mapfile, None, idx, data, {e.tag_id: e for e in idx.entries})  # type: ignore[arg-type]
    return m, {k: v.tag_id for k, v in ent.items()}


def test_shaders() -> None:
    with tempfile.TemporaryDirectory() as d:
        m, ids = synthetic_map(Path(d))
        wall = tc.resolve_shader(m, ids["wall"])
        assert wall.group == "senv" and wall.bitmap_id == ids["base"] and not wall.alpha_test
        assert wall.flags == tc.BF_BLENDED_BASE and wall.blend == tc.BLEND_OPAQUE and wall.uv_scale == (1.0, 1.0)
        assert tc.klass_cap_for(wall) == tc.CLASS_OPAQUE
        grate = tc.resolve_shader(m, ids["grate"])
        assert grate.alpha_test and grate.blend == tc.BLEND_ALPHA_TEST and tc.klass_cap_for(grate) == tc.CLASS_PUNCH
        selfi = tc.resolve_shader(m, ids["selfillum"])
        assert selfi.bitmap_id is None and selfi.flags & tc.BF_UNTEXTURED and selfi.note == "no base map"
        light = tc.resolve_shader(m, ids["light"])
        assert light.bitmap_id == ids["base"] and light.blend == tc.BLEND_ADD and light.uv_scale == (3.0, 5.0)
        assert light.flags == tc.BF_DECAL | tc.BF_TWO_SIDED and not light.alpha_test
        assert tc.klass_cap_for(light) == tc.CLASS_BLEND
        river = tc.resolve_shader(m, ids["river"])
        assert river.uv_scale == (2.0, 4.0) and river.bitmap_id == ids["base"] and river.blend == tc.BLEND_ALPHA
        assert tc.klass_cap_for(river) == tc.CLASS_BLEND
        sky = tc.resolve_shader(m, ids["sky"])          # the mask is skipped, the x1 backdrop beats the x16 stars
        assert sky.bitmap_id == ids["base"] and sky.uv_scale == (1.0, 1.0) and tc.chicago_base_map(m, m.tag_off(m.by_id[ids["sky"]]) + 108, 3) == 2
        assert tc.chicago_base_map(m, m.tag_off(m.by_id[ids["sky"]]) + 108, 2) == 1        # with only mask + stars: the stars
        river2 = tc.resolve_shader(m, ids["river2"])    # a modulator that supplies the alpha stays the base
        assert river2.bitmap_id == ids["cube"] and river2.uv_scale == (1.0, 1.0)
        glass = tc.resolve_shader(m, ids["glass"])
        assert glass.bitmap_id == ids["base"] and glass.uv_scale == (6.0, 6.0) and glass.blend == tc.BLEND_ALPHA
        water = tc.resolve_shader(m, ids["water"])
        assert water.bitmap_id == ids["cube"] and water.blend == tc.BLEND_ALPHA and tc.klass_cap_for(water) == tc.CLASS_OPAQUE
        meter = tc.resolve_shader(m, ids["meter"])
        assert meter.bitmap_id is None and meter.flags & tc.BF_UNTEXTURED and "unsupported" in meter.note
        assert tc.resolve_shader(m, ids["nomaps"]).bitmap_id is None
        unk = tc.resolve_shader(m, 0x12345678)
        assert unk.group is None and unk.flags & tc.BF_UNTEXTURED
        # conversion reads the pixels from the file, uses Xbox mip 1 (blue) because 64 -> 32 wants half size
        img = m.images(ids["base"])[0]
        t = tc.convert_texture(m, img, ids["base"], tc.CLASS_OPAQUE)
        assert (t.width, t.height) == (32, 32) and t.psm == tc.PSMT4 and len(t.mips) == 3
        r, g, b, a = decode_texture(t)[0]
        assert b > 200 and r < 30 and a == 128                                  # the level-1 colour
        assert tc.convert_texture(m, img, ids["base"], tc.CLASS_BLEND).flags == 0        # dxt1 w/o holes: opaque
        assert m.images(ids["wall"]) == [] and m.images(0xDEAD) == []
        assert not m.images(ids["cube"])[0].decodable


# ---- BSP scanning --------------------------------------------------------------------------------
def test_scan_bsp() -> None:
    load = 0x81000000
    blk = bytearray(bsp.HEADER_SIZE)
    root = len(blk)
    blk += bytes(0x27C)
    lms = len(blk)
    blk += bytes(bsp.LIGHTMAP_SIZE * 3)
    mats = len(blk)
    blk += bytes(bsp.MATERIAL_SIZE * 4)

    def material(i: int, shader: int, surfaces: int, rendered: int, lm_verts: int) -> None:
        o = mats + i * bsp.MATERIAL_SIZE
        struct.pack_into("<I", blk, o + 0x0C, shader)
        struct.pack_into("<ii", blk, o + 0x14, 0, surfaces)
        struct.pack_into("<I", blk, o + 0xB4, rendered)
        struct.pack_into("<I", blk, o + 0xC8, lm_verts)
    material(0, 0xE1740001, 10, 30, 30)      # lightmapped, page 4
    material(1, 0xE1740001, 5, 12, 12)       # same shader, page 4
    material(2, 0xE1740002, 0, 12, 12)       # no surfaces: skipped
    material(3, 0xE1740003, 8, 20, 0)        # no lightmap vertices: page not referenced
    struct.pack_into("<HH", blk, lms, 4, 0)
    struct.pack_into("<II", blk, lms + 0x14, 3, load + mats)
    struct.pack_into("<HH", blk, lms + 32, 0xFFFF, 0)                    # records with no page
    struct.pack_into("<II", blk, lms + 32 + 0x14, 1, load + mats + 3 * bsp.MATERIAL_SIZE)
    struct.pack_into("<5I4s", blk, 0, load + root, 4, 0, 4, 0, bsp.SBSP_MAGIC)
    struct.pack_into("<4sIII", blk, root, b"mtib", 0, 0, 0xEE390CC0)     # lightmaps bitmap reference
    struct.pack_into("<II", blk, root + 0x104, 3, load + lms)
    loc = bsp.BspLocation(0x800, len(blk), load, "levels\\t\\t")
    info = tc.scan_bsp(2, loc, bytes(blk))
    assert info.index == 2 and info.lightmap_tag == 0xEE390CC0
    assert info.shaders == {0xE1740001: 2, 0xE1740003: 1} and info.pages == {4}
    struct.pack_into("<I", blk, root + 12, 0xFFFFFFFF)
    assert tc.scan_bsp(0, loc, bytes(blk)).lightmap_tag is None
    struct.pack_into("<II", blk, root + 0x104, 3, load + len(blk))        # records outside the block
    assert fails(lambda: tc.scan_bsp(0, loc, bytes(blk)))
    assert fails(lambda: tc.scan_bsp(0, bsp.BspLocation(0x800, len(blk), load + 0x100000, "x"), bytes(blk)))


# ---- optional: a real map ---------------------------------------------------------------------------
def test_real_map() -> None:
    path = ROOT / "assets" / "decompressed" / "a10.map"
    if not path.exists():
        print("skip: no a10.map for the real-data check")
        return
    res = tc.convert_map(path, bsp_filter=[0, 1])
    v = tc.parse_texs(hp2.parse(res.pack).read("TEXS"))
    assert len(v.textures) >= 10 and len(v.bindings) >= 20
    assert res.untextured_materials < 0.1 * sum(sum(b.shaders.values()) for b in res.bsps)
    total = sum(t.total_bytes for t in res.textures)
    assert total < 6 * 2 ** 20
    for i, pages in res.lightmaps.items():
        assert sum(len(p.pixels) for p in pages) <= tc.LM_BUDGET_BYTES
        lm = tc.parse_lmap(hp2.parse(res.pack).read(f"LM{i}"))
        assert all(lm["pages"][p.index] for p in pages)
    # lightmap decode check: texels under real BSP vertices are never the atlas fill colour
    # (this also validates the uv orientation: v = 0 is the first row)
    try:
        from halo2ps2 import bspconv
        m = tc.MapData.open(path)
        info = tc.scan_map(m)[1]
        pages = tc.lightmap_pages_rgba(m, info)
        loc = bsp.locate(m.index, m.region, m.header.decompressed_size)[1]
        with path.open("rb") as f:
            f.seek(loc.file_offset)
            mats, _ = bspconv.read_materials(f.read(loc.size), loc)
    except (ImportError, AttributeError, TypeError):
        print("skip: bspconv.read_materials unavailable for the lightmap uv check")
        return
    bad = tot = 0
    for mt in mats:
        pg = pages.get(mt.lightmap_page)
        if pg is None:
            continue
        for u, v in mt.lmuv[:100]:
            x, y = min(pg.width - 1, int(u * pg.width)), min(pg.height - 1, int(v * pg.height))
            tot += 1
            bad += tuple(pg.pixels[(y * pg.width + x) * 4:(y * pg.width + x) * 4 + 3]) in ((255, 252, 0), (255, 0, 0))
    assert tot > 1000 and bad <= tot * 0.01, (bad, tot)


def main() -> None:
    test_clut()
    test_sizes()
    test_alpha()
    test_quantiser()
    test_convert()
    test_lightmaps()
    test_lightmap_curve()
    test_vertex_chroma()
    test_sections()
    test_shaders()
    test_scan_bsp()
    test_real_map()
    print("ok")


if __name__ == "__main__":
    main()
