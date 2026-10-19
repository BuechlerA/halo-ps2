#!/usr/bin/env python3
"""Convert the textures a map's structure BSPs need into PS2-ready ``TEXS`` / ``LMn`` sections.

Output format: docs/09-tex-section.md.  Bitmap decoding: bitmaps.py.  Pure stdlib.

Pipeline per map
    1. every ``shader_id`` of every BSP lightmap material is resolved to its base-map bitmap
       tag, blend mode, two-sidedness and uv scale (``resolve_shader``);
    2. each distinct (bitmap, alpha class) pair is converted once (``convert_texture``):
       half resolution (power of two, 8..256, Xbox mip 1 is used when present), alpha reduced to
       opaque / punch-through / blend, median-cut palette (PSMT8, or PSMT4 when a 16 colour
       palette is nearly as good), mip chain down to 8 px (<= 4 levels), CLUT in CSM1 order;
    3. every BSP's lightmap bitmap pages become 8-bit luminance pages (``convert_lightmap``);
       the chroma is left to the geometry converter, which samples ``vertex_chroma``.

Shader layouts (engine version 5; offsets inside the shader tag's data block; verified on the
shaders used by BSP materials of a10 a30 a50 b30 b40 c10 c20 c40 d20 d40).  A tag reference is
16 bytes: group (byte reversed), path pointer, path length, tag id (the id is what is read).

    all shaders   +0x24 u16 shader type (3 = environment); header ends at +0x28
    senv (836 B)  +0x28 u16 flags (bit0 alpha-tested), +0x2A u16 type (0 normal, 1 blended,
                  2 blended base specular), +0x88 base map reference.  Detail maps are at +0xB8
                  (primary, scale f32 at +0xB4), +0xCC (secondary, scale +0xC8), +0xFC (micro,
                  scale +0xF8), bump map +0x128 (scale +0x124), self illumination +0x254,
                  reflection cube map +0x324; none of them is used by PS2 materials.  There is no
                  base map uv scale: uv scale is 1.
    sotr, schi    (transparent generic / chicago; fixed part 0x6C bytes, maps follow inline)
                  +0x28 u8 numeric counter limit, +0x29 u8 flags (bit0 alpha-tested, bit1 decal,
                  bit2 two-sided, bit4 draw before water), +0x2A u16 first map type,
                  +0x2C u16 framebuffer blend function (0 alpha blend, 1 multiply, 2 double
                  multiply, 3 add, 4 subtract, 5 min, 6 max, 7 alpha-multiply add),
                  +0x54 reflexive of maps {count, address}.
                  sotr map (100 B): +0x04 u scale, +0x08 v scale, +0x1C bitmap reference.
                  schi map (220 B): +0x2C u16 colour function and +0x2E u16 alpha function (how the
                  next map combines with this one: 0 current, 1 next map, 2 multiply, 3 double multiply,
                  4 add, ...), +0x54 u scale, +0x58 v scale, +0x6C bitmap reference.
                  sotr: the first map is the base map. schi: ``chicago_base_map`` skips the maps that only
                  modulate the ones after them (multiply / double multiply with no alpha of their own:
                  the star mask of the space sky, detail tiles) and takes the least tiled of the rest.
    sgla (480 B)  +0x28 u16 flags, +0x154 f32 diffuse scale, +0x158 diffuse map reference.
    swat (624 B)  +0x28 u16 flags, +0x4C base map reference (no scale).
Everything else (scex, smet, spla, soso, ...) is not used by BSP materials in the retail data and
resolves to "untextured" (flat grey) and is counted.

Usage:
    python3 -I tools/halo2ps2/texconv.py assets/decompressed/a10.map -o assets/hp2/a10.tex.hp2
    python3 -I tools/halo2ps2/texconv.py --info assets/hp2/a10.tex.hp2
"""
from __future__ import annotations

import argparse
import struct
import sys
import time
from array import array
from collections import Counter
from dataclasses import dataclass, field
from pathlib import Path
from typing import NamedTuple

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from halo2ps2 import bitmaps, bsp, hp2, mapinfo, tagindex  # noqa: E402

_LE = sys.byteorder == "little"

# ---- constants shared with docs/09-tex-section.md ------------------------------------------
TEXS_MAGIC, LMAP_MAGIC = b"TEXS", b"LMAP"
VERSION = 1
HEADER_SIZE = 64
TEXTURE_REC = 32
BINDING_REC = 32
PAGE_REC = 32
PSMT8, PSMT4 = 0x13, 0x14          # GS pixel storage formats
NO_TEXTURE = 0xFFFF

TF_PUNCH, TF_BLEND, TF_UPSCALED = 1, 2, 4          # texture flags
CLASS_OPAQUE, CLASS_PUNCH, CLASS_BLEND = 0, 1, 2

BF_ALPHA_TEST, BF_TWO_SIDED, BF_DECAL, BF_UNTEXTURED, BF_SKY, BF_BLENDED_BASE = 1, 2, 4, 8, 0x10, 0x20
(BLEND_OPAQUE, BLEND_ALPHA_TEST, BLEND_ALPHA, BLEND_ADD, BLEND_MULTIPLY, BLEND_DOUBLE_MULTIPLY,
 BLEND_SUBTRACT) = range(7)
GROUP_CODES = {"senv": 1, "sotr": 2, "schi": 3, "sgla": 4, "swat": 5}
FLAT_GREY = 0x80808080               # RGBA8, alpha on the PS2 0x80 = 1.0 scale

MAX_TEX, MIN_TEX, MAX_MIPS = 256, 8, 4
MAX_TEX_FORMAT = 512       # the TEXS format allows 512 on a side; the converter's default cap is MAX_TEX, only HERO_BITMAPS go above it (ADR-073)
# Bitmaps (tag paths) of surfaces a cinematic shows large that a half resolution strip would smear: the a10 command wall's monitors (1024 x 256, three screens side by side)
# get 512 x 128 instead of 256 x 64.
HERO_BITMAPS = {"levels\\a10\\bitmaps\\monitors": 512,
                "scenery\\vehicles\\poa\\bitmaps\\poa-registry": 512}       # the Pillar of Autumn's name on its hull (1024 x 128): readable in the intro's flyby
# Bitmaps (tag path prefixes) whose environment shader's bump map (a p8-bump height field) is baked into the colour map as fixed-light relief shading: the Xbox lights the
# Pillar's hull plates through the bump map (dot3), which is what gives them their edges; here the lightmap is all there is, and a source that is a soft mottle magnified
# looks like noise. BUMP_LIGHT = the direction the relief is lit from (texture space, towards the light), BUMP_STRENGTH how many source levels of height change one 1.0 of shade.
BUMP_BAKE = ("scenery\\vehicles\\poa\\bitmaps\\",)
BUMP_STRENGTH = 1.1
BUMP_GAIN = 0.9                       # measured: the lit hull came out 10 .. 13 % brighter than the Xbox video's at ticks 1050 / 1060 (same hue)
BUMP_KEY = 1 << 41
SENV_BUMP_MAP, SENV_BUMP_SCALE = 0x128, 0x124
LM_BUDGET_BYTES = 512 * 1024         # per BSP, after conversion (see docs/09-tex-section.md)
LM_DEFAULT_GAIN = 1.0
CHROMA_STRENGTH = 0.4                # how much of the lightmap colour reaches the vertices and the object lighting (1.0 = all of it)
LM_GAMMA = 0.5                       # lightmap luminance tone curve exponent (1.0 = linear: index = luma x gain)
LM_WHITE = 249.0                     # the brightest lightmap texel of the retail maps (a10: 249): the curve's 1.0
PSMT4_MAX_RMS = 3.0                  # 16 colours are used when they reproduce the image within this RMS

# ---- shader tag layout --------------------------------------------------------------------
SENV_FLAGS, SENV_TYPE, SENV_BASE_MAP, SENV_SIZE = 0x28, 0x2A, 0x88, 836
TRANSPARENT_FLAGS, TRANSPARENT_BLEND, TRANSPARENT_MAPS = 0x29, 0x2C, 0x54
SOTR_MAP = (100, 0x04, 0x08, 0x1C)      # element size, u scale, v scale, bitmap ref
SCHI_MAP = (220, 0x54, 0x58, 0x6C)
SCHI_COLOR_FN, SCHI_ALPHA_FN = 0x2C, 0x2E   # per schi map: how the *next* map combines with it (u16 each)
CF_MULTIPLY, CF_DOUBLE_MULTIPLY = 2, 3      # colour functions that make a map a modulator of what follows
AF_CURRENT, AF_NEXT_MAP = 0, 1              # alpha functions under which the map contributes no alpha of its own
SGLA_SCALE, SGLA_MAP, SWAT_MAP = 0x154, 0x158, 0x4C
BLEND_FROM_FRAMEBUFFER = {0: BLEND_ALPHA, 1: BLEND_MULTIPLY, 2: BLEND_DOUBLE_MULTIPLY, 3: BLEND_ADD,
                          4: BLEND_SUBTRACT, 5: BLEND_ALPHA, 6: BLEND_ALPHA, 7: BLEND_ADD}


