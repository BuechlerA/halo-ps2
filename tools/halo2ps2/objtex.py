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


def resolve_object_shader(m: texconv.MapData, shader_id: int) -> texconv.ShaderInfo:
    """Base map, blend state and uv scale of a model part's shader. Never raises."""
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


def convert_object_texture(m: texconv.MapData, img: bitmaps.BitmapImage, source_id: int, klass_cap: int,
                           max_dim: int) -> texconv.Ps2Texture:
    """texconv.convert_texture with a configurable size cap and without the colour-key stand-in."""
    tw, th = target_size(img.width, img.height, max_dim)
    level = 1 if img.mip_count >= 1 and img.width >> 1 >= tw and img.height >> 1 >= th else 0
    w, h, rgba = bitmaps.decode_level(img, m.pixels(img), level)
    found = texconv.alpha_class(rgba)
    klass = min(klass_cap, found)
    alpha_ref = texconv.alpha_threshold(rgba[3::4]) if klass == texconv.CLASS_PUNCH and found == texconv.CLASS_BLEND else 128
    rgba = texconv.resample_rgba(rgba, w, h, tw, th, klass != texconv.CLASS_OPAQUE)
    tex = texconv.convert_rgba(source_id, rgba, tw, th, klass, 0, alpha_ref)
    if img.width < texconv.MIN_TEX or img.height < texconv.MIN_TEX:
        tex.flags |= texconv.TF_UPSCALED
    return tex


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
    plan: dict[tuple[int, int], tuple[bitmaps.BitmapImage, int]] = {}
    for s in shaders.values():
        if s.bitmap_id is None:
            reasons[s.note or "no base map"] = reasons.get(s.note or "no base map", 0) + 1
            continue
        try:
            images = m.images(s.bitmap_id)
        except bitmaps.BitmapError:
            images = []
        if not images or not images[0].decodable:
            s.bitmap_id, s.flags, s.note = None, s.flags | texconv.BF_UNTEXTURED, "base map is not a decodable 2D image"
            reasons[s.note] = reasons.get(s.note, 0) + 1
            continue
        cap = texconv.klass_cap_for(s)
        plan.setdefault((s.bitmap_id, cap), (images[0], cap))
    sizes = [(img.width, img.height) for img, _ in plan.values()]
    dim = max_dim
    while dim > MIN_MAX_DIM and estimate_bytes(sizes, dim) > budget:
        dim //= 2
    textures: list[texconv.Ps2Texture] = []
    seen: dict[tuple[int, int], int] = {}
    for sid in ids:
        s = shaders[sid]
        if s.bitmap_id is None:
            continue
        key = (s.bitmap_id, texconv.klass_cap_for(s))
        if key not in seen:
            img, cap = plan[key]
            try:
                tex = convert_object_texture(m, img, s.bitmap_id, cap, dim)
            except bitmaps.BitmapError as exc:
                s.bitmap_id, s.flags, s.note = None, s.flags | texconv.BF_UNTEXTURED, f"base map failed to decode: {exc}"
                reasons["base map failed to decode"] = reasons.get("base map failed to decode", 0) + 1
                continue
            seen[key] = len(textures)
            textures.append(tex)
        s.texture = seen[key]
        tex = textures[s.texture]
        if s.group == "soso" and s.alpha_test and not tex.flags & (texconv.TF_PUNCH | texconv.TF_BLEND):
            s.blend = texconv.BLEND_OPAQUE        # the map has no alpha: nothing to test
    blob = texconv.build_texs(textures, [shaders[sid] for sid in ids])
    untextured = sum(1 for s in shaders.values() if s.texture == texconv.NO_TEXTURE)
    return OtexResult(blob, shaders, textures, dim, max_dim, untextured, reasons)
