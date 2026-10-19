#!/usr/bin/env python3
"""Object textures for the `OTEX` section (same layout as `TEXS`, docs/09-tex-section.md; docs/11-model-section.md).

Resolves the shader tags that model parts reference to a base map plus blend state, converts each
distinct (bitmap, alpha class) once with texconv's quantiser, and writes a `TEXS`-layout blob whose
binding table is keyed by the Xbox shader tag id. The blob keeps the `TEXS` signature on purpose:
`tex_open` reads it unchanged; the pack section tag (`OTEX`) tells it apart from the BSP's `TEXS`.

Shader groups used by model parts (a10 a30 a50 b30 b40 c10 c20 c40 d20 d40, distinct shaders per map):
    soso  shader_model                 127..147   (the bulk: bipeds, weapons, vehicles, scenery)
    sotr, schi  transparent generic / chicago  11..38   (glows, holograms, flames)
    senv  environment                   3..18     (devices, doors)
    sgla  glass                         3..8
    smet  meter (HUD ammo/shield bars)  2..5      -> untextured
    spla  plasma                        1         -> untextured
    swat  water                         1 (a30)
sotr / schi / senv / sgla / swat resolve through `texconv.resolve_shader` (layouts in texconv.py).

shader_model (`soso`) layout, engine version 5 (offsets in the shader tag's data block):
    +0x28 u16 flags: bit0 detail after reflection, bit1 two-sided, bit2 NOT alpha-tested,
          bit3 alpha-blended decal, bit4 true atmospheric fog, bit5 disable two-sided culling
    +0x9C f32 map u scale, +0xA0 f32 map v scale   (1.0 on every retail soso of all ten maps)
    +0xA4 tag reference: base map (bitmap); +0xBC multipurpose map; +0xDC detail map; +0x164
          reflection cube map. Only the base map is converted (ADR-004/005).
    verified: the base map reference at +0xA4 on 124 of 127 a10 shaders (the other 3 have none) and the
              flag values 0, 1, 2, 4, 8 (the HEK flag names above match: no value outside bits 0..3);
              the scale pair is plausible (all 1.0), not exercised.
    unverified: that a clear bit 2 means the base alpha is an alpha test in this engine. The converter
              treats it so only when the base map really carries alpha (4 + 5 + 1 of 127 a10 shaders:
              dxt1 with holes or dxt3), otherwise the shader stays opaque.
Everything else (scex, spla, smet, ...) is untextured (flat grey) and counted.

Texture budget: object textures share the 6 MiB texture pool with the BSP's (docs/02 §5.3); `build_otex`
picks the largest maximum dimension (256, 128, 64, 32) for which the *estimated* size (every texture as
PSMT8 with a full mip chain, which over-estimates PSMT4 ones) stays within `budget` bytes.
"""
from __future__ import annotations

import struct
import sys
from dataclasses import dataclass, field
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from halo2ps2 import bitmaps, texconv  # noqa: E402

SOSO_FLAGS, SOSO_SCALE, SOSO_BASE_MAP, SOSO_MIN_SIZE = 0x28, 0x9C, 0xA4, 0xB4
SOSO_TWO_SIDED, SOSO_NOT_ALPHA_TESTED, SOSO_DECAL, SOSO_NO_CULL = 2, 4, 8, 0x20
DEFAULT_BUDGET = 3 * 2**20        # object textures per map; BSP textures (<= 1.1 MiB) and lightmaps share the pool
MIN_MAX_DIM = 32


def _bitmap_ref(m: texconv.MapData, off: int) -> int | None:
    """Tag id of the bitmap reference at region offset `off`; None if null or not a bitmap."""
    if off < 0 or off + 16 > len(m.region):
        return None
    group, _p, _n, tid = struct.unpack_from("<4sIII", m.region, off)
    if group != b"mtib" or tid == 0xFFFFFFFF:
        return None
    e = m.by_id.get(tid)
    return tid if e is not None and e.group == "bitm" else None


# Extra map layers of a chicago shader as pseudo shaders: a part of a sky dome that adds the stars over a nebula is drawn twice, the second
# time with a pseudo shader id that resolves to the added map (additive). {pseudo id: (chicago shader id, map index)}.
LAYER_BASE = 0xFE000000
_LAYERS: dict[int, tuple[int, int]] = {}