class TexError(ValueError):
    pass


# ---- map access ----------------------------------------------------------------------------
@dataclass
class MapData:
    path: Path
    header: mapinfo.MapHeader
    index: tagindex.TagIndex
    region: bytes
    by_id: dict[int, tagindex.TagEntry]

    @classmethod
    def open(cls, path: Path) -> "MapData":
        hdr = mapinfo.read_header(path)
        idx = tagindex.TagIndex.read(path)
        with path.open("rb") as f:
            f.seek(hdr.tag_data_offset)
            region = f.read(hdr.tag_data_size)
        return cls(path, hdr, idx, region, {e.tag_id: e for e in idx.entries})

    @property
    def base(self) -> int:
        return self.index.base

    def tag_off(self, entry: tagindex.TagEntry) -> int:
        return entry.data_address - self.base

    def pixels(self, img: bitmaps.BitmapImage) -> bytes:
        """The image's raw pixel bytes from the map file."""
        with self.path.open("rb") as f:
            f.seek(img.pixel_offset)
            data = f.read(img.pixel_size)
        if len(data) != img.pixel_size:
            raise bitmaps.BitmapError(f"pixel range {img.pixel_offset:#x}+{img.pixel_size:#x} outside the map")
        return data

    def images(self, tag_id: int) -> list[bitmaps.BitmapImage]:
        e = self.by_id.get(tag_id)
        if e is None or e.group != "bitm":
            return []
        return bitmaps.parse_images(self.region, self.base, e.data_address)


# ---- shader resolution -----------------------------------------------------------------------
@dataclass
class ShaderInfo:
    shader_id: int
    group: str | None
    path: str
    bitmap_id: int | None = None         # tag id of the base map bitmap
    flags: int = 0                       # BF_*
    blend: int = BLEND_OPAQUE
    uv_scale: tuple[float, float] = (1.0, 1.0)
    alpha_test: bool = False
    keep_alpha: bool = False             # base map alpha is transparency (not a specular mask)
    note: str = ""                       # why it is untextured, if it is
    change_source: int = 0               # colour change (objtex.py): 1..4 = the object's change colour a..d tints a second pass of the part
    mask_bitmap: int | None = None       # its mask: the blue channel of this bitmap becomes the alpha of the base map in that pass
    texture2: int = 0xFFFF               # the second pass's texture: the base map with the mask as alpha (NO_TEXTURE: none)
    bump_bitmap_id: int | None = None    # an environment shader's bump map (a p8-bump height field), baked into the colour map for BUMP_BAKE bitmaps
    texture: int = NO_TEXTURE            # filled when the texture table is built
    tint: tuple = ()                     # objtex.TINT: rgb multiplier baked into the colour map
    reflect: tuple = ()                  # objtex.soso_reflect: (mask bitmap tag id, (r, g, b) added where the mask is 1): the environment reflection baked into the base map
    blank: bool = False                  # a transparent shader whose map is not a 2D image (a cube map reflection): drawn as nothing, not as flat grey
    product: tuple = ()                  # objtex.sotr_product: bitmap tag ids whose product is the texture (a multi-map transparent generic shader)


def _read(region: bytes, off: int, fmt: str):
    try:
        return struct.unpack_from(fmt, region, off)
    except struct.error:
        raise TexError("shader field outside the tag region") from None


def _bitmap_ref(m: MapData, block: int, off: int) -> int | None:
    """Tag id of the bitmap reference at block+off, None if null or not a bitmap."""
    raw = _read(m.region, block + off, "<4sIII")
    tid = raw[3]
    if raw[0] != b"mtib" or tid == 0xFFFFFFFF:
        return None
    e = m.by_id.get(tid)
    return tid if e is not None and e.group == "bitm" else None


def chicago_base_map(m: MapData, first: int, count: int) -> int:
    """Index of the map of a ``schi`` shader that stands in for the whole stack on PS2 (one texture per
    material). A map whose colour function multiplies it with the next map (2, 3) while its alpha function
    is "current" or "next map" (0, 1) is a pure colour modulator, a mask or detail tile over the maps after
    it (the space sky's 64 px ``star mask`` x3 over the 256 px ``stars`` x16, a30's ``detail ground`` over
    ``horizon land``), and is skipped; a modulator that also supplies the alpha (the a30 rivers) stays
    eligible because the shape matters more than the pattern. Of the remaining maps the one with the
    smallest tiling is the backdrop (a30's ``sky dusk`` gradient x1 against the ``stars`` x18 added over
    it); ties keep the earlier map. Falls back to map 0."""
    best, best_tile = 0, None
    for i in range(count):
        mo = first + i * SCHI_MAP[0]
        cf, af = _read(m.region, mo + SCHI_COLOR_FN, "<HH")
        if i + 1 < count and cf in (CF_MULTIPLY, CF_DOUBLE_MULTIPLY) and af in (AF_CURRENT, AF_NEXT_MAP):
            continue
        u, v = _read(m.region, mo + SCHI_MAP[1], "<f")[0], _read(m.region, mo + SCHI_MAP[2], "<f")[0]
        tile = abs(u * v) if (u == u and v == v and u != 0.0 and v != 0.0) else 1.0
        if best_tile is None or tile < best_tile:
            best, best_tile = i, tile
    return best


def resolve_shader(m: MapData, shader_id: int) -> ShaderInfo:
    e = m.by_id.get(shader_id)
    if e is None:
        return ShaderInfo(shader_id, None, f"#{shader_id:#x}", flags=BF_UNTEXTURED, note="unknown tag id")
    info = ShaderInfo(shader_id, e.group, e.path)
    block = m.tag_off(e)
    try:
        if e.group == "senv":
            flags, typ = _read(m.region, block + SENV_FLAGS, "<HH")
            info.alpha_test = bool(flags & 1)
            info.keep_alpha = info.alpha_test
            if typ:
                info.flags |= BF_BLENDED_BASE
            info.blend = BLEND_ALPHA_TEST if info.alpha_test else BLEND_OPAQUE
            info.bitmap_id = _bitmap_ref(m, block, SENV_BASE_MAP)
            info.bump_bitmap_id = _bitmap_ref(m, block, SENV_BUMP_MAP)
        elif e.group in ("sotr", "schi"):
            flags = _read(m.region, block + TRANSPARENT_FLAGS, "<B")[0]
            blend = _read(m.region, block + TRANSPARENT_BLEND, "<H")[0]
            info.alpha_test = bool(flags & 1)
            info.keep_alpha = True
            info.flags |= (BF_DECAL if flags & 2 else 0) | (BF_TWO_SIDED if flags & 4 else 0)
            info.blend = BLEND_ALPHA_TEST if info.alpha_test else BLEND_FROM_FRAMEBUFFER.get(blend, BLEND_ALPHA)
            size, us, vs, ref = SOTR_MAP if e.group == "sotr" else SCHI_MAP
            count, addr = _read(m.region, block + TRANSPARENT_MAPS, "<II")
            if count:
                mo = addr - m.base
                if e.group == "schi" and count > 1:
                    mo += chicago_base_map(m, mo, count) * size
                info.bitmap_id = _bitmap_ref(m, mo, ref)
                u, v = _read(m.region, mo + us, "<f")[0], _read(m.region, mo + vs, "<f")[0]
                if 0.0 < u <= 4096.0 and 0.0 < v <= 4096.0:
                    info.uv_scale = (u, v)
        elif e.group == "sgla":
            info.blend, info.keep_alpha = BLEND_ALPHA, True
            info.bitmap_id = _bitmap_ref(m, block, SGLA_MAP)
            s = _read(m.region, block + SGLA_SCALE, "<f")[0]
            if 0.0 < s <= 4096.0:
                info.uv_scale = (s, s)
        elif e.group == "swat":
            info.blend = BLEND_ALPHA            # translucent water; base alpha is a reflection mask
            info.bitmap_id = _bitmap_ref(m, block, SWAT_MAP)
        else:
            info.note = f"unsupported shader group {e.group!r}"
    except (TexError, bitmaps.BitmapError) as exc:
        info.bitmap_id, info.note = None, str(exc)
    if info.bitmap_id is None and not info.note:
        info.note = "no base map"
    if info.bitmap_id is None:
        info.flags |= BF_UNTEXTURED
    return info


# ---- pixel helpers ---------------------------------------------------------------------------
class Rgba(NamedTuple):
    width: int
    height: int
    pixels: bytes          # R, G, B, A row-major


def _to_px(rgba: bytes) -> array:
    px = array("I", rgba)
    if not _LE:
        px.byteswap()
    return px