def layer_shader_id(shader_id: int, index: int) -> int:
    for pid, key in _LAYERS.items():
        if key == (shader_id, index):
            return pid
    pid = LAYER_BASE + len(_LAYERS) + 1
    _LAYERS[pid] = (shader_id, index)
    return pid


def chicago_layers(m: texconv.MapData, shader_id: int) -> list[int]:
    """Indices of the maps of a `schi` shader that add to the base map `chicago_base_map` picks: not the base, not a pure modulator
    (colour function multiply / double multiply with no alpha of its own): the space sky's stars over its nebula."""
    e = m.by_id.get(shader_id)
    if e is None or e.group != "schi":
        return []
    block = m.tag_off(e)
    count, addr = struct.unpack_from("<II", m.region, block + texconv.TRANSPARENT_MAPS)
    if count < 2:
        return []
    first = addr - m.base
    base = texconv.chicago_base_map(m, first, count)
    out = []
    for i in range(count):
        if i == base:
            continue
        mo = first + i * texconv.SCHI_MAP[0]
        cf, af = struct.unpack_from("<HH", m.region, mo + texconv.SCHI_COLOR_FN)
        if i + 1 < count and cf in (texconv.CF_MULTIPLY, texconv.CF_DOUBLE_MULTIPLY) and af in (texconv.AF_CURRENT, texconv.AF_NEXT_MAP):
            continue
        if _bitmap_ref(m, mo + texconv.SCHI_MAP[3]) is not None:
            out.append(i)
    return out


def resolve_layer(m: texconv.MapData, pseudo_id: int) -> texconv.ShaderInfo:
    sid, index = _LAYERS[pseudo_id]
    e = m.by_id[sid]
    info = texconv.ShaderInfo(pseudo_id, "schi", f"{e.path}#map{index}")
    block = m.tag_off(e)
    count, addr = struct.unpack_from("<II", m.region, block + texconv.TRANSPARENT_MAPS)
    mo = addr - m.base + index * texconv.SCHI_MAP[0]
    info.bitmap_id = _bitmap_ref(m, mo + texconv.SCHI_MAP[3])
    u, v = struct.unpack_from("<2f", m.region, mo + texconv.SCHI_MAP[1])[0], struct.unpack_from("<f", m.region, mo + texconv.SCHI_MAP[2])[0]
    if 0.0 < u <= 4096.0 and 0.0 < v <= 4096.0:
        info.uv_scale = (u, v)
    info.blend = texconv.BLEND_ADD
    info.flags |= texconv.BF_TWO_SIDED
    if info.bitmap_id is None:
        info.note, info.flags = "no layer map", info.flags | texconv.BF_UNTEXTURED
    return info


# Colour change (chgconv.py): a shader_model whose colour change source is a..d (+0x4C) and that has a multipurpose map (+0xBC) gets a second
# texture, the base map with the multipurpose BLUE channel (the mask) as alpha, and `change_source` in its binding. The engine draws such a
# part a second time over the first pass, alpha blended, with the object's change colour as the light colour.
SOSO_CHANGE_SOURCE, SOSO_MULTIPURPOSE = 0x4C, 0xBC


def soso_change(m: texconv.MapData, shader_id: int) -> tuple[int, int] | None:
    """(change source 1..4, mask bitmap tag id) of a shader_model with a colour change and a multipurpose map, else None."""
    e = m.by_id.get(shader_id)
    if e is None or e.group != "soso":
        return None
    block = m.tag_off(e)
    if block < 0 or block + SOSO_MIN_SIZE > len(m.region):
        return None
    src = struct.unpack_from("<H", m.region, block + SOSO_CHANGE_SOURCE)[0]
    mask = _bitmap_ref(m, block + SOSO_MULTIPURPOSE)
    return (src, mask) if 1 <= src <= 4 and mask is not None else None


SOSO_CUBE_MAP, SOSO_REFL_PERP_BRIGHT, SOSO_REFL_PERP_COLOUR, SOSO_REFL_PAR_BRIGHT, SOSO_REFL_PAR_COLOUR = 0x164, 0x144, 0x148, 0x154, 0x158
REFLECT_KEY = 1 << 41
_CUBE_AVG: dict[int, tuple[float, float, float] | None] = {}