def _box2_plane(p: bytes, w: int, h: int) -> bytes:
    out = bytearray((w // 2) * (h // 2))
    hw = w // 2
    for y in range(h // 2):
        a = p[2 * y * w:(2 * y + 1) * w]
        b = p[(2 * y + 1) * w:(2 * y + 2) * w]
        out[y * hw:(y + 1) * hw] = bytes([(a0 + a1 + b0 + b1 + 2) >> 2
                                          for a0, a1, b0, b1 in zip(a[0::2], a[1::2], b[0::2], b[1::2])])
    return bytes(out)


def box2(rgba: bytes, w: int, h: int, alpha_weighted: bool) -> bytes:
    """Halve both dimensions. With ``alpha_weighted`` colour is weighted by alpha so transparent
    texels do not darken or bleed into the opaque ones."""
    planes = [rgba[c::4] for c in range(4)]
    if not alpha_weighted:
        halves = [_box2_plane(p, w, h) for p in planes]
    else:
        ra, ga, ba, aa = planes
        hw, hh = w // 2, h // 2
        outs = [bytearray(hw * hh) for _ in range(4)]
        for y in range(hh):
            r0, r1 = y * 2 * w, (y * 2 + 1) * w
            for x in range(hw):
                i = r0 + 2 * x
                j = r1 + 2 * x
                idx = (i, i + 1, j, j + 1)
                asum = aa[i] + aa[i + 1] + aa[j] + aa[j + 1]
                o = y * hw + x
                outs[3][o] = (asum + 2) >> 2
                if asum:
                    for c, pl in enumerate((ra, ga, ba)):
                        outs[c][o] = min(255, (sum(pl[k] * aa[k] for k in idx) + asum // 2) // asum)
                else:
                    for c, pl in enumerate((ra, ga, ba)):
                        outs[c][o] = (sum(pl[k] for k in idx) + 2) >> 2
        halves = [bytes(o) for o in outs]
    out = bytearray(len(halves[0]) * 4)
    for c in range(4):
        out[c::4] = halves[c]
    return bytes(out)


def _halve_x(p: bytes) -> bytes:
    return bytes([(a + b + 1) >> 1 for a, b in zip(p[0::2], p[1::2])])


def _halve_y(p: bytes, w: int, h: int) -> bytes:
    rows = []
    for y in range(h // 2):
        a, b = p[2 * y * w:(2 * y + 1) * w], p[(2 * y + 1) * w:(2 * y + 2) * w]
        rows.append(bytes([(x + z + 1) >> 1 for x, z in zip(a, b)]))
    return b"".join(rows)


def _double_x(p: bytes) -> bytes:
    out = bytearray(len(p) * 2)
    out[0::2] = p
    out[1::2] = p
    return bytes(out)


def _double_y(p: bytes, w: int, h: int) -> bytes:
    return b"".join(p[y * w:(y + 1) * w] * 2 for y in range(h))


def resample_plane(p: bytes, w: int, h: int, tw: int, th: int) -> bytes:
    """Power-of-two resize of one 8-bit plane: box filter down, replicate up."""
    while w > tw and h > th:
        p, w, h = _box2_plane(p, w, h), w // 2, h // 2
    while w > tw:
        p, w = _halve_x(p), w // 2
    while h > th:
        p, h = _halve_y(p, w, h), h // 2
    while w < tw:
        p, w = _double_x(p), w * 2
    while h < th:
        p, h = _double_y(p, w, h), h * 2
    return p


def resample_rgba(rgba: bytes, w: int, h: int, tw: int, th: int, alpha_weighted: bool) -> bytes:
    while w > tw and h > th:
        rgba, w, h = box2(rgba, w, h, alpha_weighted), w // 2, h // 2
    if (w, h) == (tw, th):
        return rgba
    planes = [resample_plane(rgba[c::4], w, h, tw, th) for c in range(4)]
    out = bytearray(tw * th * 4)
    for c in range(4):
        out[c::4] = planes[c]
    return bytes(out)


def target_size(w: int, h: int, cap: int = MAX_TEX) -> tuple[int, int]:
    """Half resolution, power of two, 8..cap (256) per side."""
    tw, th = max(MIN_TEX, w // 2), max(MIN_TEX, h // 2)
    while tw > cap or th > cap:
        tw, th = max(MIN_TEX, tw // 2), max(MIN_TEX, th // 2)
    return tw, th


def alpha_class(rgba: bytes) -> int:
    alphas = set(rgba[3::4])
    if alphas <= {255}:
        return CLASS_OPAQUE
    if alphas <= {0, 255}:
        return CLASS_PUNCH
    return CLASS_BLEND


# ---- quantiser ---------------------------------------------------------------------------------
# Colour reduction before the histogram: 5/6/5 bits of RGB (what DXT and R5G6B5 sources carry),
# 4 bits of alpha when alpha is quantised too.
MASK_RGB, MASK_RGBA = 0x00F8FCF8, 0xF0F8FCF8
MASK_RGB8 = 0x00FFFFFF          # full precision: dark gradients (the space sky) band visibly when reduced to 5-6-5 first


def _expand(key: int, full: bool = False) -> tuple[int, int, int, int]:
    """Reduced key -> 8-bit channels by bit replication (0xF8 -> 0xFF, so white stays white); a full precision key is the colour itself."""
    if full:
        return key & 0xFF, key >> 8 & 0xFF, key >> 16 & 0xFF, 255
    r, g, b, a = key & 0xF8, key >> 8 & 0xFC, key >> 16 & 0xF8, key >> 24 & 0xF0
    return r | r >> 5, g | g >> 6, b | b >> 5, a | a >> 4


class Quantiser:
    """Deterministic median cut over a histogram of reduced colours.

    Palette entries are population-weighted box means. ``indices`` also classifies colours that
    were not in the histogram (the lower mips) by walking the recorded cuts; ``rms`` is the error
    over the histogram in 0..255 units per channel."""

    def __init__(self, hist: Counter, ncolours: int, mask: int):
        self.mask = mask
        self.use_alpha = mask == MASK_RGBA
        self.full = mask == MASK_RGB8
        nch = 4 if self.use_alpha else 3
        self._nch = nch
        boxes: dict[int, list] = {0: [(*_expand(k, self.full), n, k) for k, n in sorted(hist.items())]}
        scores: dict[int, tuple[float, int]] = {}

        def rate(bid: int) -> None:
            ent = boxes[bid]
            best = (0.0, 0)
            if len(ent) > 1:
                total = sum(e[4] for e in ent)
                for ch in range(nch):
                    vals = [e[ch] for e in ent]
                    score = (max(vals) - min(vals)) * total ** 0.5
                    if score > best[0]:
                        best = (score, ch)
            scores[bid] = best

        rate(0)
        self._cuts: dict[int, tuple[int, int, int, int]] = {}
        next_id = 1
        while len(boxes) < ncolours:
            bid = max(scores, key=lambda b: (scores[b][0], -b))
            score, ch = scores[bid]
            if score <= 0.0:
                break
            ent = boxes.pop(bid)
            del scores[bid]
            ent.sort(key=lambda e: (e[ch], e[5]))
            total = sum(e[4] for e in ent)
            acc, cut = 0, 1
            for i in range(len(ent) - 1):
                acc += ent[i][4]
                if ent[i + 1][ch] != ent[i][ch]:
                    cut = i + 1
                    if acc * 2 >= total:
                        break
            lo, hi = next_id, next_id + 1
            next_id += 2
            boxes[lo], boxes[hi] = ent[:cut], ent[cut:]
            self._cuts[bid] = (ch, ent[cut][ch], lo, hi)
            rate(lo)
            rate(hi)
        self.palette: list[tuple[int, int, int, int]] = []
        self._leaf: dict[int, int] = {}
        self._cache: dict[int, int] = {}
        sq = tot = 0
        for bid in sorted(boxes):
            ent = boxes[bid]
            total = sum(e[4] for e in ent) or 1
            col = [(sum(e[c] * e[4] for e in ent) + total // 2) // total for c in range(nch)]
            col = (col[0], col[1], col[2], col[3] if self.use_alpha else 255)
            self._leaf[bid] = len(self.palette)
            self.palette.append(col)
            for e in ent:
                self._cache[e[5]] = len(self.palette) - 1
                sq += e[4] * sum((e[c] - col[c]) ** 2 for c in range(nch))
                tot += e[4]
        self.rms = (sq / (tot * nch)) ** 0.5 if tot else 0.0

    def _classify(self, key: int) -> int:
        return self._classify_rgb(_expand(key, self.full))

    def _classify_rgb(self, c) -> int:
        node = 0
        while node in self._cuts:
            ch, thr, lo, hi = self._cuts[node]
            node = lo if c[ch] < thr else hi
        return self._leaf[node]

    def dithered(self, rgba: bytes, w: int, h: int) -> bytes:
        """Floyd-Steinberg error diffusion of an opaque image onto the palette: the cut tree picks each texel's entry, the error against the
        entry's colour goes to the neighbours (serpentine scan). Smooth gradients keep their tone instead of stepping between two entries."""
        pal = self.palette
        err = [[0.0] * (3 * w + 6), [0.0] * (3 * w + 6)]
        out = bytearray(w * h)
        for y in range(h):
            cur, nxt = err[y & 1], err[(y + 1) & 1]
            for i in range(len(nxt)):
                nxt[i] = 0.0
            xs = range(w) if y % 2 == 0 else range(w - 1, -1, -1)
            d = 1 if y % 2 == 0 else -1
            for x in xs:
                o = 4 * (y * w + x)
                e = 3 * x + 3
                c = [min(255.0, max(0.0, rgba[o + k] + cur[e + k])) for k in range(3)]
                idx = self._classify_rgb((int(c[0] + 0.5), int(c[1] + 0.5), int(c[2] + 0.5), 255))
                out[y * w + x] = idx
                p = pal[idx]
                for k in range(3):
                    q = c[k] - p[k]
                    cur[e + 3 * d + k] += q * (7.0 / 16.0)
                    nxt[e - 3 * d + k] += q * (3.0 / 16.0)
                    nxt[e + k] += q * (5.0 / 16.0)
                    nxt[e + 3 * d + k] += q * (1.0 / 16.0)
        return bytes(out)

    def indices(self, keys: list[int]) -> bytes:
        cache = self._cache
        for k in set(keys).difference(cache):
            cache[k] = self._classify(k)
        return bytes(map(cache.__getitem__, keys))


# ---- texture conversion ----------------------------------------------------------------------
@dataclass
class Ps2Texture:
    source_id: int
    width: int
    height: int
    psm: int
    flags: int
    clut: bytes                  # upload blob: 16x16 (PSMT8, CSM1 order) or 8x2 (PSMT4) PSMCT32
    clut_entries: int
    mips: list[bytes]            # largest first, each padded to 16 bytes
    avg_rgba: int                # average colour, RGBA8, alpha on the 0x80 scale
    colours: int = 0
    rms: float = 0.0

    @property
    def pixel_bytes(self) -> int:
        return sum(len(m) for m in self.mips)

    @property
    def total_bytes(self) -> int:
        return self.pixel_bytes + len(self.clut)


def ps2_alpha(a: int) -> int:
    """8-bit alpha -> GS alpha (0x80 = 1.0)."""
    return (a * 128 + 127) // 255


def clut_swap(i: int) -> int:
    """CSM1 8-bit CLUT order: entries 8..15 and 16..23 of every block of 32 trade places
    (an involution: the same function maps logical index <-> stored position)."""
    return (i & ~0x18) | ((i & 8) << 1) | ((i & 0x10) >> 1)


def build_clut(palette: list[tuple[int, int, int, int]], entries: int) -> bytes:
    """RGBA palette (alpha 0..255) -> upload blob of ``entries`` PSMCT32 words, GS alpha scale.
    Unused entries repeat the last colour. 256 entries are stored in CSM1 order."""
    pal = list(palette)[:entries]
    while len(pal) < entries:
        pal.append(pal[-1] if pal else (0, 0, 0, 0))
    words = [r | g << 8 | b << 16 | ps2_alpha(a) << 24 for r, g, b, a in pal]
    if entries == 256:
        words = [words[clut_swap(p)] for p in range(256)]
    out = array("I", words)
    if not _LE:
        out.byteswap()
    return out.tobytes()


def parse_clut(blob: bytes, entries: int) -> list[tuple[int, int, int, int]]:
    """Inverse of build_clut: the logical palette, alpha still on the 0x80 scale."""
    words = array("I", blob)
    if not _LE:
        words.byteswap()
    logical = [words[clut_swap(i)] if entries == 256 else words[i] for i in range(entries)]
    return [(w & 255, w >> 8 & 255, w >> 16 & 255, w >> 24) for w in logical]


def pack_nibbles(idx: bytes) -> bytes:
    """PSMT4: two indices per byte, the even pixel in the low nibble."""
    return bytes([a | b << 4 for a, b in zip(idx[0::2], idx[1::2])])


def unpack_nibbles(packed: bytes) -> bytes:
    out = bytearray(len(packed) * 2)
    out[0::2] = bytes([b & 15 for b in packed])
    out[1::2] = bytes([b >> 4 for b in packed])
    return bytes(out)


def _pad16(b: bytes) -> bytes:
    return b + bytes(-len(b) % 16)


def mip_chain(rgba: bytes, w: int, h: int, alpha_weighted: bool) -> list[tuple[int, int, bytes]]:
    """Box filtered chain, largest first, while the smaller side is above 8 px and < 4 levels."""
    levels = [(w, h, rgba)]
    for lw, lh in mip_dims(w, h)[1:]:
        rgba = box2(rgba, w, h, alpha_weighted)
        w, h = lw, lh
        levels.append((w, h, rgba))
    return levels


def mip_dims(w: int, h: int) -> list[tuple[int, int]]:
    """Level sizes of a chain starting at (w, h): halve while the smaller side is above 8, <= 4."""
    out = [(w, h)]
    while len(out) < MAX_MIPS and min(w, h) > MIN_TEX:
        w, h = w // 2, h // 2
        out.append((w, h))
    return out


def _opaque(rgba: bytes) -> bytes:
    out = bytearray(rgba)
    out[3::4] = b"\xff" * (len(rgba) // 4)
    return bytes(out)


def _average_rgba(rgba: bytes) -> int:
    """Alpha-weighted mean colour and mean alpha, packed RGBA8 with alpha on the 0x80 scale."""
    n = len(rgba) // 4
    al = rgba[3::4]
    a = sum(al)
    if a:
        rgb = [sum(x * y for x, y in zip(rgba[c::4], al)) // a for c in range(3)]
    else:
        rgb = [sum(rgba[c::4]) // n for c in range(3)]
    return rgb[0] | rgb[1] << 8 | rgb[2] << 16 | ps2_alpha(a // n) << 24


def alpha_threshold(alphas: bytes) -> int:
    """Punch-through reference for a texture whose alpha is not binary: Otsu's split of the alpha
    histogram clamped to 16..128. Retail alpha-tested maps put their solid texels as low as ~80
    (and the holes at 0), so a fixed 128 would erase them."""
    hist = Counter(alphas)
    total = len(alphas)
    sum_all = sum(v * n for v, n in hist.items())
    best, best_t, w0, s0 = -1.0, 128, 0, 0
    for t in range(256):
        n = hist.get(t, 0)
        w0 += n
        s0 += t * n
        if w0 == 0 or w0 == total:
            continue
        m0, m1 = s0 / w0, (sum_all - s0) / (total - w0)
        between = w0 * (total - w0) * (m0 - m1) ** 2
        if between > best:
            best, best_t = between, t + 1
    return max(16, min(128, best_t))


def _level_indices(q: Quantiser, rgba: bytes, klass: int, alpha_ref: int, dim: tuple | None = None) -> bytes:
    if dim is not None:
        return q.dithered(rgba, dim[0], dim[1])
    px = _to_px(rgba)
    idx = q.indices([p & q.mask for p in px])
    if klass == CLASS_PUNCH:             # index 0 is the transparent entry
        return bytes([0 if p >> 24 < alpha_ref else i + 1 for p, i in zip(px, idx)])
    return idx


def convert_rgba(source_id: int, rgba: bytes, w: int, h: int, klass: int, force_bits: int = 0,
                 alpha_ref: int = 128, smooth: bool = False) -> Ps2Texture:
    """RGBA8 image already at its target size -> PS2 texture. ``klass`` is CLASS_*;
    ``force_bits`` 0 picks PSMT8 or PSMT4 by error, 4 / 8 force it; ``alpha_ref`` is the
    punch-through threshold (texels with alpha below it become the transparent entry). ``smooth`` (opaque images only): the palette is built at
    full colour precision and the indices are error diffused (sky gradients)."""
    if klass == CLASS_OPAQUE:
        rgba = _opaque(rgba)
    levels = mip_chain(rgba, w, h, klass != CLASS_OPAQUE)
    smooth = smooth and klass == CLASS_OPAQUE
    mask = MASK_RGBA if klass == CLASS_BLEND else MASK_RGB8 if smooth else MASK_RGB
    px = _to_px(levels[0][2])
    hist = Counter(p & mask for p in px if klass != CLASS_PUNCH or p >> 24 >= alpha_ref)
    if not hist:                                         # fully transparent punch-through image
        hist[0] = 1
    reserve = 1 if klass == CLASS_PUNCH else 0
    q8 = Quantiser(hist, 256 - reserve, mask) if force_bits != 4 else None
    q4 = Quantiser(hist, 16 - reserve, mask) if force_bits != 8 else None
    if force_bits == 4:
        use4 = True
    elif force_bits == 8 or q4 is None:
        use4 = False
    else:
        use4 = q4.rms <= PSMT4_MAX_RMS
    q = q4 if use4 else q8
    pal = list(q.palette)
    if klass == CLASS_PUNCH:
        n = len(pal)
        avg = tuple(sum(c[i] for c in pal) // n for i in range(3))
        pal.insert(0, (*avg, 0))
    mips = []
    for lw, lh, lrgba in levels:
        idx = _level_indices(q, lrgba, klass, alpha_ref, (lw, lh) if smooth else None)
        mips.append(_pad16(pack_nibbles(idx) if use4 else idx))
    entries = 16 if use4 else 256
    flags = {CLASS_PUNCH: TF_PUNCH, CLASS_BLEND: TF_BLEND}.get(klass, 0)
    return Ps2Texture(source_id, w, h, PSMT4 if use4 else PSMT8, flags, build_clut(pal, entries), entries,
                      mips, _average_rgba(rgba), len(q.palette) + reserve, q.rms)


KEY_TOLERANCE = 24          # sum of |dR| + |dG| + |dB| around the key colour that becomes transparent
KEY_MIN_BORDER = 0.4        # the key must cover this fraction of the image border


def colour_key_opaque(rgba: bytes, w: int, h: int) -> bytes:
    """Alpha-tested shaders whose bitmap carries no alpha at all (retail a30 tree sprites: DXT1 with
    no transparent block, trees on a flat olive background) get the dominant border colour keyed
    out. The retail data must get its transparency from somewhere this converter does not read yet,
    so this is a documented stand-in; images that already have alpha are returned unchanged."""
    if alpha_class(rgba) != CLASS_OPAQUE:
        return rgba
    border: Counter = Counter()
    for x in range(w):
        for y in (0, h - 1):
            border[bytes(rgba[(y * w + x) * 4:(y * w + x) * 4 + 3])] += 1
    for y in range(h):
        for x in (0, w - 1):
            border[bytes(rgba[(y * w + x) * 4:(y * w + x) * 4 + 3])] += 1
    key, count = border.most_common(1)[0]
    if count < KEY_MIN_BORDER * (2 * (w + h)):
        return rgba
    kr, kg, kb = key
    out = bytearray(rgba)
    for i in range(0, len(out), 4):
        if abs(out[i] - kr) + abs(out[i + 1] - kg) + abs(out[i + 2] - kb) <= KEY_TOLERANCE:
            out[i + 3] = 0
    return bytes(out)


def bump_img(m: MapData, s: "ShaderInfo") -> bitmaps.BitmapImage | None:
    """The bump map of an environment shader whose base bitmap is in BUMP_BAKE, when it decodes."""
    if s.group != "senv" or s.bump_bitmap_id is None or s.bitmap_id is None or not m.by_id[s.bitmap_id].path.startswith(BUMP_BAKE):
        return None
    try:
        imgs = m.images(s.bump_bitmap_id)
    except bitmaps.BitmapError:
        return None
    return imgs[0] if imgs and imgs[0].decodable else None


def bake_relief(rgba: bytes, w: int, h: int, height: bytes, strength: float = BUMP_STRENGTH) -> bytes:
    """Colour map x relief shade of a height field (one byte per texel, same size): the shade is 1 + strength x (height up-left - height down-right) / 255, so a plate edge
    facing the light (up-left) brightens and the opposite edge darkens; clamped to 0.5 .. 1.7. Edges of the image clamp."""
    out = bytearray(rgba)
    for y in range(h):
        ya, yb = max(0, y - 1) * w, min(h - 1, y + 1) * w
        for x in range(w):
            xa, xb = max(0, x - 1), min(w - 1, x + 1)
            sh = 1.0 + strength * (height[ya + xa] - height[yb + xb]) / 255.0
            sh = (0.5 if sh < 0.5 else 1.7 if sh > 1.7 else sh) * BUMP_GAIN
            o = 4 * (y * w + x)
            for c in range(3):
                v = int(out[o + c] * sh + 0.5)
                out[o + c] = 255 if v > 255 else v
    return bytes(out)


def convert_texture(m: MapData, img: bitmaps.BitmapImage, source_id: int, klass_cap: int,
                    force_bits: int = 0, key_opaque: bool = False, bump: bitmaps.BitmapImage | None = None) -> Ps2Texture:
    """Decode, shrink and quantise one bitmap image. ``klass_cap`` is the strongest alpha class the
    shader allows: CLASS_OPAQUE drops alpha, CLASS_PUNCH thresholds it, CLASS_BLEND keeps it; the
    class actually used is the lesser of that and what the pixels contain."""
    tw, th = target_size(img.width, img.height, HERO_BITMAPS.get(m.by_id[source_id].path, MAX_TEX))
    level = 1 if img.mip_count >= 1 and img.width >> 1 >= tw and img.height >> 1 >= th and bump is None else 0
    w, h, rgba = bitmaps.decode_level(img, m.pixels(img), level)
    if key_opaque and klass_cap == CLASS_PUNCH:
        rgba = colour_key_opaque(rgba, w, h)
    if bump is not None:                                  # relief baked at the source resolution, then shrunk with the colour
        bw, bh, brgba = bitmaps.decode_level(bump, m.pixels(bump), 0)
        height = resample_plane(bytes(brgba[0::4]), bw, bh, w, h) if (bw, bh) != (w, h) else bytes(brgba[0::4])
        rgba = bake_relief(bytes(rgba), w, h, height)
    found = alpha_class(rgba)
    klass = min(klass_cap, found)
    alpha_ref = alpha_threshold(rgba[3::4]) if klass == CLASS_PUNCH and found == CLASS_BLEND else 128
    rgba = resample_rgba(rgba, w, h, tw, th, klass != CLASS_OPAQUE)
    tex = convert_rgba(source_id, rgba, tw, th, klass, 8 if bump is not None else force_bits, alpha_ref, smooth=bump is not None)
    if img.width < MIN_TEX or img.height < MIN_TEX:
        tex.flags |= TF_UPSCALED
    return tex


# ---- lightmaps -------------------------------------------------------------------------------
def luminance(rgba: bytes) -> bytes:
    """Rec.601 luma plane (0..255) of an RGBA image."""
    return bytes([(77 * r + 150 * g + 29 * b + 128) >> 8
                  for r, g, b in zip(rgba[0::4], rgba[1::4], rgba[2::4])])


@dataclass
class LightmapPage:
    index: int
    src_width: int
    src_height: int
    width: int
    height: int
    pixels: bytes                # PSMT8 row-major luminance indices, padded to 16 bytes
    avg_rgba: int                # mean page colour (RGBA8, alpha 0xFF) for flat fallback
    avg_luma: int


def lightmap_size(w: int, h: int, extra_shift: int = 0) -> tuple[int, int]:
    """Half resolution (lightmaps are low frequency), halved again ``extra_shift`` times, min 8."""
    s = 1 + extra_shift
    return max(MIN_TEX, w >> s), max(MIN_TEX, h >> s)


def lightmap_curve(luma: int, gain: float, gamma: float = LM_GAMMA) -> int:
    """Index of the grey ramp for a lightmap luminance. ``gamma`` 1 is the linear ``luma x gain``. Otherwise the brightest texel
    (LM_WHITE) maps to 127 x gain (1.0 x gain when 0x80 is 1.0) and darker texels follow x ** gamma: the retail a10 lightmaps are
    bimodal (a quarter of the texels at 170..249, the rest mostly below 80), so the linear gain 1.5 that matched the mean
    brightness of the Xbox intro doubled every lit floor and wall to white (2026-10-18 comparison, scripts/cmp-intro.sh) while
    the curve keeps the mean and lifts the shadows."""
    if gamma == 1.0:
        return min(255, int(luma * gain + 0.5))
    return min(255, int(127.0 * gain * (min(luma, LM_WHITE) / LM_WHITE) ** gamma + 0.5))


def lightmap_tone_rgb(r: float, g: float, b: float, gain: float, gamma: float = LM_GAMMA, chroma: float = None) -> tuple[float, float, float]:
    """A lightmap colour as the BSP passes show it: the luminance through ``lightmap_curve`` (index units, 128 = 1.0), the tint
    pulled towards neutral by ``chroma`` (default CHROMA_STRENGTH). The object lighting probes use it, so a crewman stands in the
    light of the wall behind him instead of a linear ``luma x 1.0`` that was 2.5 times darker than the walls (Keyes, bridge)."""
    chroma = CHROMA_STRENGTH if chroma is None else chroma
    if gamma == 1.0 and chroma == 1.0:
        return r * gain, g * gain, b * gain                 # the legacy linear probe
    y = 0.299 * r + 0.587 * g + 0.114 * b
    if y < 1.0:
        return 0.0, 0.0, 0.0
    ty = float(lightmap_curve(int(y + 0.5), gain, gamma))
    return tuple(ty * (1.0 + chroma * (c / y - 1.0)) for c in (r, g, b))  # type: ignore[return-value]


def convert_lightmap_page(index: int, page: Rgba, extra_shift: int = 0, gain: float = LM_DEFAULT_GAIN,
                          gamma: float = LM_GAMMA) -> LightmapPage:
    """RGBA page -> PSMT8 luminance page. Index = ``lightmap_curve(luma, gain, gamma)``; the shared CLUT is a grey
    ramp (see ``grey_ramp_clut``), so index 0x80 modulates by 1.0 when used as alpha."""
    w, h, rgba = page
    tw, th = lightmap_size(w, h, extra_shift)
    y = resample_plane(luminance(rgba), w, h, tw, th)
    if gain != 1.0 or gamma != 1.0:
        y = bytes([lightmap_curve(v, gain, gamma) for v in y])
    n = len(rgba) // 4
    avg = [sum(rgba[c::4]) // n for c in range(3)]
    return LightmapPage(index, w, h, tw, th, _pad16(y), avg[0] | avg[1] << 8 | avg[2] << 16 | 0xFF000000,
                        sum(y) // len(y))


def grey_ramp_clut() -> bytes:
    """Entry i = (i, i, i, alpha i) in CSM1 order. As alpha, 0x80 = 1.0 and 0xFF = ~2.0."""
    words = [i | i << 8 | i << 16 | i << 24 for i in range(256)]
    out = array("I", [words[clut_swap(p)] for p in range(256)])
    if not _LE:
        out.byteswap()
    return out.tobytes()


def vertex_chroma(page: Rgba | tuple[int, int, bytes], lm_uv: tuple[float, float],
                  strength: float = CHROMA_STRENGTH) -> tuple[int, int, int]:
    """Chroma-only tint for a vertex: the page colour at ``lm_uv`` (bilinear, clamped) divided by its
    luminance, scaled so 0x80 = 1.0, pulled towards neutral by ``strength`` and clamped to 0..255. A neutral grey page
    gives (128, 128, 128); pages (nearly) black at that point give neutral too. Luminance itself lives in the PSMT8 page.
    The a10 lightmaps are amber on average (BSP 1: mean 120 / 90 / 51); at full strength every wall came out yellow-green where
    the Xbox intro frames show neutral grey walls (2026-10-18 comparison, scripts/cmp-intro.sh); 0.4 keeps the red alarm
    lights and the cold console light without the global cast."""
    w, h, px = page
    u, v = lm_uv
    fx = min(max(u * w - 0.5, 0.0), w - 1.0)
    fy = min(max(v * h - 0.5, 0.0), h - 1.0)
    x0, y0 = int(fx), int(fy)
    x1, y1 = min(x0 + 1, w - 1), min(y0 + 1, h - 1)
    tx, ty = fx - x0, fy - y0
    rgb = []
    for c in range(3):
        top = px[(y0 * w + x0) * 4 + c] * (1 - tx) + px[(y0 * w + x1) * 4 + c] * tx
        bot = px[(y1 * w + x0) * 4 + c] * (1 - tx) + px[(y1 * w + x1) * 4 + c] * tx
        rgb.append(top * (1 - ty) + bot * ty)
    y = 0.299 * rgb[0] + 0.587 * rgb[1] + 0.114 * rgb[2]
    if y < 1.0:
        return 128, 128, 128
    return tuple(max(0, min(255, int((1.0 + strength * (c / y - 1.0)) * 128 + 0.5))) for c in rgb)  # type: ignore[return-value]


# ---- BSP scanning ----------------------------------------------------------------------------
BSP_LIGHTMAPS, BSP_MATERIALS_AT = 0x104, 0x14
MAT_SHADER, MAT_SURFACES, MAT_RENDERED, MAT_LIGHTMAP_VERTS = 0x0C, 0x18, 0xB4, 0xC8
NO_PAGE = 0xFFFF


@dataclass
class BspInfo:
    index: int
    path: str
    lightmap_tag: int | None = None
    pages: set[int] = field(default_factory=set)            # lightmap pages referenced by materials
    shaders: Counter = field(default_factory=Counter)        # shader id -> material count


def scan_bsp(index: int, loc: bsp.BspLocation, block: bytes) -> BspInfo:
    """Shader ids, lightmap bitmap and referenced pages of one BSP block (same material filter as
    bspconv: materials without surfaces or rendered vertices are skipped)."""
    info = BspInfo(index, loc.path)
    root = struct.unpack_from("<I", block, 0)[0] - loc.address
    if not 0 <= root <= len(block) - 0x27C:
        raise TexError(f"{loc.path}: structure_bsp root outside the block")
    tid = struct.unpack_from("<I", block, root + 12)[0]
    info.lightmap_tag = None if tid == 0xFFFFFFFF else tid
    count, addr = struct.unpack_from("<II", block, root + BSP_LIGHTMAPS)
    o = addr - loc.address
    if count and (o < 0 or o + count * bsp.LIGHTMAP_SIZE > len(block)):
        raise TexError(f"{loc.path}: lightmap records outside the block")
    for li in range(count):
        rec = o + li * bsp.LIGHTMAP_SIZE
        page = struct.unpack_from("<H", block, rec)[0]
        mcount, maddr = struct.unpack_from("<II", block, rec + BSP_MATERIALS_AT)
        mo = maddr - loc.address
        if mcount and (mo < 0 or mo + mcount * bsp.MATERIAL_SIZE > len(block)):
            raise TexError(f"{loc.path}: lightmap materials outside the block")
        for k in range(mcount):
            mat = mo + k * bsp.MATERIAL_SIZE
            n = struct.unpack_from("<i", block, mat + MAT_SURFACES)[0]
            rv = struct.unpack_from("<I", block, mat + MAT_RENDERED)[0]
            lv = struct.unpack_from("<I", block, mat + MAT_LIGHTMAP_VERTS)[0]
            if n == 0 or rv == 0:
                continue
            info.shaders[struct.unpack_from("<I", block, mat + MAT_SHADER)[0]] += 1
            if lv and page != NO_PAGE:
                info.pages.add(page)
    return info


def scan_map(m: MapData) -> list[BspInfo]:
    out = []
    with m.path.open("rb") as f:
        for i, loc in enumerate(bsp.locate(m.index, m.region, m.header.decompressed_size)):
            f.seek(loc.file_offset)
            out.append(scan_bsp(i, loc, f.read(loc.size)))
    return out


def lightmap_pages_rgba(m: MapData, info: BspInfo) -> dict[int, Rgba]:
    """Full resolution RGBA of every referenced lightmap page (input for ``vertex_chroma``)."""
    out: dict[int, Rgba] = {}
    if info.lightmap_tag is None:
        return out
    images = m.images(info.lightmap_tag)
    for page in sorted(info.pages):
        if page >= len(images) or not images[page].decodable:
            continue
        img = images[page]
        w, h, rgba = bitmaps.decode_level(img, m.pixels(img), 0)
        out[page] = Rgba(w, h, rgba)
    return out


# ---- section writers / parsers ---------------------------------------------------------------
TEXS_HEADER = "<4sHHIIIIIII28x"
TEXTURE_FMT = "<IHHBBHIIIHHI"
BINDING_FMT = "<IHHBBHffIH6x"
LMAP_HEADER = "<4sHHIIIIIfI28x"
PAGE_FMT = "<HHBBHIIHHII4x"
for _f, _n in ((TEXS_HEADER, HEADER_SIZE), (LMAP_HEADER, HEADER_SIZE), (TEXTURE_FMT, TEXTURE_REC),
               (BINDING_FMT, BINDING_REC), (PAGE_FMT, PAGE_REC)):
    assert struct.calcsize(_f) == _n, (_f, struct.calcsize(_f))


def build_texs(textures: list[Ps2Texture], shaders: list[ShaderInfo]) -> bytes:
    """TEXS section. ``shaders[i].texture`` indexes ``textures`` (or NO_TEXTURE)."""
    binds = sorted(shaders, key=lambda s: s.shader_id)
    tex_off = HEADER_SIZE
    bind_off = tex_off + TEXTURE_REC * len(textures)
    data_off = bind_off + BINDING_REC * len(binds)
    data = bytearray()
    trecs = bytearray()
    for t in textures:
        clut_off, pix_off = data_off + len(data), data_off + len(data) + len(t.clut)
        data += t.clut
        for mip in t.mips:
            data += mip
        trecs += struct.pack(TEXTURE_FMT, t.source_id, t.width, t.height, t.psm, len(t.mips), t.flags,
                             clut_off, pix_off, t.pixel_bytes, t.clut_entries, 0, t.avg_rgba)
    brecs = bytearray()
    for s in binds:
        flat = textures[s.texture].avg_rgba if s.texture != NO_TEXTURE else FLAT_GREY
        brecs += struct.pack(BINDING_FMT, s.shader_id, s.texture, s.flags, s.blend,
                             GROUP_CODES.get(s.group or "", 0), s.change_source, s.uv_scale[0], s.uv_scale[1], flat, s.texture2)
    untextured = sum(1 for s in binds if s.texture == NO_TEXTURE)
    total = data_off + len(data)
    head = struct.pack(TEXS_HEADER, TEXS_MAGIC, VERSION, 0, len(textures), len(binds), tex_off, bind_off,
                       data_off, total, untextured)
    return bytes(head + trecs + brecs + data)


@dataclass
class TexsView:
    textures: list[dict]
    bindings: list[dict]
    data: bytes

    def mip(self, tex: dict, level: int) -> tuple[int, int, bytes]:
        bits = 4 if tex["psm"] == PSMT4 else 8
        off = tex["pixel_offset"]
        dims = mip_dims(tex["width"], tex["height"])[:tex["mips"]]
        for lw, lh in dims[:level]:
            off += (lw * lh * bits // 8 + 15) // 16 * 16
        w, h = dims[level]
        return w, h, self.data[off:off + (w * h * bits // 8 + 15) // 16 * 16]


def parse_texs(data: bytes) -> TexsView:
    """Validating reader (the Python twin of engine/src/world/tex.c)."""
    if len(data) < HEADER_SIZE or data[:4] != TEXS_MAGIC:
        raise TexError("not a TEXS section")
    _m, ver, _r, nt, nb, to, bo, do, total, _u = struct.unpack_from(TEXS_HEADER, data)
    if ver != VERSION or total != len(data) or to % 16 or bo % 16 or do % 16 or do > len(data) \
            or to + nt * TEXTURE_REC > len(data) or bo + nb * BINDING_REC > len(data):
        raise TexError("inconsistent TEXS header")
    textures = []
    for i in range(nt):
        sid, w, h, psm, mips, flags, co, po, pb, ce, _p, avg = struct.unpack_from(
            TEXTURE_FMT, data, to + i * TEXTURE_REC)
        bits = {PSMT8: 8, PSMT4: 4}.get(psm)
        if bits is None or not (1 <= mips <= MAX_MIPS) or w & (w - 1) or h & (h - 1) \
                or not MIN_TEX <= w <= MAX_TEX_FORMAT or not MIN_TEX <= h <= MAX_TEX_FORMAT:
            raise TexError(f"texture {i}: bad format or size")
        if ce != (256 if psm == PSMT8 else 16) or co % 16 or po % 16 or co < do or co + ce * 4 > po \
                or po + pb > len(data):
            raise TexError(f"texture {i}: bad clut or pixel range")
        dims = mip_dims(w, h)
        if mips > len(dims):
            raise TexError(f"texture {i}: {mips} mips exceed the chain rule")
        want = sum((lw * lh * bits // 8 + 15) // 16 * 16 for lw, lh in dims[:mips])
        if want != pb:
            raise TexError(f"texture {i}: pixel bytes {pb} != {want}")
        textures.append({"source_id": sid, "width": w, "height": h, "psm": psm, "mips": mips, "flags": flags,
                         "clut_offset": co, "pixel_offset": po, "pixel_bytes": pb, "clut_entries": ce,
                         "avg_rgba": avg})
    bindings, last = [], -1
    for i in range(nb):
        sid, tex, flags, blend, group, change, su, sv, flat, tex2 = struct.unpack_from(
            BINDING_FMT, data, bo + i * BINDING_REC)
        if sid <= last or (tex != NO_TEXTURE and tex >= nt):
            raise TexError(f"binding {i}: unsorted or bad texture index")
        last = sid
        bindings.append({"shader_id": sid, "texture": tex, "flags": flags, "blend": blend, "group": group,
                         "uv_scale": (su, sv), "flat_rgba": flat, "change": change, "texture2": tex2})
    return TexsView(textures, bindings, data)


def build_lmap(pages: list[LightmapPage], gain: float = LM_DEFAULT_GAIN) -> bytes:
    """LMAP section: dense page table indexed by the BSP's lightmap page number (width 0 = unused)."""
    count = max((p.index for p in pages), default=-1) + 1
    by_index = {p.index: p for p in pages}
    table_off = HEADER_SIZE
    clut_off = table_off + PAGE_REC * count
    data_off = clut_off + 1024
    data = bytearray()
    recs = bytearray()
    for i in range(count):
        p = by_index.get(i)
        if p is None:
            recs += struct.pack(PAGE_FMT, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0)
            continue
        recs += struct.pack(PAGE_FMT, p.width, p.height, PSMT8, 0, 0, data_off + len(data), len(p.pixels),
                            p.src_width, p.src_height, p.avg_rgba, p.avg_luma)
        data += p.pixels
    total = data_off + len(data)
    head = struct.pack(LMAP_HEADER, LMAP_MAGIC, VERSION, 0, count, table_off, clut_off, data_off, total,
                       gain, len(pages))
    return bytes(head + recs + grey_ramp_clut() + data)


def parse_lmap(data: bytes) -> dict:
    if len(data) < HEADER_SIZE or data[:4] != LMAP_MAGIC:
        raise TexError("not an LMAP section")
    _m, ver, _r, count, to, co, do, total, gain, used = struct.unpack_from(LMAP_HEADER, data)
    if ver != VERSION or total != len(data) or to != HEADER_SIZE or co != to + PAGE_REC * count \
            or do != co + 1024 or do > len(data):
        raise TexError("inconsistent LMAP header")
    pages = []
    for i in range(count):
        w, h, psm, _p, flags, po, pb, sw, sh, avg, luma = struct.unpack_from(PAGE_FMT, data, to + i * PAGE_REC)
        if w == 0:
            pages.append(None)
            continue
        if psm != PSMT8 or w & (w - 1) or h & (h - 1) or po % 16 or po < do or po + pb > len(data) \
                or pb != (w * h + 15) // 16 * 16:
            raise TexError(f"lightmap page {i}: bad format or range")
        pages.append({"width": w, "height": h, "pixel_offset": po, "pixel_bytes": pb, "src_width": sw,
                      "src_height": sh, "avg_rgba": avg, "avg_luma": luma})
    return {"pages": pages, "gain": gain, "used": used, "clut": data[co:co + 1024], "data": data}


# ---- whole map -------------------------------------------------------------------------------
@dataclass
class ConvertResult:
    pack: bytes
    textures: list[Ps2Texture]
    shaders: list[ShaderInfo]
    bsps: list[BspInfo]
    lightmaps: dict[int, list[LightmapPage]]
    lm_shift: dict[int, int]
    seconds: float = 0.0
    untextured_materials: int = 0
    untextured_reasons: Counter = field(default_factory=Counter)


def klass_cap_for(info: ShaderInfo) -> int:
    """Strongest alpha class the shader's base map may use."""
    if info.alpha_test:
        return CLASS_PUNCH
    return CLASS_BLEND if info.keep_alpha else CLASS_OPAQUE


# ---- sotr shaders that multiply their maps ----------------------------------------------------
STAGE_SIZE, STAGE_INPUT_A, STAGE_INPUT_B = 112, 0x3C, 0x40
STAGE_MAP_INPUTS = (5, 6, 7, 8, 11, 12)           # map colour 0..3, scratch colour 0 / 1 (shader_transparent_generic_stage input enum)
PRODUCT_KEY = 1 << 40


def _u16(m: MapData, off: int) -> int:
    return struct.unpack_from("<H", m.region, off)[0]


def sotr_product(m: MapData, shader_id: int) -> tuple[int, ...]:
    """Bitmaps of an additive `sotr` shader whose stages only multiply its maps with each other (the Autumn's engine exhaust: a radial glow, a
    second glow and a blue band; the command displays' indicator lines): the Xbox shows their product. () for any other shader."""
    e = m.by_id.get(shader_id)
    if e is None or e.group != "sotr":
        return ()
    block = m.tag_off(e)
    if _u16(m, block + TRANSPARENT_BLEND) != 3:
        return ()
    mcount, maddr = struct.unpack_from("<II", m.region, block + TRANSPARENT_MAPS)
    scount, saddr = struct.unpack_from("<II", m.region, block + 0x60)
    size, _us, _vs, ref = SOTR_MAP
    if not 2 <= mcount <= 4 or scount != mcount - 1 or not 0 <= maddr - m.base < len(m.region) - mcount * size:
        return ()
    if not 0 <= saddr - m.base < len(m.region) - scount * STAGE_SIZE:
        return ()
    for i in range(scount):
        so = saddr - m.base + i * STAGE_SIZE
        if _u16(m, so + STAGE_INPUT_A) not in STAGE_MAP_INPUTS or _u16(m, so + STAGE_INPUT_B) not in STAGE_MAP_INPUTS:
            return ()
    ids = [_bitmap_ref(m, maddr - m.base, i * size + ref) for i in range(mcount)]
    return tuple(ids) if all(i is not None for i in ids) else ()


def blank_texture(source_id: int, additive: bool) -> Ps2Texture:
    """8 x 8 texture that draws nothing: black for an additive shader, fully transparent for an alpha blended one."""
    px = bytes([0, 0, 0, 255 if additive else 0] * 64)
    return convert_rgba(source_id, px, 8, 8, CLASS_OPAQUE if additive else CLASS_BLEND, 4, 128)


def multiply_normalise(layers: list[bytes]) -> bytes:
    """Channel-wise product of equally sized RGBA layers, scaled so the brightest colour channel reaches 255; alpha 255."""
    acc = list(layers[0])
    for rgba in layers[1:]:
        acc = [a * b // 255 if i & 3 != 3 else 255 for i, (a, b) in enumerate(zip(acc, rgba))]
    peak = max((v for i, v in enumerate(acc) if i & 3 != 3), default=0) or 1
    return bytes(min(255, v * 255 // peak) if i & 3 != 3 else 255 for i, v in enumerate(acc))


def product_texture(m: MapData, imgs: list, source_id: int, tw: int, th: int, force_bits: int = 8) -> Ps2Texture:
    """The product of several maps at tw x th, scaled so its brightest channel reaches 255 (an emissive glow: the Xbox's stage arithmetic is not
    as dark as the product in 8 bits). Opaque: the shader is additive."""
    layers = []
    for img in imgs:
        w, h, rgba = bitmaps.decode_level(img, m.pixels(img), 0)
        layers.append(resample_rgba(rgba, w, h, tw, th, False))
    return convert_rgba(source_id, multiply_normalise(layers), tw, th, CLASS_OPAQUE, force_bits, 128)


def convert_map(path: Path, lightmaps: bool = True, lm_gain: float = LM_DEFAULT_GAIN,
                bsp_filter: list[int] | None = None) -> ConvertResult:
    t0 = time.time()
    m = MapData.open(path)
    infos = [b for b in scan_map(m) if bsp_filter is None or b.index in bsp_filter]
    shader_ids = sorted({sid for b in infos for sid in b.shaders})
    shaders = [resolve_shader(m, sid) for sid in shader_ids]
    textures: list[Ps2Texture] = []
    seen: dict[tuple[int, int], int] = {}
    reasons: Counter = Counter()
    for s in shaders:
        if s.bitmap_id is None:
            reasons[s.note or "no base map"] += 1
            continue
        images = m.images(s.bitmap_id)
        if not images or not images[0].decodable:
            if s.group in ("sotr", "schi") and s.blend in (BLEND_ADD, BLEND_ALPHA):
                add = s.blend == BLEND_ADD
                key = (PRODUCT_KEY * 2 + int(add), 0)
                if key not in seen:
                    seen[key] = len(textures)
                    textures.append(blank_texture(s.bitmap_id, add))
                s.texture, s.blank = seen[key], True
                reasons["cube map reflection layer drawn as nothing"] += 1
                continue
            s.bitmap_id, s.flags, s.note = None, s.flags | BF_UNTEXTURED, "base map is not a decodable 2D image"
            reasons[s.note] += 1
            continue
        cap = klass_cap_for(s)
        prod = sotr_product(m, s.shader_id)
        bump = bump_img(m, s) if not prod else None
        key = (s.bitmap_id, cap) if not prod else (PRODUCT_KEY | prod[-1], cap)
        if bump is not None:
            key = (s.bitmap_id | BUMP_KEY, cap)
        if key not in seen:
            try:
                if prod:
                    pimgs = [m.images(b)[0] for b in prod]
                    tw, th = target_size(max(i.width for i in pimgs), max(i.height for i in pimgs))
                    tex = product_texture(m, pimgs, s.bitmap_id, max(tw, 32), max(th, 32))
                else:
                    tex = convert_texture(m, images[0], s.bitmap_id, cap, key_opaque=s.alpha_test, bump=bump)
            except bitmaps.BitmapError as exc:
                s.bitmap_id, s.flags, s.note = None, s.flags | BF_UNTEXTURED, f"base map failed to decode: {exc}"
                reasons["base map failed to decode"] += 1
                continue
            seen[key] = len(textures)
            textures.append(tex)
        s.texture = seen[key]
        if textures[s.texture].flags & TF_PUNCH and s.alpha_test:
            s.flags |= BF_ALPHA_TEST
    untextured_mats = 0
    for b in infos:
        untextured_mats += sum(c for sid, c in b.shaders.items()
                               if next(s for s in shaders if s.shader_id == sid).texture == NO_TEXTURE)
    sections: list[tuple[str, bytes]] = [("TEXS", build_texs(textures, shaders))]
    lm_pages: dict[int, list[LightmapPage]] = {}
    lm_shift: dict[int, int] = {}
    if lightmaps:
        for b in infos:
            if b.lightmap_tag is None or not b.pages:
                continue
            rgba = lightmap_pages_rgba(m, b)
            shift = 0
            while True:
                pages = [convert_lightmap_page(i, p, shift, lm_gain) for i, p in sorted(rgba.items())]
                if sum(len(p.pixels) for p in pages) <= LM_BUDGET_BYTES or shift >= 3:
                    break
                shift += 1
            lm_pages[b.index], lm_shift[b.index] = pages, shift
            sections.append((f"LM{b.index}", build_lmap(pages, lm_gain)))
    pack = hp2.build(path.stem, sections)
    return ConvertResult(pack, textures, shaders, infos, lm_pages, lm_shift, time.time() - t0,
                         untextured_mats, reasons)


def report(name: str, r: ConvertResult) -> str:
    pix = sum(t.pixel_bytes for t in r.textures)
    clut = sum(len(t.clut) for t in r.textures)
    psm4 = sum(1 for t in r.textures if t.psm == PSMT4)
    klass = Counter("punch" if t.flags & TF_PUNCH else "blend" if t.flags & TF_BLEND else "opaque"
                    for t in r.textures)
    groups = Counter(s.group for s in r.shaders)
    lines = [f"{name}: {len(r.shaders)} shaders ({', '.join(f'{k} {v}' for k, v in sorted(groups.items(), key=str))}), "
             f"{len(r.textures)} textures ({psm4} PSMT4; {dict(klass)}), "
             f"pixels {pix / 1024:.0f} KiB + CLUTs {clut / 1024:.0f} KiB = {(pix + clut) / 2**20:.2f} MiB "
             f"of the 6.0 MiB pool",
             f"  untextured: {sum(1 for s in r.shaders if s.texture == NO_TEXTURE)} shaders, "
             f"{r.untextured_materials} of {sum(sum(b.shaders.values()) for b in r.bsps)} BSP materials "
             f"{dict(r.untextured_reasons) if r.untextured_reasons else ''}"]
    lm_total = 0
    biggest = 0
    for i, pages in sorted(r.lightmaps.items()):
        size = sum(len(p.pixels) for p in pages)
        lm_total += size
        biggest = max(biggest, size)
    if r.lightmaps:
        lines.append(f"  lightmaps: {len(r.lightmaps)} BSPs, {lm_total / 2**20:.2f} MiB total, largest BSP "
                     f"{biggest / 1024:.0f} KiB (budget {LM_BUDGET_BYTES // 1024} KiB), extra shrink "
                     f"{dict((k, v) for k, v in r.lm_shift.items() if v) or 'none'}")
    lines.append(f"  pack {len(r.pack) / 2**20:.2f} MiB, converted in {r.seconds:.1f} s")
    return "\n".join(lines)


def info(path: Path) -> None:
    pk = hp2.parse(path.read_bytes())
    for s in pk.sections:
        raw = pk.read(s.tag)
        if s.tag == "TEXS":
            v = parse_texs(raw)
            n4 = sum(1 for t in v.textures if t["psm"] == PSMT4)
            print(f"TEXS: {len(v.textures)} textures ({n4} PSMT4), {len(v.bindings)} bindings "
                  f"({sum(1 for b in v.bindings if b['texture'] == NO_TEXTURE)} untextured), {len(raw)} bytes")
        elif s.tag.startswith("LM"):
            lm = parse_lmap(raw)
            used = [p for p in lm["pages"] if p]
            print(f"{s.tag}: {len(used)} pages of {len(lm['pages'])}, "
                  f"{sum(p['pixel_bytes'] for p in used)} pixel bytes, gain {lm['gain']:g}")


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("input", type=Path, help="decompressed .map (or, with --info, a texture .hp2)")
    ap.add_argument("-o", "--out", type=Path)
    ap.add_argument("--info", action="store_true", help="input is a .hp2: summarise its TEXS/LMn sections")
    ap.add_argument("--no-lightmaps", action="store_true")
    ap.add_argument("--lm-gain", type=float, default=LM_DEFAULT_GAIN, help="lightmap luminance gain")
    ap.add_argument("--bsp", type=int, action="append", help="only this BSP index (repeatable)")
    args = ap.parse_args(argv)
    if args.info:
        info(args.input)
        return 0
    if not args.out:
        ap.error("-o is required")
    res = convert_map(args.input, not args.no_lightmaps, args.lm_gain, args.bsp)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_bytes(res.pack)
    print(report(args.input.stem, res))
    print(f"wrote {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