def cube_average(m: texconv.MapData, bitmap_id: int) -> tuple[float, float, float] | None:
    """Mean colour of a cube map's six faces (the first mip of each; DXT1 / DXT3 / DXT5 or 32 bit), None for anything else."""
    if bitmap_id in _CUBE_AVG:
        return _CUBE_AVG[bitmap_id]
    out = None
    try:
        img = m.images(bitmap_id)[0]
        raw = m.pixels(img)
        w, h, per_face = img.width, img.height, img.pixel_size // 6
        if img.type == 2 and per_face and w >= 4 and h >= 4:
            tot = [0.0, 0.0, 0.0]
            ok = True
            for f in range(6):
                chunk = raw[f * per_face:(f + 1) * per_face]
                if img.format == 14:
                    px = bitmaps.decode_dxt(14, chunk[:w * h // 2], w, h)
                elif img.format in (15, 16):
                    px = bitmaps.decode_dxt(img.format, chunk[:w * h], w, h)
                elif img.format in (10, 11):
                    px = bytes(c for i in range(w * h) for c in (chunk[i * 4 + 2], chunk[i * 4 + 1], chunk[i * 4], 255))
                else:
                    ok = False
                    break
                for c in range(3):
                    tot[c] += sum(px[c::4])
            if ok:
                n = 6.0 * w * h
                out = (tot[0] / n, tot[1] / n, tot[2] / n)
    except (bitmaps.BitmapError, IndexError, struct.error):
        out = None
    _CUBE_AVG[bitmap_id] = out
    return out


def soso_reflect(m: texconv.MapData, shader_id: int) -> tuple[int, tuple[int, int, int]] | None:
    """(multipurpose map id, rgb) of a shader_model with an environment reflection: where the multipurpose RED channel is 1 the reflection adds
    brightness x tint x the cube map's mean colour (the marines' armour reflects a sage coloured room: 0.6 x (143, 155, 137))."""
    e = m.by_id.get(shader_id)
    if e is None or e.group != "soso":
        return None
    block = m.tag_off(e)
    if block < 0 or block + SOSO_CUBE_MAP + 16 > len(m.region):
        return None
    mask, cube = _bitmap_ref(m, block + SOSO_MULTIPURPOSE), _bitmap_ref(m, block + SOSO_CUBE_MAP)
    if mask is None or cube is None:
        return None
    avg = cube_average(m, cube)
    if avg is None:
        return None
    rd = lambda off: struct.unpack_from("<f", m.region, block + off)[0]  # noqa: E731
    pb, kb = rd(SOSO_REFL_PERP_BRIGHT), rd(SOSO_REFL_PAR_BRIGHT)
    pc = [rd(SOSO_REFL_PERP_COLOUR + 4 * i) for i in range(3)]
    kc = [rd(SOSO_REFL_PAR_COLOUR + 4 * i) for i in range(3)]
    vals = pb + kb + sum(pc) + sum(kc)
    if not (vals == vals) or not 0.0 <= pb <= 4.0 or not 0.0 <= kb <= 4.0 or any(not 0.0 <= x <= 1.5 for x in pc + kc):
        return None
    bright = 0.5 * (pb + kb)
    add = tuple(int(min(255.0, bright * 0.5 * (pc[i] + kc[i]) * avg[i] + 0.5)) for i in range(3))
    return (mask, add) if max(add) >= 8 else None


def convert_cc_texture(m: texconv.MapData, base: bitmaps.BitmapImage, mask: bitmaps.BitmapImage, source_id: int, max_dim: int) -> texconv.Ps2Texture:
    """The base map (colour) with the mask's blue channel as alpha, converted like an object texture of alpha class blend."""
    tw, th = target_size(base.width, base.height, max_dim)
    level = 1 if base.mip_count >= 1 and base.width >> 1 >= tw and base.height >> 1 >= th else 0
    w, h, rgba = bitmaps.decode_level(base, m.pixels(base), level)
    mw, mh, mrgba = bitmaps.decode_level(mask, m.pixels(mask), 0)
    plane = texconv.resample_plane(bytes(mrgba[2::4]), mw, mh, w, h)
    rgba = bytearray(rgba)
    rgba[3::4] = plane
    rgba = texconv.resample_rgba(bytes(rgba), w, h, tw, th, True)
    return texconv.convert_rgba(source_id, rgba, tw, th, texconv.CLASS_BLEND, 0, 128)


def resolve_object_shader(m: texconv.MapData, shader_id: int) -> texconv.ShaderInfo:
    """Base map, blend state and uv scale of a model part's shader (and its colour change, if any). Never raises."""
    info = _resolve_object_shader(m, shader_id)
    info.product = texconv.sotr_product(m, shader_id)
    info.reflect = soso_reflect(m, shader_id) or ()
    got = soso_change(m, shader_id)
    if got is not None and info.bitmap_id is not None:
        info.change_source, info.mask_bitmap = got
    return info


def _resolve_object_shader(m: texconv.MapData, shader_id: int) -> texconv.ShaderInfo:
    if shader_id in _LAYERS:
        return resolve_layer(m, shader_id)
    e = m.by_id.get(shader_id)
    if e is None or e.group != "soso":
        return texconv.resolve_shader(m, shader_id)
    info = texconv.ShaderInfo(shader_id, e.group, e.path)
    block = m.tag_off(e)
    if block < 0 or block + SOSO_MIN_SIZE > len(m.region):
        info.note, info.flags = "shader block outside the tag region", texconv.BF_UNTEXTURED
        return info
    flags = struct.unpack_from("<H", m.region, block + SOSO_FLAGS)[0]
    su, sv = struct.unpack_from("<2f", m.region, block + SOSO_SCALE)
    if 0.0 < su <= 4096.0 and 0.0 < sv <= 4096.0:
        info.uv_scale = (su, sv)
    if flags & (SOSO_TWO_SIDED | SOSO_NO_CULL):
        info.flags |= texconv.BF_TWO_SIDED
    if flags & SOSO_DECAL:
        info.flags |= texconv.BF_DECAL
        info.blend, info.keep_alpha = texconv.BLEND_ALPHA, True
    elif not flags & SOSO_NOT_ALPHA_TESTED:
        info.alpha_test = info.keep_alpha = True
        info.blend = texconv.BLEND_ALPHA_TEST
    info.bitmap_id = _bitmap_ref(m, block + SOSO_BASE_MAP)
    if info.bitmap_id is None:
        info.note, info.flags = "no base map", info.flags | texconv.BF_UNTEXTURED
        info.alpha_test, info.blend = False, texconv.BLEND_OPAQUE
    return info


def target_size(w: int, h: int, max_dim: int) -> tuple[int, int]:
    """Half resolution, power of two, 8..max_dim per side."""
    tw, th = max(texconv.MIN_TEX, w // 2), max(texconv.MIN_TEX, h // 2)
    while tw > max_dim or th > max_dim:
        tw, th = max(texconv.MIN_TEX, tw // 2), max(texconv.MIN_TEX, th // 2)
    return tw, th


# Brightness measured against the Xbox intro video: the dome's nebula is a 1x additive layer in its shader, yet it shows about three times as bright
# there as the bitmap's own colours (the tag data gives no reason: the Xbox path is not decoded). Applied to the converted copy only.
SKY_GAIN = {"sky\\sky_x10\\bitmaps\\space": 2.5}


def convert_object_texture(m: texconv.MapData, img: bitmaps.BitmapImage, source_id: int, klass_cap: int,
                           max_dim: int, reflect: tuple | None = None) -> texconv.Ps2Texture:
    """texconv.convert_texture with a configurable size cap and without the colour-key stand-in. A sky bitmap (the whole dome is made of a few of
    them, and they are dark gradients that 16 colours band) keeps its size up to 256 and always gets a 256 colour palette."""
    sky = m.by_id[source_id].path.startswith("sky\\")
    tw, th = target_size(img.width, img.height, max_dim)
    if sky:
        tw, th = min(texconv.MAX_TEX, max(texconv.MIN_TEX, img.width)), min(texconv.MAX_TEX, max(texconv.MIN_TEX, img.height))
    level = 1 if img.mip_count >= 1 and img.width >> 1 >= tw and img.height >> 1 >= th else 0
    w, h, rgba = bitmaps.decode_level(img, m.pixels(img), level)
    found = texconv.alpha_class(rgba)
    klass = min(klass_cap, found)
    alpha_ref = texconv.alpha_threshold(rgba[3::4]) if klass == texconv.CLASS_PUNCH and found == texconv.CLASS_BLEND else 128
    if reflect:                                      # (mask image, rgb): the environment reflection baked into the colour map
        mimg, add = reflect
        mw, mh, mrgba = bitmaps.decode_level(mimg, m.pixels(mimg), 0)
        plane = texconv.resample_plane(bytes(mrgba[0::4]), mw, mh, w, h)
        rgba = bytearray(rgba)
        for i, a in enumerate(plane):
            if a:
                for c in range(3):
                    rgba[4 * i + c] = min(255, rgba[4 * i + c] + add[c] * a // 255)
        rgba = bytes(rgba)
    rgba = texconv.resample_rgba(rgba, w, h, tw, th, klass != texconv.CLASS_OPAQUE)
    gain = SKY_GAIN.get(m.by_id[source_id].path)
    if gain:
        rgba = bytes(min(255, int(v * gain + 0.5)) if i & 3 != 3 else v for i, v in enumerate(rgba))
    tex = texconv.convert_rgba(source_id, rgba, tw, th, klass, 8 if sky or reflect else 0, alpha_ref)
    if img.width < texconv.MIN_TEX or img.height < texconv.MIN_TEX:
        tex.flags |= texconv.TF_UPSCALED
    return tex


PRODUCT_KEY = texconv.PRODUCT_KEY


def convert_product_texture(m: texconv.MapData, imgs: list[bitmaps.BitmapImage], source_id: int, max_dim: int) -> texconv.Ps2Texture:
    tw, th = target_size(max(i.width for i in imgs), max(i.height for i in imgs), max_dim)
    return texconv.product_texture(m, imgs, source_id, max(tw, 32), max(th, 32))


def estimate_bytes(sizes: list[tuple[int, int]], max_dim: int) -> int:
    """Upper bound of the pixel + CLUT bytes of textures whose source sizes are `sizes`."""
    total = 0
    for w, h in sizes:
        tw, th = target_size(w, h, max_dim)
        for lw, lh in texconv.mip_dims(tw, th)[:texconv.MAX_MIPS]:
            total += (lw * lh + 15) // 16 * 16
        total += 1024
    return total


@dataclass
class OtexResult:
    blob: bytes
    shaders: dict[int, texconv.ShaderInfo]
    textures: list[texconv.Ps2Texture]
    max_dim: int
    estimate_dim: int = 256
    untextured_shaders: int = 0
    reasons: dict[str, int] = field(default_factory=dict)

    @property
    def texture_bytes(self) -> int:
        return sum(t.total_bytes for t in self.textures)


def build_otex(m: texconv.MapData, shader_ids: list[int], budget: int = DEFAULT_BUDGET,
               max_dim: int = texconv.MAX_TEX) -> OtexResult:
    """The OTEX blob for the given shader tag ids (sorted, deduplicated here)."""
    ids = sorted(set(shader_ids))
    shaders = {sid: resolve_object_shader(m, sid) for sid in ids}
    reasons: dict[str, int] = {}
    # which bitmaps are actually decodable (decided before sizing the budget)
    plan: dict[tuple[int, int, int], tuple[bitmaps.BitmapImage, int]] = {}
    refl_imgs: dict[int, bitmaps.BitmapImage] = {}
    for s in shaders.values():
        if s.bitmap_id is None:
            reasons[s.note or "no base map"] = reasons.get(s.note or "no base map", 0) + 1
            continue
        try:
            images = m.images(s.bitmap_id)
        except bitmaps.BitmapError:
            images = []
        if not images or not images[0].decodable:
            if s.group in ("sotr", "schi") and s.blend in (texconv.BLEND_ADD, texconv.BLEND_ALPHA):
                s.blank = True                       # a cube map reflection layer: drawn as nothing (see texconv.blank_texture)
                reasons["cube map reflection layer drawn as nothing"] = reasons.get("cube map reflection layer drawn as nothing", 0) + 1
                continue
            s.bitmap_id, s.flags, s.note = None, s.flags | texconv.BF_UNTEXTURED, "base map is not a decodable 2D image"
            reasons[s.note] = reasons.get(s.note, 0) + 1
            continue
        cap = texconv.klass_cap_for(s)
        used_plain = True
        if s.product:
            try:
                pimgs = [m.images(b)[0] for b in s.product]
            except (bitmaps.BitmapError, IndexError):
                pimgs = []
            if pimgs and all(i.decodable for i in pimgs):
                plan.setdefault((s.bitmap_id, cap, PRODUCT_KEY | s.product[-1]), (pimgs, cap))
                used_plain = False
            else:
                s.product = ()
        if s.reflect and not s.product:
            try:
                rimgs = m.images(s.reflect[0])
            except bitmaps.BitmapError:
                rimgs = []
            if rimgs and rimgs[0].decodable:
                refl_imgs[s.reflect[0]] = rimgs[0]
                plan.setdefault((s.bitmap_id, cap, REFLECT_KEY | s.reflect[0]), (images[0], cap))
                used_plain = False
            else:
                s.reflect = ()
        if used_plain:
            plan.setdefault((s.bitmap_id, cap, 0), (images[0], cap))
        if s.mask_bitmap:
            plan.setdefault((s.bitmap_id, -1, s.mask_bitmap), (images[0], cap))
    sizes = [(img.width, img.height) for img, _ in ([(i if not isinstance(i, list) else max(i, key=lambda x: x.width), c) for i, c in plan.values()])]
    dim = max_dim
    while dim > MIN_MAX_DIM and estimate_bytes(sizes, dim) > budget:
        dim //= 2
    textures: list[texconv.Ps2Texture] = []
    seen: dict[tuple[int, int, int], int] = {}
    for sid in ids:
        s = shaders[sid]
        if s.blank:
            add = s.blend == texconv.BLEND_ADD
            bkey = (texconv.PRODUCT_KEY * 2 + int(add), 0, 0)
            if bkey not in seen:
                seen[bkey] = len(textures)
                textures.append(texconv.blank_texture(s.bitmap_id, add))
            s.texture = seen[bkey]
            continue
        if s.bitmap_id is None:
            continue
        key = (s.bitmap_id, texconv.klass_cap_for(s), PRODUCT_KEY | s.product[-1] if s.product else REFLECT_KEY | s.reflect[0] if s.reflect else 0)
        if key not in seen:
            img, cap = plan[key]
            try:
                if s.product:
                    tex = convert_product_texture(m, img, s.bitmap_id, dim)
                else:
                    tex = convert_object_texture(m, img, s.bitmap_id, cap, dim, (refl_imgs[s.reflect[0]], s.reflect[1]) if s.reflect else None)
            except bitmaps.BitmapError as exc:
                s.bitmap_id, s.flags, s.note = None, s.flags | texconv.BF_UNTEXTURED, f"base map failed to decode: {exc}"
                reasons["base map failed to decode"] = reasons.get("base map failed to decode", 0) + 1
                continue
            seen[key] = len(textures)
            textures.append(tex)
        s.texture = seen[key]
        tex = textures[s.texture]
        if s.mask_bitmap and s.change_source:
            key2 = (s.bitmap_id, -1, s.mask_bitmap)
            if key2 not in seen:
                try:
                    mimgs = m.images(s.mask_bitmap)
                    if not mimgs or not mimgs[0].decodable:
                        raise bitmaps.BitmapError("colour change mask is not a decodable 2D image")
                    seen[key2] = len(textures)
                    textures.append(convert_cc_texture(m, plan[key][0], mimgs[0], s.bitmap_id, dim))
                except bitmaps.BitmapError:
                    s.change_source = 0
            if s.change_source:
                s.texture2 = seen[key2]
        if s.group == "soso" and s.alpha_test and not tex.flags & (texconv.TF_PUNCH | texconv.TF_BLEND):
            s.blend = texconv.BLEND_OPAQUE        # the map has no alpha: nothing to test
    blob = texconv.build_texs(textures, [shaders[sid] for sid in ids])
    untextured = sum(1 for s in shaders.values() if s.texture == texconv.NO_TEXTURE)
    return OtexResult(blob, shaders, textures, dim, max_dim, untextured, reasons)
