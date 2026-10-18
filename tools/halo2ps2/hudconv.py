#!/usr/bin/env python3
"""Convert the gameplay HUD of an Xbox Halo map (hud_globals, unit / weapon / grenade HUD interfaces, fonts, HUD
strings, HUD bitmaps) into the PS2 ``<map>.hud.hp2`` pack with the sections ``HUD0``, ``FONT``, ``STRS`` and ``HUDT``
(docs/20-hud-section.md; reader engine/src/ui/huddef.c, contract engine/src/ui/huddef.h).

Everything below was read off the retail maps (all ten campaign maps; every check is in ``verify`` and the tests).
"verified" = checked against the data (the cross-check is named), "unverified" = taken from the public tag names or a
convention the data does not contradict.  Offsets are inside the tag's own data block; a reflexive is
``u32 count, u32 address, u32 pad``; a tag reference is 16 bytes ``char[4] group (byte reversed), u32 path ptr,
u32 path len, u32 tag id``; ``ColorARGBInt`` is the little-endian u32 0xAARRGGBB, i.e. the BYTES ``B, G, R, A``
(verified: hud_globals carries the same default colour as float ARGB and as ARGBInt).

bitmap (``bitm``) sequences  (the HUD addresses sprites as bitmap tag + sequence index)
    +0x54 reflexive sequences (64 B): +0x00 char[32] name, +0x20 s16 first bitmap index, +0x22 s16 bitmap count,
          +0x34 reflexive sprites (32 B): +0x00 s16 bitmap index, +0x08 f32 left, right, top, bottom (0..1 of that
          image), +0x18 f32 registration x, y (0..1 of the image; the centre of a reticle sprite)
    sprite sheets (type 3): the sequence has sprites and bitmap count 0; interface bitmaps (type 0): no sprites,
    ``count`` whole images from ``first``.  verified: every HUD sequence index of all ten maps is in range, sprite rects
    lie inside 0..1, a sequence's sprites all belong to existing images.
hud_number (``hud#``, +0x7DC of globals): +0x00 bitmap reference, +0x10 s8 bitmap digit width, +0x11 s8 screen digit
    width, +0x12 s8 x offset, +0x13 s8 y offset, +0x14 s8 decimal point width, +0x15 s8 colon width.  The bitmap has 15
    sprites: 0..9 the digits, 10 '.', 11 ':', 12 '-', 13 'm' (verified by rendering), 14 a wide glyph (unknown use).
font (``font``)
    +0x00 u32 flags, +0x04 s16 ascending height, +0x06 descending, +0x08 leading height, +0x0A leading width,
    +0x30 reflexive character tables (256 pages of 256 u16, not used: the character list is searched by code point),
    +0x3C four font references (bold, italic, condense, underline; null on every retail font),
    +0x7C reflexive characters (20 B): u16 code point, s16 advance, s16 bitmap width, s16 bitmap height,
          s16 bitmap origin x, s16 bitmap origin y, u16 hardware index (0xFFFF), u16 pad, s32 pixel offset;
    +0x88 data {u32 size, flags, file offset, pointer, id}: 8-bit coverage, ``width * height`` bytes per glyph, glyph
          after glyph (verified: the sizes sum to ``size`` and every offset is the running sum).
    Glyph placement (verified on the metrics): quad left = pen x + origin x, quad top = baseline - origin y; the advance
    equals origin x + width within one pixel for 90 % of the glyphs ('_' has a negative origin y: below the baseline).
unicode_string_list (``ustr``): +0x00 reflexive strings (20 B: u32 length in BYTES including the NUL, flags, file
    offset, pointer, id); UTF-16LE with NUL.  hud_message_text (``hmt ``): +0x00 data {size, flags, offset, pointer,
    id} (UTF-16LE text, messages separated by NULs), +0x14 reflexive message elements (2 B: u8 type, u8 value;
    type 0 = text of ``value`` UTF-16 units, type 1 = icon ``value``), +0x20 reflexive messages (64 B: char[32] name,
    u16 first text unit, u16 first element, u16 element count).  verified: the text units of a message's text elements
    add up to the next message's first text unit on every message of all ten maps.
globals (``matg``): +0x128 grenades (68 B each, grenade HUD reference at +0x14 of the element), +0x78C hud_globals,
    +0x7DC hud_number.  scenario +0x594: the level's hud_message_text.  weapon tag +0x480: its weapon HUD interface.
hud_globals (``hudg``)
    +0x00 u16 anchor, +0x24 s16 x, y, +0x28 f32 width scale, +0x2C height scale, +0x30 u16 scaling flags
    (zero scales on retail), +0x48 / +0x58 single / multi player font, +0x68 f32 item message scale x, y,
    +0x70 / +0x80 f32 ARGB text colours, +0x94 item message strings (ustr), +0xA4 / +0xB4 icon bitmap / icon message
    strings, +0xC4 reflexive button icons (16 B: u16 sequence, s16 width offset, s16 x, y, ARGBInt colour, u8, u8, u16
    index), +0xF0 hud messages (hmt), +0x150 waypoint bitmap, +0x2C0 default weapon HUD, +0x2D0 f32 motion sensor range,
    velocity sensitivity, scale (unverified names), +0x338 damage arrow bitmap, +0x348 u16 sequence, u16 multiplayer
    sequence, +0x34C ARGBInt colour (unverified names).  Waypoint arrows (+0x160, 104 B) are multiplayer only and not
    converted.
Common HUD element (all three interfaces, 0x24-byte header, then the body)
    +0x00 s16 anchor offset x, y, +0x04 f32 width scale, +0x08 height scale, +0x0C u16 scaling flags, +0x24 body
    static element body (104 B total): +0x24 bitmap, +0x34 ARGBInt default colour, +0x38 flash colour, +0x3C f32 flash
        period, +0x40 flash delay, +0x44 u16 flash count, +0x46 u16 flash flags, +0x48 f32 flash length,
        +0x4C ARGBInt disabled colour, +0x54 u16 sequence index, +0x58 reflexive multitexture overlays
        (not converted).  verified: 104 B stride and field positions on every unit HUD static (bitmap at +0x24 of the next
        element), 180 B weapon static = 0x24 state header + this + 40 B pad.
    meter body (104 B): +0x24 bitmap, +0x34 colour at minimum, +0x38 at maximum, +0x3C flash, +0x40 empty, +0x44 u8 flags,
        +0x45 u8 minimum meter value, +0x46 u16 sequence index, +0x48 u8 alpha multiplier, +0x49 u8 alpha bias,
        +0x4A u16 value scale, +0x4C f32 opacity, +0x50 translucency, +0x54 ARGBInt disabled colour.
    number body: +0x24 default colour (no bitmap: digits come from hud_number), +0x28 flash colour, +0x2C f32 period,
        +0x30 delay, +0x34 u16 count, +0x36 u16 flash flags, +0x38 f32 length, +0x3C disabled colour,
        +0x44 u8 maximum digits, u8 flags (1 leading zeros, 2 only when zoomed, 4 trailing 'm'), u8 fractional digits.
anchors (u16 at the HUD tag's start, HEK order top left, top right, bottom left, bottom right, centre): the anchor points are
    the corners of the 7.5 % title-safe frame of the 640 x 480 screen (48 / 36 px in) and the screen centre; the element's
    matching corner goes to anchor + offset (x right, y down). The unit HUD's motion sensor is always laid out from the
    bottom-left anchor, its centre offset counted inward from that corner (verified against an Xbox frame, ADR-057). The
    motion sensor blip is the bitmap ``ui\\hud\\bitmaps\\hud_sensor_blip`` (found by path, sequence 0).
unit_hud_interface (``unhi``): fixed 0x56C B: +0x00 u16 anchor, then static 0x24 hud background, 0x8C shield panel
    background, 0xF4 shield panel meter, 0x17C health panel background, 0x1E4 health panel meter, 0x26C motion sensor
    background, 0x2D4 foreground (each element 104 B, meters 136 B: 104 + 32 pad), 0x35C motion sensor centre
    (header only), +0x3C0 reflexive sounds (56 B: reference, u32 latched-to bit set, f32 scale), +0x3CC reflexive
    auxiliary overlays (324 B: u16 type, 20 B header, static 104, meter 104, f32 minimum fraction cutoff, u32 flags).
    verified by the strides of the reflexives and the element offsets of all nine a10 unit HUDs.
weapon_hud_interface (``wphi``): fixed 0x17C B: +0x00 child HUD reference, +0x10 u32 flags (bit 0 use parent flashing),
    +0x14 u16 total ammo cutoff, loaded ammo, heat, age cutoff, +0x3C u16 anchor, +0x60 reflexive static elements (180 B:
    u16 state attached to, u16 can use on map type 0 any / 1 solo / 2 multiplayer at +0x04, then the common element at
    +0x24), +0x6C meters (180), +0x78 numbers (160), +0x84 crosshairs (104: u16 type at +0x00, bitmap at +0x24,
    reflexive overlays at +0x34; overlay 108 B: x, y, scales, flags u16 at +0x0C, ARGBInt default colour at +0x24, flash
    colour +0x28, period +0x2C, delay +0x30, u16 count +0x34, flags +0x36, length +0x38, disabled +0x3C, u16 frame rate
    +0x44, u16 sequence index +0x46, u32 flags +0x48), +0x90 overlays (104: state, map type, bitmap +0x24, sub overlay
    reflexive +0x34; sub overlay 136 B: common fields as the crosshair overlay, u16 sequence index +0x48, u16 type
    +0x4A, u32 flags +0x4C), +0x9C u32 crosshair types present (bit n = type n), +0xAC screen effects (not converted),
    +0x13C u16 pickup icon sequence (hud_globals icon bitmap), s16 width offset, s16 x, y.
    verified: the crosshair sequence index at +0x46 (not +0x44) is in range on every crosshair of ten maps and differs
    per weapon, the type bit set equals the crosshair list.
grenade_hud_interface (``grhi``): fixed 0x1F8 B: +0x00 u16 anchor, 0x24 background (104), 0x8C total grenades background
    (104), 0xF4 total grenades numbers, +0x14C overlay bitmap, +0x15C reflexive overlays (136 B, as a weapon sub overlay).
"""
from __future__ import annotations

import argparse
import math
import struct
import sys
import time
import zlib
from dataclasses import dataclass, field
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from halo2ps2 import bitmaps, hp2, tagindex, texconv  # noqa: E402

# ---- format constants (docs/20-hud-section.md) -----------------------------------------------------------------
HUD0_MAGIC, FONT_MAGIC, STRS_MAGIC = b"HUD0", b"FONT", b"STRS"
VERSION = 1                          # FONT / STRS
HUD0_VERSION = 2                     # 2: anchors on the title-safe frame, motion sensor bottom left, blip sprite in the globals
HUD0_HEADER, FONT_HEADER, STRS_HEADER = 128, 64, 64
SPRITE_REC, ELEM_REC, METER_REC, NUMBER_REC, CROSS_REC, HUD_REC = 24, 64, 80, 64, 16, 64
ELEM_FMT = "<BBBBHHIhhhhhhffHBBIIIHHHHHHHH"
METER_FMT = "<BBBBHBBhhhhhhffHBBIIIIIHHff"
NUMBER_FMT = "<BBBBBBBBhhhhhhffHHIIIHHHHHH"
HUD_FMT = "<IBBHIHHHHHHHHHHHHHHIHhhhHHII"
GLOBALS_FMT = "<BBhhHffIIffIIIIIIIIIfffHHIHHbbbbbbHHH"
ICON_FMT = "<HHhhhhIBBHI"
SOUND_REC, ICON_REC, LINK_REC, GLOBALS_REC = 32, 32, 16, 192
FONT_REC, GLYPH_REC = 48, 24
LIST_REC, SET_REC, MSG_REC, SEG_REC, STR_REC = 32, 32, 48, 8, 8
NONE16 = 0xFFFF
NO_STATE = 0xFF
MAX_HUDS, MAX_ELEMS = 255, 4096

VIRT_W, VIRT_H = 640, 480           # the virtual screen every position is resolved to
SAFE_X, SAFE_Y = 48, 36              # the anchors sit on the 7.5 % title-safe frame of that screen (verified on the Xbox frame)
BLIP_BITMAP_PATH = "ui\\hud\\bitmaps\\hud_sensor_blip"   # the motion sensor blip (no HUD tag references it; all ten maps have it)
TICK = 30                            # ticks per second

# hud kind
K_UNIT, K_WEAPON, K_GRENADE = 0, 1, 2
# anchors (HEK order; all five verified, docs/20-hud-section.md)
A_TL, A_TR, A_BL, A_BR, A_C = range(5)
# element roles
(R_NONE, R_HUD_BG, R_SHIELD_BG, R_HEALTH_BG, R_MOTION_BG, R_MOTION_FG, R_MOTION_CENTER, R_AUX_BG, R_STATIC, R_OVERLAY,
 R_CROSSHAIR, R_GRENADE_BG, R_GRENADE_COUNT_BG, R_GRENADE_OVERLAY) = range(14)
MOTION_ROLES = (R_MOTION_BG, R_MOTION_FG, R_MOTION_CENTER)   # always drawn in the bottom-left corner (see element_anchor)
M_SHIELD, M_HEALTH, M_AUX, M_WEAPON = 1, 2, 3, 4
N_WEAPON, N_GRENADE = 1, 2
# weapon "state attached to"
STATES = ("total ammo", "loaded ammo", "heat", "age", "secondary total ammo", "secondary loaded ammo",
          "distance to target", "elevation to target")
# blend codes shared with TEXS bindings (tex.h TEX_BLEND_*)
BLEND_ALPHA, BLEND_ADD = texconv.BLEND_ALPHA, texconv.BLEND_ADD
# link owner kinds
L_WEAPON, L_UNIT, L_GRENADE = 0, 1, 2

# ---- tag layout ---------------------------------------------------------------------------------------------------
UNHI_FIXED, WPHI_FIXED, GRHI_FIXED = 0x56C, 0x17C, 0x1F8
UNHI_STATICS = ((0x24, R_HUD_BG), (0x8C, R_SHIELD_BG), (0x17C, R_HEALTH_BG), (0x26C, R_MOTION_BG), (0x2D4, R_MOTION_FG))
UNHI_METERS = ((0xF4, M_SHIELD), (0x1E4, M_HEALTH))
UNHI_CENTRE, UNHI_SOUNDS, UNHI_AUX = 0x35C, 0x3C0, 0x3CC
STATIC_BODY, METER_BODY, NUMBER_BODY = 0x24, 0x24, 0x24
STATIC_SIZE, UNIT_METER_SIZE, WEAPON_SIZE, WEAPON_NUMBER_SIZE = 104, 136, 180, 160
CROSS_SIZE, CROSS_OVERLAY_SIZE, SUB_OVERLAY_SIZE, OVERLAY_SIZE = 104, 108, 136, 104
AUX_SIZE, SOUND_TAG_SIZE, ICON_TAG_SIZE = 324, 56, 16
MAX_CROSSHAIR_TYPE = 31


class HudError(ValueError):
    pass


# ---- tag region access --------------------------------------------------------------------------------------------
class Region:
    """Bounds-checked reads of the tag region of a decompressed map (offsets are region offsets)."""

    def __init__(self, m: texconv.MapData):
        self.m = m
        self.data = m.region
        self.base = m.base

    def u(self, fmt: str, off: int):
        if off < 0:
            raise HudError(f"negative offset {off}")
        try:
            return struct.unpack_from(fmt, self.data, off)
        except struct.error:
            raise HudError(f"read of {fmt} at {off:#x} outside the tag region") from None

    def reflexive(self, off: int, stride: int, limit: int = 4096) -> tuple[int, int]:
        count, addr = self.u("<II", off)
        if count == 0:
            return 0, 0
        o = addr - self.base
        if count > limit or o < 0 or o + count * stride > len(self.data):
            raise HudError(f"reflexive at {off:#x}: {count} x {stride} at {addr:#x} outside the tag region")
        return count, o

    def tagref(self, off: int) -> tuple[str | None, int]:
        raw, tid = self.u("<4s", off)[0], self.u("<I", off + 12)[0]
        group = tagindex.fourcc(raw)
        if tid == 0xFFFFFFFF or group is None:
            return None, 0xFFFFFFFF
        return group, tid

    def ref(self, off: int, group: str) -> int:
        """Tag id of the reference at ``off`` if it is a live tag of ``group``, else 0."""
        g, tid = self.tagref(off)
        e = self.m.by_id.get(tid)
        return tid if g == group and e is not None and e.group == group else 0

    def entry(self, tid: int) -> tagindex.TagEntry | None:
        return self.m.by_id.get(tid)

    def block(self, e: tagindex.TagEntry) -> int:
        return e.data_address - self.base


def gs_colour(r: int, g: int, b: int, a: int) -> int:
    """8-bit channels -> RGBA word on the GS scale (0x80 = 1.0 per channel), R in the low byte (like a CLUT word)."""
    def s(v: int) -> int:
        return (v * 128 + 127) // 255
    return s(r) | s(g) << 8 | s(b) << 16 | s(a) << 24


def argb_int(raw: bytes) -> int:
    """ColorARGBInt (bytes B, G, R, A) -> GS colour word."""
    b, g, r, a = raw
    return gs_colour(r, g, b, a)


def argb_float(a: float, r: float, g: float, b: float) -> int:
    def c(v: float) -> int:
        return 0 if not math.isfinite(v) else max(0, min(255, int(v * 255 + 0.5)))
    return gs_colour(c(r), c(g), c(b), c(a))


def opaque_if_unset(c: int) -> int:
    """Meters and crosshair overlays carry alpha 0 in every retail colour (the alpha byte is not used there: the aim
    reticle and the shield meter are visible by default), so a colour with alpha 0 and non-zero RGB becomes opaque (0x80).
    An all-zero colour stays zero: it means "not set"."""
    return c | 0x80 << 24 if c >> 24 == 0 and c & 0xFFFFFF else c


def ticks(seconds: float) -> int:
    if not math.isfinite(seconds) or seconds < 0:
        return 0
    return min(0xFFFF, int(seconds * TICK + 0.5))


def finite(*v: float) -> bool:
    return all(math.isfinite(x) for x in v)


# ---- bitmaps: sequences, sprites, textures ----------------------------------------------------------------------
@dataclass
class Sequence:
    first: int
    count: int
    sprites: list[tuple[int, float, float, float, float, float, float]]   # image, l, r, t, b, regx, regy


def parse_sequences(r: Region, tag_block: int) -> list[Sequence]:
    n, o = r.reflexive(tag_block + 0x54, 64, 1024)
    out = []
    for i in range(n):
        q = o + i * 64
        first, count = r.u("<hh", q + 0x20)
        sn, so = r.reflexive(q + 0x34, 32, 1024)
        sprites = []
        for k in range(sn):
            bi, _p, _q2, left, right, top, bottom, rx, ry = r.u("<hhI6f", so + k * 32)
            if not finite(left, right, top, bottom, rx, ry):
                raise HudError("non-finite sprite rectangle")
            sprites.append((bi, left, right, top, bottom, rx, ry))
        out.append(Sequence(first, count, sprites))
    return out


@dataclass
class Sprite:
    tex: int
    flags: int
    w: int
    h: int
    u0: int
    v0: int
    u1: int
    v1: int
    reg_x: int
    reg_y: int
    bitmap_tag: int

    def pack(self) -> bytes:
        return struct.pack("<HHHHHHHHhhI", self.tex, self.flags, self.w, self.h, self.u0, self.v0, self.u1, self.v1,
                           self.reg_x, self.reg_y, self.bitmap_tag)


SF_PADDED, SF_SCALED = 1, 2


def pow2_ceil(v: int) -> int:
    p = 1
    while p < v:
        p <<= 1
    return p


def mask_texture(source_id: int, w: int, h: int, plane: bytes) -> texconv.Ps2Texture:
    """PSMT8 texture whose palette is a white alpha ramp: index = coverage / alpha, so the 8-bit plane is stored
    exactly (no quantisation).  ``plane`` is ``w * h`` bytes."""
    palette = [(255, 255, 255, i) for i in range(256)]
    mean = (sum(plane) + len(plane) // 2) // len(plane)
    return texconv.Ps2Texture(source_id, w, h, texconv.PSMT8, texconv.TF_BLEND, texconv.build_clut(palette, 256), 256,
                              [plane + bytes(-len(plane) % 16)], 0xFFFFFF | texconv.ps2_alpha(mean) << 24, 256, 0.0)


@dataclass
class TexInfo:
    index: int
    tw: int                  # converted texture size
    th: int
    sw: int                  # source image size (px)
    sh: int
    kx: float                # texels per source pixel (1.0 unless the image was halved)
    ky: float
    padded: bool


class Textures:
    """HUDT builder: one texture per (bitmap tag, image) that a sprite uses and one per font atlas page."""

    def __init__(self, m: texconv.MapData | None):
        self.m = m
        self.textures: list[texconv.Ps2Texture] = []
        self.info: dict[tuple[int, int], TexInfo] = {}
        self.problems: list[str] = []

    def add_raw(self, tex: texconv.Ps2Texture) -> int:
        tex.mips = tex.mips[:1]
        self.textures.append(tex)
        return len(self.textures) - 1

    def for_image(self, tid: int, idx: int, img: bitmaps.BitmapImage) -> TexInfo | None:
        key = (tid, idx)
        if key in self.info:
            return self.info[key]
        assert self.m is not None
        if not img.decodable:
            self.problems.append(f"bitmap {tid:#x} image {idx}: not a decodable 2D image")
            self.info[key] = None  # type: ignore[assignment]
            return None
        w, h, rgba = bitmaps.decode_level(img, self.m.pixels(img), 0)
        pw, ph = max(texconv.MIN_TEX, pow2_ceil(w)), max(texconv.MIN_TEX, pow2_ceil(h))
        padded = (pw, ph) != (w, h)
        tw, th = pw, ph
        while tw > texconv.MAX_TEX or th > texconv.MAX_TEX:
            tw, th = max(texconv.MIN_TEX, tw // 2), max(texconv.MIN_TEX, th // 2)
        mask = img.format in (bitmaps.FMT_AY8, bitmaps.FMT_A8)
        if padded:                                           # transparent border right and below
            rows = [rgba[y * w * 4:(y + 1) * w * 4] + bytes((pw - w) * 4) for y in range(h)]
            rgba = b"".join(rows) + bytes((ph - h) * pw * 4)
        if mask:                                             # alpha / intensity only: white, exact 8-bit alpha
            plane = texconv.resample_plane(rgba[3::4], pw, ph, tw, th)
            tex = mask_texture(tid, tw, th, plane)
        else:
            found = texconv.alpha_class(rgba)
            klass = texconv.CLASS_BLEND if found != texconv.CLASS_OPAQUE else texconv.CLASS_OPAQUE
            rgba = texconv.resample_rgba(rgba, pw, ph, tw, th, klass != texconv.CLASS_OPAQUE)
            tex = texconv.convert_rgba(tid, rgba, tw, th, klass)
        info = TexInfo(self.add_raw(tex), tw, th, w, h, tw / pw, th / ph, padded)   # padding does not scale
        self.info[key] = info
        return info


class Sprites:
    """Sprite table: (bitmap tag, sequence) -> consecutive sprite records (one per sprite / image of the sequence)."""

    def __init__(self, r: Region, tex: Textures):
        self.r, self.tex = r, tex
        self.sprites: list[Sprite] = []
        self.cache: dict[tuple[int, int], tuple[int, int]] = {}
        self.seqs: dict[int, list[Sequence]] = {}
        self.problems: list[str] = []

    def sequences(self, tid: int) -> list[Sequence]:
        if tid not in self.seqs:
            e = self.r.entry(tid)
            if e is None or e.group != "bitm":
                raise HudError(f"tag {tid:#x} is not a bitmap")
            self.seqs[tid] = parse_sequences(self.r, self.r.block(e))
        return self.seqs[tid]

    def lookup(self, tid: int, seq: int) -> tuple[int, int]:
        """(first sprite index, frame count) for sequence ``seq`` of bitmap ``tid``; (NONE16, 0) if there is none."""
        if tid == 0:
            return NONE16, 0
        key = (tid, seq)
        if key in self.cache:
            return self.cache[key]
        res = (NONE16, 0)
        try:
            seqs = self.sequences(tid)
            images = self.r.m.images(tid)
            if seq >= len(seqs):
                raise HudError(f"sequence {seq} of {self.r.entry(tid).path!r} out of range ({len(seqs)})")
            s = seqs[seq]
            first = len(self.sprites)
            if s.sprites:
                items = [(bi, left, right, top, bottom, rx, ry) for bi, left, right, top, bottom, rx, ry in s.sprites]
            else:
                if s.count < 1 or s.first < 0 or s.first + s.count > len(images):
                    raise HudError(f"sequence {seq} of {self.r.entry(tid).path!r}: images {s.first}+{s.count} of "
                                   f"{len(images)}")
                items = [(s.first + k, 0.0, 1.0, 0.0, 1.0, 0.5, 0.5) for k in range(s.count)]
            built = []
            for bi, left, right, top, bottom, rx, ry in items:
                if not 0 <= bi < len(images):
                    raise HudError(f"sprite image {bi} out of range ({len(images)})")
                if not (-1e-3 <= left <= right <= 1.001 and -1e-3 <= top <= bottom <= 1.001):
                    raise HudError(f"sprite rectangle {left:.3f} {right:.3f} {top:.3f} {bottom:.3f} not inside the image")
                ti = self.tex.for_image(tid, bi, images[bi])
                if ti is None:
                    raise HudError("image not convertible")
                iw, ih = ti.sw, ti.sh
                sw, sh = max(1, round((right - left) * iw)), max(1, round((bottom - top) * ih))
                u0, u1 = round(left * iw * ti.kx), round(right * iw * ti.kx)
                v0, v1 = round(top * ih * ti.ky), round(bottom * ih * ti.ky)
                u1, v1 = max(u1, u0 + 1), max(v1, v0 + 1)
                built.append(Sprite(ti.index, (SF_PADDED if ti.padded else 0) | (SF_SCALED if ti.kx != 1 or ti.ky != 1 else 0),
                                    sw, sh, u0, v0, min(u1, ti.tw), min(v1, ti.th),
                                    round((rx - left) * iw), round((ry - top) * ih), tid))
            self.sprites += built
            res = (first, len(built))
        except HudError as exc:
            self.problems.append(str(exc))
        self.cache[key] = res
        return res

    def size(self, first: int, scale: tuple[float, float]) -> tuple[int, int]:
        if first == NONE16:
            return 0, 0
        s = self.sprites[first]
        return round(s.w * scale[0]), round(s.h * scale[1])


# ---- HUD data model -------------------------------------------------------------------------------------------------
@dataclass
class Elem:
    role: int
    state: int = NO_STATE
    allowed: int = 0
    frames: int = 0
    sprite: int = NONE16
    flags: int = 0                   # u16: overlay "type" raw (weapon sub overlay) or 0
    ext_flags: int = 0               # u32: overlay / crosshair overlay flags raw
    x: int = 0
    y: int = 0
    w: int = 0
    h: int = 0
    off_x: int = 0
    off_y: int = 0
    sx: float = 1.0
    sy: float = 1.0
    scaling: int = 0
    anchor: int = 0
    blend: int = BLEND_ALPHA
    colour: int = 0
    flash_colour: int = 0
    disabled_colour: int = 0
    flash_period: int = 0
    flash_delay: int = 0
    flash_count: int = 0
    flash_flags: int = 0
    flash_length: int = 0
    frame_rate: int = 0
    seq: int = 0
    type: int = 0

    def pack(self) -> bytes:
        return struct.pack(ELEM_FMT, self.role, self.state, self.allowed, self.frames, self.sprite,
                           self.flags, self.ext_flags, self.x, self.y, self.w, self.h, self.off_x, self.off_y, self.sx,
                           self.sy, self.scaling, self.anchor, self.blend, self.colour, self.flash_colour,
                           self.disabled_colour, self.flash_period, self.flash_delay, self.flash_count,
                           self.flash_flags, self.flash_length, self.frame_rate, self.seq, self.type)


@dataclass
class Meter:
    role: int
    state: int = NO_STATE
    allowed: int = 0
    flags: int = 0
    sprite: int = NONE16
    min_value: int = 0
    anchor: int = 0
    x: int = 0
    y: int = 0
    w: int = 0
    h: int = 0
    off_x: int = 0
    off_y: int = 0
    sx: float = 1.0
    sy: float = 1.0
    scaling: int = 0
    alpha_mult: int = 0
    alpha_bias: int = 0
    colour_min: int = 0
    colour_max: int = 0
    colour_flash: int = 0
    colour_empty: int = 0
    colour_disabled: int = 0
    value_scale: int = 0
    seq: int = 0
    opacity: float = 0.0
    translucency: float = 0.0
    min_fraction: float = 0.0        # auxiliary meters only
    aux_flags: int = 0

    def pack(self) -> bytes:
        return (struct.pack(METER_FMT, self.role, self.state, self.allowed, self.flags, self.sprite,
                            self.min_value, self.anchor, self.x, self.y, self.w, self.h, self.off_x, self.off_y, self.sx,
                            self.sy, self.scaling, self.alpha_mult, self.alpha_bias, self.colour_min, self.colour_max,
                            self.colour_flash, self.colour_empty, self.colour_disabled, self.value_scale, self.seq,
                            self.opacity, self.translucency) + struct.pack("<fI", self.min_fraction, self.aux_flags)
                ).ljust(METER_REC, b"\0")


@dataclass
class Number:
    role: int
    state: int = NO_STATE
    allowed: int = 0
    max_digits: int = 0
    flags: int = 0
    fraction_digits: int = 0
    anchor: int = 0
    x: int = 0
    y: int = 0
    w: int = 0
    h: int = 0
    off_x: int = 0
    off_y: int = 0
    sx: float = 1.0
    sy: float = 1.0
    scaling: int = 0
    colour: int = 0
    flash_colour: int = 0
    disabled_colour: int = 0
    flash_period: int = 0
    flash_delay: int = 0
    flash_count: int = 0
    flash_flags: int = 0
    flash_length: int = 0

    def pack(self) -> bytes:
        return struct.pack(NUMBER_FMT, self.role, self.state, self.allowed, self.max_digits, self.flags,
                           self.fraction_digits, self.anchor, 0, self.x, self.y, self.w, self.h, self.off_x, self.off_y,
                           self.sx, self.sy, self.scaling, 0, self.colour, self.flash_colour, self.disabled_colour,
                           self.flash_period, self.flash_delay, self.flash_count, self.flash_flags, self.flash_length,
                           0).ljust(NUMBER_REC, b"\0")


@dataclass
class Crosshair:
    type: int
    elem_first: int
    elem_count: int
    bitmap_tag: int

    def pack(self) -> bytes:
        return struct.pack("<BBHHHII", self.type, 0, self.elem_first, self.elem_count, 0, self.bitmap_tag, 0)


@dataclass
class HudSound:
    sound_tag: int
    group: str
    latched: int
    scale: float
    hud_tag: int

    def pack(self) -> bytes:
        return struct.pack("<I4sIfI", self.sound_tag, self.group.encode("ascii")[:4].ljust(4, b"\0"), self.latched,
                           self.scale, self.hud_tag).ljust(SOUND_REC, b"\0")


@dataclass
class Hud:
    tag_id: int
    kind: int
    path: str
    anchor: int = 0
    flags: int = 0
    child: int = 0
    total_cutoff: int = 0
    loaded_cutoff: int = 0
    heat_cutoff: int = 0
    age_cutoff: int = 0
    crosshair_mask: int = 0
    icon_seq: int = NONE16
    icon_width_offset: int = 0
    icon_x: int = 0
    icon_y: int = 0
    icon_sprite: int = NONE16
    elems: list[Elem] = field(default_factory=list)
    meters: list[Meter] = field(default_factory=list)
    numbers: list[Number] = field(default_factory=list)
    crosshairs: list[Crosshair] = field(default_factory=list)
    cross_elems: list[Elem] = field(default_factory=list)
    sounds: list[HudSound] = field(default_factory=list)


@dataclass
class Glyph:
    code: int
    advance: int
    w: int
    h: int
    ox: int
    oy: int
    tex: int = NONE16
    u0: int = 0
    v0: int = 0
    u1: int = 0
    v1: int = 0

    def pack(self) -> bytes:
        return struct.pack("<HhHHhhHHHHHH", self.code, self.advance, self.w, self.h, self.ox, self.oy, self.tex,
                           self.u0, self.v0, self.u1, self.v1, 0)


@dataclass
class Font:
    tag_id: int
    path: str
    flags: int
    ascent: int
    descent: int
    leading_h: int
    leading_w: int
    glyphs: list[Glyph]
    tex_first: int = 0
    tex_count: int = 0


@dataclass
class Message:
    name: str
    segments: list[tuple[int, int, str]]        # (kind 0 text / 1 icon, icon value, text)


@dataclass
class Globals:
    tag_id: int = 0
    anchor: int = 0
    off_x: int = 0
    off_y: int = 0
    font_sp: int = 0
    font_mp: int = 0
    item_scale: tuple[float, float] = (1.0, 1.0)
    text_colour_a: int = 0
    text_colour_b: int = 0
    default_weapon_hud: int = 0
    item_strings: int = 0
    icon_strings: int = 0
    messages_tag: int = 0
    level_messages_tag: int = 0
    icon_bitmap: int = 0
    motion: tuple[float, float, float] = (0.0, 0.0, 0.0)
    damage_sprite: int = NONE16
    damage_mp_sprite: int = NONE16
    damage_colour: int = 0
    digit_tag: int = 0
    digit_first: int = NONE16
    digit_count: int = 0
    digit_widths: tuple[int, int, int, int, int, int] = (0, 0, 0, 0, 0, 0)
    icons: list[tuple] = field(default_factory=list)
    blip_tag: int = 0                           # BLIP_BITMAP_PATH bitmap, 0 when the map lacks it
    blip_sprite: int = NONE16                   # its sequence 0 as a sprite (the motion sensor blip)


@dataclass
class HudSet:
    name: str
    globals: Globals
    huds: list[Hud]
    fonts: list[Font]
    lists: list[tuple[int, list[str]]]           # (ustr tag id, strings)
    message_sets: list[tuple[int, list[Message]]]
    links: list[tuple[int, int, int, int]]       # (owner tag / index, kind, hud tag id, order)
    sprites: list[Sprite]
    textures: Textures
    problems: list[str] = field(default_factory=list)
    notes: list[str] = field(default_factory=list)
    seconds: float = 0.0


# ---- layout ---------------------------------------------------------------------------------------------------------
def anchor_point(anchor: int) -> tuple[int, int]:
    """The anchor points are the corners of the title-safe frame (7.5 % in from every edge, i.e. SAFE_X / SAFE_Y) and the
    screen centre (verified against the Xbox frame: ammo panel, shield panel, meters and the motion sensor all land within
    2 px of these points, docs/20-hud-section.md)."""
    ax = VIRT_W - SAFE_X if anchor in (A_TR, A_BR) else VIRT_W // 2 if anchor == A_C else SAFE_X
    ay = VIRT_H - SAFE_Y if anchor in (A_BL, A_BR) else VIRT_H // 2 if anchor == A_C else SAFE_Y
    return ax, ay


def resolve_position(anchor: int, off_x: int, off_y: int, w: int, h: int) -> tuple[int, int]:
    """Top-left corner on the 640 x 480 virtual screen. The element's matching corner (its centre for A_C) sits at the
    anchor point plus the tag's offset (x right, y down at every anchor)."""
    ax, ay = anchor_point(anchor)
    x = ax + off_x - (w if anchor in (A_TR, A_BR) else w // 2 if anchor == A_C else 0)
    y = ay + off_y - (h if anchor in (A_BL, A_BR) else h // 2 if anchor == A_C else 0)
    return x, y


def corner_point(anchor: int, off_x: int, off_y: int) -> tuple[int, int]:
    """A point measured inward from the anchor corner (the motion sensor centre: its (41, 41) is the radar centre of the
    cyborg sensor image counted from the image's bottom-left corner, which the Xbox frame shows at (89, 403))."""
    ax, ay = anchor_point(anchor)
    x = ax - off_x if anchor in (A_TR, A_BR) else ax + off_x
    y = ay - off_y if anchor in (A_BL, A_BR) else ay + off_y
    return x, y


def element_anchor(role: int, hud_anchor: int) -> int:
    """The anchor an element of a unit HUD is laid out from: the HUD's, except the motion sensor, which the engine draws
    in the bottom-left corner whatever the unit HUD's anchor (both retail unit HUDs with a sensor are top-right anchored,
    their sensor images carry the radar in the bottom-left corner of the image, and the Xbox frame shows it bottom left)."""
    return A_BL if role in MOTION_ROLES else hud_anchor


def _scale(sx: float, sy: float) -> tuple[float, float]:
    """A zero width / height scale in a tag means "not set": 1.0."""
    return (sx if finite(sx) and sx > 0 else 1.0, sy if finite(sy) and sy > 0 else 1.0)


# ---- parsing --------------------------------------------------------------------------------------------------------
class Collector:
    def __init__(self, m: texconv.MapData):
        self.m = m
        self.r = Region(m)
        self.tex = Textures(m)
        self.sprites = Sprites(self.r, self.tex)
        self.problems: list[str] = []
        self.notes: list[str] = []
        self.digit_tag = 0

    # -- common elements --
    def _element(self, o: int, role: int, anchor: int, state: int = NO_STATE, allowed: int = 0) -> Elem:
        """Static element body at ``o`` (the common 0x24 header starts there)."""
        r = self.r
        offx, offy, sx, sy, scaling = r.u("<hhffH", o)
        bm = r.ref(o + 0x24, "bitm")
        seq, = r.u("<H", o + 0x54)
        period, delay = r.u("<ff", o + 0x3C)
        fcount, fflags = r.u("<HH", o + 0x44)
        length, = r.u("<f", o + 0x48)
        first, frames = self.sprites.lookup(bm, seq) if bm else (NONE16, 0)
        sc = _scale(sx, sy)
        w, h = self.sprites.size(first, sc)
        x, y = resolve_position(anchor, offx, offy, w, h)
        return Elem(role, state, allowed, frames, first, 0, 0, x, y, w, h, offx, offy, sc[0], sc[1], scaling, anchor,
                    BLEND_ALPHA, argb_int(r.data[o + 0x34:o + 0x38]), argb_int(r.data[o + 0x38:o + 0x3C]),
                    argb_int(r.data[o + 0x4C:o + 0x50]), ticks(period), ticks(delay), fcount, fflags, ticks(length),
                    0, seq, 0)

    def _meter(self, o: int, role: int, anchor: int, state: int = NO_STATE, allowed: int = 0) -> Meter:
        r = self.r
        offx, offy, sx, sy, scaling = r.u("<hhffH", o)
        bm = r.ref(o + 0x24, "bitm")
        flags, minv, seq, amult, abias, vscale = r.u("<BBHBBH", o + 0x44)
        opacity, transl = r.u("<ff", o + 0x4C)
        first, _frames = self.sprites.lookup(bm, seq) if bm else (NONE16, 0)
        sc = _scale(sx, sy)
        w, h = self.sprites.size(first, sc)
        x, y = resolve_position(anchor, offx, offy, w, h)
        return Meter(role, state, allowed, flags, first, minv, anchor, x, y, w, h, offx, offy, sc[0], sc[1], scaling,
                     amult, abias, *(opaque_if_unset(argb_int(r.data[o + k:o + k + 4])) for k in (0x34, 0x38, 0x3C, 0x40, 0x54)),
                     vscale, seq, opacity if finite(opacity) else 0.0, transl if finite(transl) else 0.0)

    def _number(self, o: int, role: int, anchor: int, state: int = NO_STATE, allowed: int = 0) -> Number:
        r = self.r
        offx, offy, sx, sy, scaling = r.u("<hhffH", o)
        period, delay = r.u("<ff", o + 0x2C)
        fcount, fflags = r.u("<HH", o + 0x34)
        length, = r.u("<f", o + 0x38)
        digits, flags, frac = r.u("<BBB", o + 0x44)
        sc = _scale(sx, sy)
        g = self.globals
        # drawn size: the digit sprite height and (digits * screen digit width [+ decimal point]) wide
        h = 0
        if g.digit_first != NONE16:
            h = round(self.sprites.sprites[g.digit_first].h * sc[1])
        w = round((digits * g.digit_widths[1] + (g.digit_widths[4] if frac else 0)) * sc[0])
        x, y = resolve_position(anchor, offx, offy, w, h)
        return Number(role, state, allowed, digits, flags, frac, anchor, x, y, w, h, offx, offy, sc[0], sc[1], scaling,
                      argb_int(r.data[o + 0x24:o + 0x28]), argb_int(r.data[o + 0x28:o + 0x2C]),
                      argb_int(r.data[o + 0x3C:o + 0x40]), ticks(period), ticks(delay), fcount, fflags, ticks(length))

    def _overlay_common(self, o: int, role: int, anchor: int, first: int, frames: int, bm_seq: int) -> Elem:
        """Overlay / crosshair overlay fields shared by both (offset 0x00..0x3C); ``first``/``frames`` resolved by caller."""
        r = self.r
        offx, offy, sx, sy, scaling = r.u("<hhffH", o)
        period, delay = r.u("<ff", o + 0x2C)
        fcount, fflags = r.u("<HH", o + 0x34)
        length, = r.u("<f", o + 0x38)
        sc = _scale(sx, sy)
        w, h = self.sprites.size(first, sc)
        x, y = resolve_position(anchor, offx, offy, w, h)
        return Elem(role, NO_STATE, 0, frames, first, 0, 0, x, y, w, h, offx, offy, sc[0], sc[1], scaling, anchor,
                    BLEND_ALPHA, argb_int(r.data[o + 0x24:o + 0x28]), argb_int(r.data[o + 0x28:o + 0x2C]),
                    argb_int(r.data[o + 0x3C:o + 0x40]), ticks(period), ticks(delay), fcount, fflags, ticks(length),
                    0, bm_seq, 0)

    # -- unit HUD --
    def unit(self, e: tagindex.TagEntry) -> Hud:
        r = self.r
        b = r.block(e)
        h = Hud(e.tag_id, K_UNIT, e.path)
        h.anchor, = r.u("<H", b)
        if h.anchor > A_C:
            raise HudError(f"{e.path}: anchor {h.anchor}")
        for off, role in UNHI_STATICS:
            h.elems.append(self._element(b + off, role, element_anchor(role, h.anchor)))
        for off, role in UNHI_METERS:
            h.meters.append(self._meter(b + off, role, h.anchor))
        # motion sensor centre: header only; x, y = the radar centre on screen (offset counted inward from the bottom-left
        # anchor, the corner the sensor image is drawn from)
        offx, offy, sx, sy, scaling = r.u("<hhffH", b + UNHI_CENTRE)
        sc = _scale(sx, sy)
        x, y = corner_point(A_BL, offx, offy)
        h.elems.append(Elem(R_MOTION_CENTER, NO_STATE, 0, 0, NONE16, 0, 0, x, y, 0, 0, offx, offy, sc[0], sc[1], scaling,
                            A_BL))
        n, o = r.reflexive(b + UNHI_SOUNDS, SOUND_TAG_SIZE, 64)
        for i in range(n):
            q = o + i * SOUND_TAG_SIZE
            g, tid = r.tagref(q)
            latched, scale = r.u("<If", q + 0x10)
            if g is not None and r.entry(tid) is not None:
                h.sounds.append(HudSound(tid, g, latched, scale if finite(scale) else 0.0, e.tag_id))
        n, o = r.reflexive(b + UNHI_AUX, AUX_SIZE, 16)
        for i in range(n):
            q = o + i * AUX_SIZE
            typ, = r.u("<H", q)
            h.elems.append(self._element(q + 0x14, R_AUX_BG, h.anchor))
            m = self._meter(q + 0x14 + STATIC_SIZE, M_AUX, h.anchor)
            m.state = typ
            m.min_fraction, m.aux_flags = r.u("<fI", q + 0x14 + 2 * STATIC_SIZE)
            if not finite(m.min_fraction):
                m.min_fraction = 0.0
            h.meters.append(m)
        return h

    # -- weapon HUD --
    def weapon(self, e: tagindex.TagEntry) -> Hud:
        r = self.r
        b = r.block(e)
        h = Hud(e.tag_id, K_WEAPON, e.path)
        h.child = r.ref(b, "wphi")
        h.flags, h.total_cutoff, h.loaded_cutoff, h.heat_cutoff, h.age_cutoff = r.u("<IHHHH", b + 0x10)
        h.anchor, = r.u("<H", b + 0x3C)
        if h.anchor > A_C:
            raise HudError(f"{e.path}: anchor {h.anchor}")
        h.crosshair_mask, = r.u("<I", b + 0x9C)
        seq, wo, ix, iy = r.u("<Hhhh", b + 0x13C)
        h.icon_seq, h.icon_width_offset, h.icon_x, h.icon_y = seq, wo, ix, iy
        n, o = r.reflexive(b + 0x60, WEAPON_SIZE, 256)
        for i in range(n):
            q = o + i * WEAPON_SIZE
            state, allowed = r.u("<H2xH", q)
            h.elems.append(self._element(q + 0x24, R_STATIC, h.anchor, state, allowed))
        n, o = r.reflexive(b + 0x6C, WEAPON_SIZE, 256)
        for i in range(n):
            q = o + i * WEAPON_SIZE
            state, allowed = r.u("<H2xH", q)
            h.meters.append(self._meter(q + 0x24, M_WEAPON, h.anchor, state, allowed))
        n, o = r.reflexive(b + 0x78, WEAPON_NUMBER_SIZE, 256)
        for i in range(n):
            q = o + i * WEAPON_NUMBER_SIZE
            state, allowed = r.u("<H2xH", q)
            h.numbers.append(self._number(q + 0x24, N_WEAPON, h.anchor, state, allowed))
        n, o = r.reflexive(b + 0x90, OVERLAY_SIZE, 256)
        for i in range(n):
            q = o + i * OVERLAY_SIZE
            state, allowed = r.u("<H2xH", q)
            bm = r.ref(q + 0x24, "bitm")
            sn, so = r.reflexive(q + 0x34, SUB_OVERLAY_SIZE, 256)
            for k in range(sn):
                p = so + k * SUB_OVERLAY_SIZE
                seq, typ, flags = r.u("<HHI", p + 0x48)
                first, frames = self.sprites.lookup(bm, seq) if bm else (NONE16, 0)
                el = self._overlay_common(p, R_OVERLAY, h.anchor, first, frames, seq)
                el.state, el.allowed, el.type, el.ext_flags = state, allowed, typ, flags
                el.frame_rate, = r.u("<H", p + 0x44)
                h.elems.append(el)
        n, o = r.reflexive(b + 0x84, CROSS_SIZE, 64)
        for i in range(n):
            q = o + i * CROSS_SIZE
            typ, = r.u("<H", q)
            if typ > MAX_CROSSHAIR_TYPE:
                raise HudError(f"{e.path}: crosshair type {typ}")
            bm = r.ref(q + 0x24, "bitm")
            sn, so = r.reflexive(q + 0x34, CROSS_OVERLAY_SIZE, 64)
            first_elem = len(h.cross_elems)
            for k in range(sn):
                p = so + k * CROSS_OVERLAY_SIZE
                frate, seq, flags = r.u("<HHI", p + 0x44)
                first, frames = self.sprites.lookup(bm, seq) if bm else (NONE16, 0)
                el = self._overlay_common(p, R_CROSSHAIR, A_C, first, frames, seq)
                el.type, el.ext_flags, el.frame_rate = typ, flags, frate
                el.colour, el.flash_colour, el.disabled_colour = (opaque_if_unset(c) for c in
                                                                  (el.colour, el.flash_colour, el.disabled_colour))
                h.cross_elems.append(el)
            h.crosshairs.append(Crosshair(typ, first_elem, sn, bm))
        return h

    # -- grenade HUD --
    def grenade(self, e: tagindex.TagEntry) -> Hud:
        r = self.r
        b = r.block(e)
        h = Hud(e.tag_id, K_GRENADE, e.path)
        h.anchor, = r.u("<H", b)
        if h.anchor > A_C:
            raise HudError(f"{e.path}: anchor {h.anchor}")
        h.elems.append(self._element(b + 0x24, R_GRENADE_BG, h.anchor))
        h.elems.append(self._element(b + 0x8C, R_GRENADE_COUNT_BG, h.anchor))
        h.numbers.append(self._number(b + 0xF4, N_GRENADE, h.anchor))
        bm = r.ref(b + 0x14C, "bitm")
        n, o = r.reflexive(b + 0x15C, SUB_OVERLAY_SIZE, 64)
        for k in range(n):
            p = o + k * SUB_OVERLAY_SIZE
            seq, typ, flags = r.u("<HHI", p + 0x48)
            first, frames = self.sprites.lookup(bm, seq) if bm else (NONE16, 0)
            el = self._overlay_common(p, R_GRENADE_OVERLAY, h.anchor, first, frames, seq)
            el.type, el.ext_flags = typ, flags
            h.elems.append(el)
        return h

    # -- fonts --
    def font(self, e: tagindex.TagEntry) -> Font:
        r = self.r
        b = r.block(e)
        flags, asc, desc, lead_h, lead_w = r.u("<Ihhhh", b)
        n, o = r.reflexive(b + 0x7C, 20, 4096)
        size, _fl, _fo, ptr = r.u("<IIII", b + 0x88)
        po = ptr - r.base
        if size and (po < 0 or po + size > len(r.data)):
            raise HudError(f"{e.path}: glyph pixels outside the tag region")
        glyphs: list[Glyph] = []
        pixels = {}
        run = 0
        for i in range(n):
            code, adv, bw, bh, ox, oy, _hw, _pad, poff = r.u("<Hhhhhh2H i".replace(" ", ""), o + i * 20)
            bw, bh = max(0, bw), max(0, bh)
            if bw * bh and (poff != run or poff + bw * bh > size):
                raise HudError(f"{e.path}: glyph {code:#x} pixel offset {poff} (expected {run}, size {size})")
            if glyphs and code <= glyphs[-1].code:
                raise HudError(f"{e.path}: characters not strictly ascending at {code:#x}")
            if bw * bh:
                pixels[code] = (bw, bh, r.data[po + poff:po + poff + bw * bh])
                run += bw * bh
            glyphs.append(Glyph(code, adv, bw, bh, ox, oy))
        if run != size:
            raise HudError(f"{e.path}: glyph pixels add up to {run}, tag says {size}")
        font = Font(e.tag_id, e.path, flags, asc, desc, lead_h, lead_w, glyphs)
        font._pixels = pixels  # type: ignore[attr-defined]
        return font

    # -- strings --
    def ustr(self, e: tagindex.TagEntry) -> list[str]:
        r = self.r
        b = r.block(e)
        n, o = r.reflexive(b, 20, 8192)
        out = []
        for i in range(n):
            ln, _f, _fo, ptr = r.u("<IIII", o + i * 20)
            po = ptr - r.base
            if ln % 2 or ln < 2 or po < 0 or po + ln > len(r.data):
                raise HudError(f"{e.path}: string {i} length {ln} / pointer {ptr:#x}")
            raw = r.data[po:po + ln]
            if raw[-2:] != b"\0\0":
                raise HudError(f"{e.path}: string {i} is not NUL terminated")
            out.append(raw[:-2].decode("utf-16le", "replace"))
        return out

    def hmt(self, e: tagindex.TagEntry) -> list[Message]:
        r = self.r
        b = r.block(e)
        size, _f, _fo, ptr = r.u("<IIII", b)
        po = ptr - r.base
        if size % 2 or po < 0 or po + size > len(r.data):
            raise HudError(f"{e.path}: text outside the tag region")
        text = r.data[po:po + size].decode("utf-16le", "replace")
        ne, eo = r.reflexive(b + 0x14, 2, 4096)
        nm, mo = r.reflexive(b + 0x20, 64, 1024)
        elements = [r.u("<BB", eo + i * 2) for i in range(ne)]
        out = []
        starts = [r.u("<H", mo + i * 64 + 0x20)[0] for i in range(nm)]
        for i in range(nm):
            q = mo + i * 64
            name = r.data[q:q + 32].split(b"\0")[0].decode("latin1")
            tstart, estart, ecount = r.u("<HHH", q + 0x20)
            if estart + ecount > ne:
                raise HudError(f"{e.path}: message {name!r} elements {estart}+{ecount} of {ne}")
            segs, pos = [], tstart
            for kind, value in elements[estart:estart + ecount]:
                if kind == 0:
                    if pos + value > len(text):
                        raise HudError(f"{e.path}: message {name!r} text past the end")
                    segs.append((0, 0, text[pos:pos + value].rstrip("\0")))
                    pos += value
                elif kind == 1:
                    segs.append((1, value, ""))
                else:
                    raise HudError(f"{e.path}: message {name!r} element type {kind}")
            nxt = starts[i + 1] if i + 1 < nm else None
            if nxt is not None and pos != nxt:
                raise HudError(f"{e.path}: message {name!r} text ends at {pos}, next message starts at {nxt}")
            out.append(Message(name, segs))
        return out

    # -- globals --
    def hud_globals(self, matg: tagindex.TagEntry) -> Globals:
        r = self.r
        mb = r.block(matg)
        hg = r.ref(mb + 0x78C, "hudg")
        g = Globals(hg)
        he = r.entry(hg)
        if he is None:
            raise HudError("globals has no hud_globals")
        b = r.block(he)
        g.anchor, = r.u("<H", b)
        g.off_x, g.off_y = r.u("<hh", b + 0x24)
        g.font_sp, g.font_mp = r.ref(b + 0x48, "font"), r.ref(b + 0x58, "font")
        g.item_scale = tuple(r.u("<ff", b + 0x68))        # type: ignore[assignment]
        g.text_colour_a = argb_float(*r.u("<4f", b + 0x70))
        g.text_colour_b = argb_float(*r.u("<4f", b + 0x80))
        g.item_strings, g.icon_bitmap, g.icon_strings = r.ref(b + 0x94, "ustr"), r.ref(b + 0xA4, "bitm"), r.ref(b + 0xB4, "ustr")
        g.messages_tag = r.ref(b + 0xF0, "hmt ")
        g.default_weapon_hud = r.ref(b + 0x2C0, "wphi")
        g.motion = tuple(r.u("<3f", b + 0x2D0))           # type: ignore[assignment]
        # hud_number digits (globals +0x7DC)
        g.digit_tag = r.ref(mb + 0x7DC, "hud#")
        de = r.entry(g.digit_tag)
        if de is not None:
            db = r.block(de)
            bm = r.ref(db, "bitm")
            g.digit_widths = tuple(r.u("<6b", db + 0x10))  # type: ignore[assignment]
            if bm:
                g.digit_first, g.digit_count = self.sprites.lookup(bm, 0)
        # button icons (+0xC4, 16 B) as sprites of the icon bitmap
        n, o = r.reflexive(b + 0xC4, ICON_TAG_SIZE, 64)
        for i in range(n):
            q = o + i * ICON_TAG_SIZE
            seq, wo, ox, oy = r.u("<Hhhh", q)
            colour = argb_int(r.data[q + 8:q + 12])
            b12, b13, idx = r.u("<BBH", q + 12)
            sprite = self.sprites.lookup(g.icon_bitmap, seq)[0] if g.icon_bitmap else NONE16
            g.icons.append((sprite, seq, wo, ox, oy, colour, b12, b13, idx))
        # damage arrows
        dbm = r.ref(b + 0x338, "bitm")
        dseq, dmp = r.u("<HH", b + 0x348)
        g.damage_colour = argb_int(r.data[b + 0x34C:b + 0x350])
        if dbm:
            g.damage_sprite = self.sprites.lookup(dbm, dseq)[0]
            g.damage_mp_sprite = self.sprites.lookup(dbm, dmp)[0]
        return g

    def weapon_icon(self, h: Hud, g: Globals) -> None:
        """Pickup icon of a weapon HUD (sequence of hud_globals' icon bitmap). The fields hold uninitialised bytes in
        child HUDs and in one vehicle turret (a text fragment, e.g. 25193): a sequence past the bitmap's list means "no icon"."""
        n = len(self.sprites.sequences(g.icon_bitmap)) if g.icon_bitmap else 0
        if h.icon_seq >= n or abs(h.icon_width_offset) > 256 or abs(h.icon_x) > 640 or abs(h.icon_y) > 480:
            self.notes.append(f"{h.path}: pickup icon fields {h.icon_seq}/{h.icon_width_offset} are not an icon: ignored")
            h.icon_seq, h.icon_width_offset, h.icon_x, h.icon_y = NONE16, 0, 0, 0
            return
        h.icon_sprite = self.sprites.lookup(g.icon_bitmap, h.icon_seq)[0]


def _find_all(m: texconv.MapData, group: str) -> list[tagindex.TagEntry]:
    return [e for e in m.index.entries if e.group == group]


def _scan_refs(r: Region, b: int, size: int, group: str) -> list[int]:
    """Tag ids of references of ``group`` found at 4-aligned offsets of the block, in order of appearance."""
    out = []
    for o in range(0, max(0, size - 16), 4):
        g, p, ln, tid = struct.unpack_from("<4sIII", r.data, b + o)
        if tid != 0xFFFFFFFF and g == group[::-1].encode() and r.ref(b + o, group) == tid and tid not in out:
            out.append(tid)
    return out


def _block_size(m: texconv.MapData, e: tagindex.TagEntry, cap: int = 0x2000) -> int:
    nxt = min((x.data_address for x in m.index.entries if x.data_address > e.data_address and m.index.in_tag_region(x.data_address)),
              default=m.base + len(m.region))
    return min(nxt - e.data_address, cap)


def collect(path: Path) -> HudSet:
    """Everything the gameplay HUD of one map needs, parsed and resolved (bitmaps converted)."""
    return collect_map(texconv.MapData.open(path), path.stem)


def collect_map(m: texconv.MapData, name: str) -> HudSet:
    t0 = time.time()
    c = Collector(m)
    r = c.r
    c.problems = c.sprites.problems
    matg = next(iter(_find_all(m, "matg")), None)
    if matg is None:
        raise HudError("map has no globals tag")

    def guard(what: str, fn, *a):
        try:
            return fn(*a)
        except HudError as exc:
            c.problems.append(f"{what}: {exc}")
            return None

    g = guard("hud_globals", c.hud_globals, matg) or Globals()
    c.globals = g
    huds: list[Hud] = []
    # every HUD tag of the map (the map only contains those it uses)
    for grp, fn in (("unhi", c.unit), ("wphi", c.weapon), ("grhi", c.grenade)):
        for e in _find_all(m, grp):
            h = guard(e.path, fn, e)
            if h is not None:
                huds.append(h)
    if len(huds) > MAX_HUDS:
        raise HudError("too many HUD tags")
    # fonts used by the HUD
    fonts = []
    for tid in (g.font_sp, g.font_mp):
        if tid and all(f.tag_id != tid for f in fonts):
            f = guard(r.entry(tid).path, c.font, r.entry(tid))
            if f is not None:
                fonts.append(f)
    # strings
    lists = []
    for tid in (g.item_strings, g.icon_strings):
        if tid:
            s = guard(r.entry(tid).path, c.ustr, r.entry(tid))
            if s is not None:
                lists.append((tid, s))
    sets = []
    scn = m.index.scenario
    level_msgs = r.ref(r.block(scn) + 0x594, "hmt ")
    g.level_messages_tag = level_msgs
    for tid in (g.messages_tag, level_msgs):
        if tid:
            s = guard(r.entry(tid).path, c.hmt, r.entry(tid))
            if s is not None:
                sets.append((tid, s))
    # links: weapon -> weapon HUD, unit -> unit HUDs, grenade type -> grenade HUD
    links: list[tuple[int, int, int, int]] = []
    hud_ids = {h.tag_id for h in huds}
    for e in _find_all(m, "weap"):
        w = r.ref(r.block(e) + 0x480, "wphi")
        if w:
            links.append((e.tag_id, L_WEAPON, w, 0))
    for grp in ("bipd", "vehi"):
        for e in _find_all(m, grp):
            ids = _scan_refs(r, r.block(e), _block_size(m, e, 0x1000), "unhi")
            links += [(e.tag_id, L_UNIT, t, i) for i, t in enumerate(ids)]
    n, o = r.reflexive(r.block(matg) + 0x128, 68, 16)
    for i in range(n):
        t = r.ref(o + i * 68 + 0x14, "grhi")
        if t:
            links.append((i, L_GRENADE, t, 0))
    for h in huds:
        if h.kind == K_WEAPON:
            c.weapon_icon(h, g)
    for owner, kind, tid, order in links:
        if tid not in hud_ids:
            c.problems.append(f"link {owner:#x}/{kind}: HUD tag {tid:#x} was not converted")
    for tid in {t for _o, _k, t, _i in links} | ({g.default_weapon_hud} - {0}):
        if tid not in hud_ids:
            c.problems.append(f"HUD tag {tid:#x} referenced but not converted")
    # motion sensor blip: sequence 0 of the BLIP_BITMAP_PATH bitmap (looked up by path: no tag references it), last sprite
    for e in _find_all(m, "bitm"):
        if e.path == BLIP_BITMAP_PATH:
            g.blip_tag = e.tag_id
            g.blip_sprite = c.sprites.lookup(e.tag_id, 0)[0]
            break
    if g.blip_sprite == NONE16:
        c.problems.append(f"no motion sensor blip bitmap ({BLIP_BITMAP_PATH})")
    # font atlases (after the bitmap textures so the sprite indices stay small)
    for f in fonts:
        pack_font_atlas(f, c.tex)
    for p in c.tex.problems:
        c.problems.append(p)
    hs = HudSet(name, g, huds, fonts, lists, sets, links, c.sprites.sprites, c.tex, c.problems, c.notes)
    hs.seconds = time.time() - t0
    return hs


def pack_font_atlas(f: Font, tex: Textures, page_w: int = 256, max_h: int = 256) -> None:
    """Glyph coverage planes -> PSMT8 alpha-ramp pages (shelf packing, 1 px gutter), fills the glyph records."""
    pixels = getattr(f, "_pixels", {})
    order = sorted((g for g in f.glyphs if g.w * g.h), key=lambda g: (-g.h, -g.w, g.code))
    pages: list[list[tuple[Glyph, int, int]]] = [[]]
    x = y = shelf = 0
    for g in order:
        if g.w + 1 > page_w:
            raise HudError(f"{f.path}: glyph {g.code:#x} wider than a page")
        if x + g.w + 1 > page_w:
            x, y, shelf = 0, y + shelf, 0
        if y + g.h + 1 > max_h:
            pages.append([])
            x = y = shelf = 0
        pages[-1].append((g, x, y))
        x += g.w + 1
        shelf = max(shelf, g.h + 1)
    f.tex_first = len(tex.textures)
    for page in pages:
        if not page:
            continue
        need_h = max(y + g.h for g, _x, y in page)
        ph = max(texconv.MIN_TEX, pow2_ceil(need_h))
        pw = max(texconv.MIN_TEX, pow2_ceil(max(x + g.w for g, x, _y in page)))
        plane = bytearray(pw * ph)
        for g, gx, gy in page:
            _w, _h, px = (g.w, g.h, pixels[g.code][2])
            for row in range(g.h):
                plane[(gy + row) * pw + gx:(gy + row) * pw + gx + g.w] = px[row * g.w:(row + 1) * g.w]
        ti = tex.add_raw(mask_texture(f.tag_id, pw, ph, bytes(plane)))
        for g, gx, gy in page:
            g.tex, g.u0, g.v0, g.u1, g.v1 = ti, gx, gy, gx + g.w, gy + g.h
    f.tex_count = len(tex.textures) - f.tex_first
    f._pixels = {}     # type: ignore[attr-defined]


# ---- writers ----------------------------------------------------------------------------------------------------------
def _align(b: bytearray, n: int = 16) -> None:
    b += bytes(-len(b) % n)


def build_hud0(hs: HudSet) -> bytes:
    g = hs.globals
    elems: list[Elem] = []
    meters: list[Meter] = []
    numbers: list[Number] = []
    crosses: list[Crosshair] = []
    sounds: list[HudSound] = []
    hud_recs = bytearray()
    for h in hs.huds:
        ef, mf, nf, cf, sf = len(elems), len(meters), len(numbers), len(crosses), len(sounds)
        elems += h.elems
        meters += h.meters
        numbers += h.numbers
        sounds += h.sounds
        cef = len(elems)
        elems += h.cross_elems
        for cr in h.crosshairs:
            crosses.append(Crosshair(cr.type, cef + cr.elem_first, cr.elem_count, cr.bitmap_tag))
        if len(elems) > MAX_ELEMS:
            raise HudError("too many elements")
        hud_recs += struct.pack(HUD_FMT, h.tag_id, h.kind, h.anchor, h.flags, h.child, h.total_cutoff, h.loaded_cutoff,
                                h.heat_cutoff, h.age_cutoff, ef, len(h.elems) + len(h.cross_elems), mf, len(h.meters),
                                nf, len(h.numbers), cf, len(h.crosshairs), sf, len(h.sounds), h.crosshair_mask,
                                h.icon_seq, h.icon_width_offset, h.icon_x, h.icon_y, h.icon_sprite, 0, 0, 0)
    out_icons = bytearray()
    for sprite, seq, wo, ox, oy, colour, b12, b13, idx in g.icons:
        out_icons += struct.pack(ICON_FMT, sprite, seq, wo, ox, oy, 0, colour, b12, b13, idx, 0).ljust(ICON_REC, b"\0")
    sprites = b"".join(s.pack() for s in hs.sprites)
    links = b"".join(struct.pack("<IIII", owner, kind, tid, order) for owner, kind, tid, order in hs.links)
    glob = struct.pack(GLOBALS_FMT, g.anchor, 0, g.off_x, g.off_y, 0, 0.0, 0.0, g.font_sp,
                       g.font_mp, g.item_scale[0], g.item_scale[1], g.text_colour_a, g.text_colour_b, g.default_weapon_hud,
                       g.item_strings, g.icon_strings, g.messages_tag, g.level_messages_tag, g.icon_bitmap, g.digit_tag, *g.motion,
                       g.damage_sprite, g.damage_mp_sprite, g.damage_colour, g.digit_first, g.digit_count, *g.digit_widths,
                       len(g.icons), g.blip_sprite, 0).ljust(GLOBALS_REC, b"\0")
    # section: header, globals, then tables
    body = bytearray()
    offs = {}

    def table(name: str, data: bytes) -> None:
        _align(body)
        offs[name] = HUD0_HEADER + len(body)
        body.extend(data)

    table("globals", glob)
    table("huds", bytes(hud_recs))
    table("elems", b"".join(e.pack() for e in elems))
    table("meters", b"".join(m.pack() for m in meters))
    table("numbers", b"".join(n.pack() for n in numbers))
    table("crosses", b"".join(c.pack() for c in crosses))
    table("sounds", b"".join(s.pack() for s in sounds))
    table("sprites", sprites)
    table("icons", bytes(out_icons))
    table("links", links)
    _align(body)
    total = HUD0_HEADER + len(body)
    head = struct.pack("<4sHHIIIIIIIIIIII", HUD0_MAGIC, HUD0_VERSION, 0, total, len(hs.huds), len(elems), len(meters),
                       len(numbers), len(crosses), len(sounds), len(hs.sprites), len(g.icons), len(hs.links), 0,
                       offs["globals"])
    head += struct.pack("<IIIIIIIII", offs["huds"], offs["elems"], offs["meters"], offs["numbers"], offs["crosses"],
                        offs["sounds"], offs["sprites"], offs["icons"], offs["links"])
    return bytes(head.ljust(HUD0_HEADER, b"\0") + body)


def build_font(hs: HudSet) -> bytes:
    glyphs = bytearray()
    fonts = bytearray()
    n = 0
    for f in hs.fonts:
        fonts += struct.pack("<IIhhhhIIHH", f.tag_id, f.flags, f.ascent, f.descent, f.leading_h, f.leading_w, n,
                             len(f.glyphs), f.tex_first, f.tex_count).ljust(FONT_REC, b"\0")
        for gl in f.glyphs:
            glyphs += gl.pack()
        n += len(f.glyphs)
    off_f = FONT_HEADER
    off_g = (off_f + len(fonts) + 15) & ~15
    total = off_g + len(glyphs)
    head = struct.pack("<4sHHIIIII", FONT_MAGIC, VERSION, 0, total, len(hs.fonts), n, off_f, off_g)
    return bytes(head.ljust(FONT_HEADER, b"\0") + fonts + bytes(off_g - off_f - len(fonts)) + glyphs)


def build_strs(hs: HudSet) -> bytes:
    blob = bytearray()
    strings: list[tuple[int, int]] = []

    def add(text: str) -> tuple[int, int]:
        off = len(blob) // 2
        raw = text.encode("utf-16le")
        blob.extend(raw + b"\0\0")
        strings.append((off, len(raw) // 2))
        return len(strings) - 1, off

    lists = bytearray()
    for tid, strs in hs.lists:
        first = len(strings)
        for s in strs:
            add(s)
        lists += struct.pack("<III", tid, len(strs), first).ljust(LIST_REC, b"\0")
    sets = bytearray()
    msgs = bytearray()
    segs = bytearray()
    nseg = nmsg = 0
    for tid, ms in hs.message_sets:
        sets += struct.pack("<III", tid, len(ms), nmsg).ljust(SET_REC, b"\0")
        for msg in ms:
            msgs += struct.pack("<32sHH", msg.name.encode("latin1")[:31], nseg, len(msg.segments)).ljust(MSG_REC, b"\0")
            for kind, value, text in msg.segments:
                if kind == 0:
                    _i, off = add(text)
                    segs += struct.pack("<BBHI", 0, 0, len(text), off)
                else:
                    segs += struct.pack("<BBHI", 1, value, 0, 0)
            nseg += len(msg.segments)
        nmsg += len(ms)
    table = b"".join(struct.pack("<II", off, ln) for off, ln in strings)
    parts = [("lists", bytes(lists)), ("sets", bytes(sets)), ("msgs", bytes(msgs)), ("segs", bytes(segs)),
             ("table", table), ("chars", bytes(blob))]
    offs, cur = {}, STRS_HEADER
    body = bytearray()
    for name, data in parts:
        pad = -cur % 16
        body += bytes(pad)
        cur += pad
        offs[name] = cur
        body += data
        cur += len(data)
    cur_pad = -cur % 16
    body += bytes(cur_pad)
    total = cur + cur_pad
    head = struct.pack("<4sHHIIIIIIIIIIII", STRS_MAGIC, VERSION, 0, total, len(hs.lists), len(hs.message_sets), nmsg, nseg,
                       len(strings), len(blob) // 2, offs["lists"], offs["sets"], offs["msgs"], offs["segs"], offs["table"])
    head += struct.pack("<I", offs["chars"])
    return bytes(head.ljust(STRS_HEADER, b"\0") + body)


def build_hudt(hs: HudSet) -> bytes:
    return texconv.build_texs(hs.textures.textures, [])


def build_pack(hs: HudSet) -> bytes:
    return hp2.build(hs.name, [("HUD0", build_hud0(hs)), ("FONT", build_font(hs)), ("STRS", build_strs(hs)),
                               ("HUDT", build_hudt(hs))])


# ---- reference parsers (the Python twin of engine/src/ui/huddef.c) -----------------------------------------------------
def _u(data: bytes, fmt: str, off: int):
    try:
        return struct.unpack_from(fmt, data, off)
    except struct.error:
        raise HudError(f"read at {off:#x} outside the section") from None


def parse_hud0(data: bytes) -> dict:
    if len(data) < HUD0_HEADER or data[:4] != HUD0_MAGIC:
        raise HudError("not a HUD0 section")
    (_m, ver, _r, total, nh, ne, nm, nn, nc, ns, nsp, ni, nl, _z, og) = _u(data, "<4sHHIIIIIIIIIIII", 0)
    oh, oe, om, on, oc, os_, osp, oi, ol = _u(data, "<IIIIIIIII", 0x38)
    if ver != HUD0_VERSION or total != len(data):
        raise HudError("bad HUD0 header")
    tabs = {"huds": (oh, nh, HUD_REC), "elems": (oe, ne, ELEM_REC), "meters": (om, nm, METER_REC),
            "numbers": (on, nn, NUMBER_REC), "crosses": (oc, nc, CROSS_REC), "sounds": (os_, ns, SOUND_REC),
            "sprites": (osp, nsp, SPRITE_REC), "icons": (oi, ni, ICON_REC), "links": (ol, nl, LINK_REC)}
    for name, (off, cnt, rec) in tabs.items():
        if off % 16 or off < HUD0_HEADER or off + cnt * rec > len(data):
            raise HudError(f"table {name} outside the section")
    out: dict = {"globals_offset": og, "total": total}
    out["sprites"] = [dict(zip(("tex", "flags", "w", "h", "u0", "v0", "u1", "v1", "reg_x", "reg_y", "bitmap"),
                               _u(data, "<HHHHHHHHhhI", osp + i * SPRITE_REC))) for i in range(nsp)]
    elem_keys = ("role", "state", "allowed", "frames", "sprite", "flags", "ext_flags", "x", "y", "w", "h", "off_x",
                 "off_y", "sx", "sy", "scaling", "anchor", "blend", "colour", "flash_colour", "disabled_colour",
                 "flash_period", "flash_delay", "flash_count", "flash_flags", "flash_length", "frame_rate", "seq", "type")
    out["elems"] = [dict(zip(elem_keys, _u(data, ELEM_FMT, oe + i * ELEM_REC))) for i in range(ne)]
    meter_keys = ("role", "state", "allowed", "flags", "sprite", "min_value", "anchor", "x", "y", "w", "h", "off_x", "off_y",
                  "sx", "sy", "scaling", "alpha_mult", "alpha_bias", "colour_min", "colour_max", "colour_flash",
                  "colour_empty", "colour_disabled", "value_scale", "seq", "opacity", "translucency")
    out["meters"] = []
    for i in range(nm):
        base = om + i * METER_REC
        d = dict(zip(meter_keys, _u(data, METER_FMT, base)))
        d["min_fraction"], d["aux_flags"] = _u(data, "<fI", base + struct.calcsize(METER_FMT))
        out["meters"].append(d)
    number_keys = ("role", "state", "allowed", "max_digits", "flags", "fraction_digits", "anchor", "pad", "x", "y", "w", "h",
                   "off_x", "off_y", "sx", "sy", "scaling", "pad2", "colour", "flash_colour", "disabled_colour",
                   "flash_period", "flash_delay", "flash_count", "flash_flags", "flash_length", "pad3")
    out["numbers"] = [dict(zip(number_keys, _u(data, NUMBER_FMT, on + i * NUMBER_REC))) for i in range(nn)]
    out["crosses"] = [dict(zip(("type", "pad", "elem_first", "elem_count", "pad2", "bitmap", "pad3"),
                               _u(data, "<BBHHHII", oc + i * CROSS_REC))) for i in range(nc)]
    out["sounds"] = [dict(zip(("sound", "group", "latched", "scale", "hud"), _u(data, "<I4sIfI", os_ + i * SOUND_REC)))
                     for i in range(ns)]
    out["icons"] = [dict(zip(("sprite", "seq", "width_offset", "x", "y", "pad0", "colour", "b12", "b13", "index", "pad"),
                             _u(data, ICON_FMT, oi + i * ICON_REC))) for i in range(ni)]
    out["links"] = [dict(zip(("owner", "kind", "hud", "order"), _u(data, "<IIII", ol + i * LINK_REC))) for i in range(nl)]
    hud_keys = ("tag", "kind", "anchor", "flags", "child", "total_cutoff", "loaded_cutoff", "heat_cutoff", "age_cutoff",
                "elem_first", "elem_count", "meter_first", "meter_count", "number_first", "number_count", "cross_first",
                "cross_count", "sound_first", "sound_count", "crosshair_mask", "icon_seq", "icon_width_offset", "icon_x",
                "icon_y", "icon_sprite", "pad", "pad2", "pad3")
    out["huds"] = [dict(zip(hud_keys, _u(data, HUD_FMT, oh + i * HUD_REC))) for i in range(nh)]
    gk = ("anchor", "pad", "off_x", "off_y", "scaling", "wscale", "hscale", "font_sp", "font_mp", "item_scale_x",
          "item_scale_y", "text_colour_a", "text_colour_b", "default_weapon_hud", "item_strings", "icon_strings",
          "messages", "level_messages", "icon_bitmap", "digit_tag", "motion_range", "motion_velocity", "motion_scale",
          "damage_sprite", "damage_mp_sprite", "damage_colour", "digit_first", "digit_count", "dw0", "dw1", "dw2", "dw3",
          "dw4", "dw5", "icon_count", "blip_sprite", "pad3")
    out["globals"] = dict(zip(gk, _u(data, GLOBALS_FMT, og)))
    return out


def parse_font(data: bytes) -> dict:
    if len(data) < FONT_HEADER or data[:4] != FONT_MAGIC:
        raise HudError("not a FONT section")
    _m, ver, _r, total, nf, ng, of, og = _u(data, "<4sHHIIIII", 0)
    if ver != VERSION or total != len(data) or of + nf * FONT_REC > len(data) or og + ng * GLYPH_REC > len(data):
        raise HudError("bad FONT header")
    fonts = []
    for i in range(nf):
        tid, fl, asc, desc, lh, lw, gf, gc, tf, tc = _u(data, "<IIhhhhIIHH", of + i * FONT_REC)
        if gf + gc > ng:
            raise HudError("font glyph range")
        gl = [dict(zip(("code", "advance", "w", "h", "ox", "oy", "tex", "u0", "v0", "u1", "v1", "pad"),
                       _u(data, "<HhHHhhHHHHHH", og + (gf + k) * GLYPH_REC))) for k in range(gc)]
        fonts.append({"tag": tid, "flags": fl, "ascent": asc, "descent": desc, "leading_h": lh, "leading_w": lw,
                      "tex_first": tf, "tex_count": tc, "glyphs": gl})
    return {"fonts": fonts}


def parse_strs(data: bytes) -> dict:
    if len(data) < STRS_HEADER or data[:4] != STRS_MAGIC:
        raise HudError("not a STRS section")
    (_m, ver, _r, total, nl, ns, nmsg, nseg, nstr, nchars, ol, os_, om, oseg, otab) = _u(data, "<4sHHIIIIIIIIIIII", 0)
    oc, = _u(data, "<I", 0x38)
    if ver != VERSION or total != len(data):
        raise HudError("bad STRS header")
    table = [_u(data, "<II", otab + i * STR_REC) for i in range(nstr)]

    def text(off: int, ln: int) -> str:
        if off + ln > nchars:
            raise HudError("string outside the character data")
        return data[oc + off * 2:oc + (off + ln) * 2].decode("utf-16le")

    lists = []
    for i in range(nl):
        tid, cnt, first = _u(data, "<III", ol + i * LIST_REC)
        lists.append({"tag": tid, "strings": [text(*table[first + k]) for k in range(cnt)]})
    segs = [_u(data, "<BBHI", oseg + i * SEG_REC) for i in range(nseg)]
    msgs = []
    for i in range(nmsg):
        name, sf, sc = _u(data, "<32sHH", om + i * MSG_REC)
        parts = []
        for kind, value, ln, off in segs[sf:sf + sc]:
            parts.append((0, 0, text(off, ln)) if kind == 0 else (1, value, ""))
        msgs.append({"name": name.split(b"\0")[0].decode("latin1"), "segments": parts})
    sets = []
    for i in range(ns):
        tid, cnt, first = _u(data, "<III", os_ + i * SET_REC)
        sets.append({"tag": tid, "messages": msgs[first:first + cnt]})
    return {"lists": lists, "sets": sets}


# ---- verification -----------------------------------------------------------------------------------------------------
def on_screen(x: int, y: int, w: int, h: int) -> bool:
    """The whole drawn rectangle lies on the 640 x 480 screen (the anchor rule is checked with this on every element,
    meter and number of all ten maps: none is clipped)."""
    return 0 <= x and 0 <= y and x + w <= VIRT_W and y + h <= VIRT_H


def verify(hs: HudSet) -> list[str]:
    """Range / consistency checks of a converted map; returns problems (empty on retail maps)."""
    bad: list[str] = list(hs.problems)
    ns = len(hs.sprites)
    nt = len(hs.textures.textures)
    for s in hs.sprites:
        if s.tex >= nt:
            bad.append("sprite texture out of range")
            continue
        t = hs.textures.textures[s.tex]
        if not (0 <= s.u0 < s.u1 <= t.width and 0 <= s.v0 < s.v1 <= t.height):
            bad.append(f"sprite rectangle outside its texture ({s.u0},{s.v0},{s.u1},{s.v1} in {t.width}x{t.height})")
    ids = set()
    for h in hs.huds:
        ids.add(h.tag_id)
        for e in h.elems + h.cross_elems:
            if e.sprite != NONE16 and not (e.sprite + max(1, e.frames) <= ns):
                bad.append(f"{h.path}: element sprite {e.sprite}+{e.frames} of {ns}")
            if not -VIRT_W <= e.x <= 2 * VIRT_W or not -VIRT_H <= e.y <= 2 * VIRT_H:
                bad.append(f"{h.path}: element at {e.x},{e.y} far outside the screen")
            if e.sprite != NONE16 and not on_screen(e.x, e.y, e.w, e.h):
                bad.append(f"{h.path}: element {e.role} drawn outside the screen ({e.x},{e.y} {e.w}x{e.h})")
        for mt in h.meters:
            if mt.sprite != NONE16 and not on_screen(mt.x, mt.y, mt.w, mt.h):
                bad.append(f"{h.path}: meter {mt.role} drawn outside the screen ({mt.x},{mt.y} {mt.w}x{mt.h})")
        for nb in h.numbers:
            if not on_screen(nb.x, nb.y, nb.w, nb.h):
                bad.append(f"{h.path}: number drawn outside the screen ({nb.x},{nb.y} {nb.w}x{nb.h})")
            if not (0.05 <= e.sx <= 16 and 0.05 <= e.sy <= 16):
                bad.append(f"{h.path}: element scale {e.sx} {e.sy}")
        for mt in h.meters:
            if mt.sprite != NONE16 and mt.sprite >= ns:
                bad.append(f"{h.path}: meter sprite {mt.sprite} of {ns}")
            if mt.sprite != NONE16 and not (mt.colour_min | mt.colour_max) & 0xFFFFFF:
                bad.append(f"{h.path}: meter with a sprite and no colour")
            if not (0.0 <= mt.opacity <= 1.0 and 0.0 <= mt.translucency <= 1.0):
                bad.append(f"{h.path}: meter opacity {mt.opacity} / translucency {mt.translucency}")
        for nb in h.numbers:
            if not 1 <= nb.max_digits <= 12 or nb.flags > 7 or nb.fraction_digits > 4:
                bad.append(f"{h.path}: number digits {nb.max_digits} flags {nb.flags} fraction {nb.fraction_digits}")
        if h.crosshairs:
            mask = 0
            for cr in h.crosshairs:
                mask |= 1 << cr.type
            if mask != h.crosshair_mask:
                bad.append(f"{h.path}: crosshair type mask {h.crosshair_mask:#x} != list {mask:#x}")
        if h.child and h.child not in {x.tag_id for x in hs.huds}:
            bad.append(f"{h.path}: child HUD {h.child:#x} not converted")
    g = hs.globals
    if g.digit_first == NONE16 or g.digit_count < 13:
        bad.append("hud_number digit sprites missing")
    if g.blip_sprite != NONE16 and g.blip_sprite >= ns:
        bad.append("blip sprite out of range")
    if not hs.fonts:
        bad.append("no fonts")
    for f in hs.fonts:
        for gl in f.glyphs:
            if gl.w * gl.h and gl.tex == NONE16:
                bad.append(f"font {f.path}: glyph {gl.code:#x} has no atlas position")
    return bad


def report(hs: HudSet) -> str:
    tex = hs.textures.textures
    pix = sum(t.pixel_bytes for t in tex)
    clut = sum(len(t.clut) for t in tex)
    kinds = {K_UNIT: "unit", K_WEAPON: "weapon", K_GRENADE: "grenade"}
    counts = {k: sum(1 for h in hs.huds if h.kind == i) for i, k in kinds.items()}
    lines = [f"{hs.name}: {len(hs.huds)} HUD tags ({', '.join(f'{v} {k}' for k, v in sorted(counts.items()))}), "
             f"{sum(len(h.elems) + len(h.cross_elems) for h in hs.huds)} elements, {sum(len(h.meters) for h in hs.huds)} meters, "
             f"{sum(len(h.numbers) for h in hs.huds)} numbers, {sum(len(h.crosshairs) for h in hs.huds)} crosshairs, "
             f"{len(hs.sprites)} sprites",
             f"  fonts: {', '.join(f'{f.path} ({len(f.glyphs)} glyphs)' for f in hs.fonts)}",
             f"  strings: {sum(len(s) for _t, s in hs.lists)} in {len(hs.lists)} lists, "
             f"{sum(len(m) for _t, m in hs.message_sets)} messages in {len(hs.message_sets)} sets; links {len(hs.links)}",
             f"  textures: {len(tex)} ({sum(1 for t in tex if t.psm == texconv.PSMT4)} PSMT4), pixels {pix / 1024:.0f} KiB + "
             f"CLUTs {clut / 1024:.0f} KiB = {(pix + clut) / 1024:.0f} KiB, {hs.seconds:.1f} s"]
    for p in hs.problems:
        lines.append(f"  PROBLEM: {p}")
    return "\n".join(lines)


# ---- PNG rendering (host check) -----------------------------------------------------------------------------------------
def write_png(path: Path, w: int, h: int, rgba: bytes) -> None:
    raw = b"".join(b"\0" + rgba[y * w * 4:(y + 1) * w * 4] for y in range(h))

    def chunk(tag: bytes, d: bytes) -> bytes:
        c = struct.pack(">I", len(d)) + tag + d
        return c + struct.pack(">I", zlib.crc32(tag + d) & 0xFFFFFFFF)
    path.write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)) +
                     chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))


def texture_rgba(view: texconv.TexsView, i: int) -> tuple[int, int, bytes]:
    """HUDT texture ``i`` (mip 0) as RGBA8 (alpha 0..255)."""
    t = view.textures[i]
    w, h, px = view.mip(t, 0)
    pal = texconv.parse_clut(view.data[t["clut_offset"]:t["clut_offset"] + t["clut_entries"] * 4], t["clut_entries"])
    idx = px[:w * h] if t["psm"] == texconv.PSMT8 else texconv.unpack_nibbles(px)[:w * h]
    out = bytearray(w * h * 4)
    lut = [bytes((r, g, b, min(255, a * 255 // 128))) for r, g, b, a in pal]
    for k, v in enumerate(idx):
        out[k * 4:k * 4 + 4] = lut[v]
    return w, h, bytes(out)


def _over(dst: bytearray, w: int, h: int, x: int, y: int, sw: int, sh: int, src, tint: tuple[int, int, int, int]) -> None:
    """Alpha-blend a sprite (``src(u, v) -> r,g,b,a`` over its size) into an RGB canvas."""
    for yy in range(sh):
        py = y + yy
        if not 0 <= py < h:
            continue
        for xx in range(sw):
            px = x + xx
            if not 0 <= px < w:
                continue
            r, g, b, a = src(xx, yy)
            a = a * tint[3] // 255
            o = (py * w + px) * 4
            dst[o] = (dst[o] * (255 - a) + r * tint[0] // 255 * a) // 255
            dst[o + 1] = (dst[o + 1] * (255 - a) + g * tint[1] // 255 * a) // 255
            dst[o + 2] = (dst[o + 2] * (255 - a) + b * tint[2] // 255 * a) // 255


def render_preview(hud0: dict, view: texconv.TexsView, hud_tags: list[int], text_lines: list[tuple[int, int, str]] | None = None,
                   fonts: dict | None = None, cross_types: tuple[int, ...] = (0,), bg=(24, 40, 24)) -> tuple[int, int, bytes]:
    """Compose the elements of the given HUD tags on a 640 x 480 canvas at their resolved positions (default colour,
    alpha blend, crosshairs of the given types, meters full). A checking aid, not the engine's renderer."""
    W, H = VIRT_W, VIRT_H
    canvas = bytearray(bytes(bg + (255,)) * (W * H))
    cache: dict[int, tuple[int, int, bytes]] = {}

    def tex(i: int):
        if i not in cache:
            cache[i] = texture_rgba(view, i)
        return cache[i]

    def tint_of(c: int) -> tuple[int, int, int, int]:
        r, g, b, a = c & 255, c >> 8 & 255, c >> 16 & 255, c >> 24
        return min(255, r * 2), min(255, g * 2), min(255, b * 2), min(255, a * 2)

    def draw(sprite: int, x: int, y: int, w: int, h: int, colour: int) -> None:
        if sprite == NONE16:
            return
        s = hud0["sprites"][sprite]
        tw, th, px = tex(s["tex"])
        su, sv = s["u1"] - s["u0"], s["v1"] - s["v0"]

        def src(xx: int, yy: int):
            u = s["u0"] + min(su - 1, xx * su // max(1, w))
            v = s["v0"] + min(sv - 1, yy * sv // max(1, h))
            return px[(v * tw + u) * 4:(v * tw + u) * 4 + 4]
        _over(canvas, W, H, x, y, w, h, src, tint_of(colour))

    for tag in hud_tags:
        hud = next(h for h in hud0["huds"] if h["tag"] == tag)
        chain = [hud]
        while chain[-1]["child"] and len(chain) < 4:
            chain.append(next(h for h in hud0["huds"] if h["tag"] == chain[-1]["child"]))
        for hh in chain:
            for e in hud0["elems"][hh["elem_first"]:hh["elem_first"] + hh["elem_count"]]:
                if e["role"] == R_CROSSHAIR:
                    continue
                if e["allowed"] == 2:
                    continue
                draw(e["sprite"], e["x"], e["y"], e["w"], e["h"], e["colour"])
            for m in hud0["meters"][hh["meter_first"]:hh["meter_first"] + hh["meter_count"]]:
                if m["allowed"] == 2:
                    continue
                draw(m["sprite"], m["x"], m["y"], m["w"], m["h"], m["colour_max"] if m["colour_max"] >> 24 else m["colour_min"] | 0x40000000)
        for hh in chain:
            for cr in hud0["crosses"][hh["cross_first"]:hh["cross_first"] + hh["cross_count"]]:
                if cr["type"] not in cross_types:
                    continue
                for e in hud0["elems"][cr["elem_first"]:cr["elem_first"] + cr["elem_count"]]:
                    if e["ext_flags"] & 4:                       # only when zoomed
                        continue
                    draw(e["sprite"], e["x"], e["y"], e["w"], e["h"], e["colour"])
    if text_lines and fonts:
        f = fonts["fonts"][0]
        glyphs = {g["code"]: g for g in f["glyphs"]}
        for x, y, s in text_lines:
            pen = x
            for ch in s:
                g = glyphs.get(ord(ch))
                if g is None:
                    continue
                if g["w"] * g["h"]:
                    tw, th, px = tex(g["tex"])
                    _over(canvas, W, H, pen + g["ox"], y - g["oy"], g["w"], g["h"],
                          lambda xx, yy, g=g, tw=tw, px=px: px[((g["v0"] + yy) * tw + g["u0"] + xx) * 4:((g["v0"] + yy) * tw + g["u0"] + xx) * 4 + 4],
                          (255, 255, 255, 255))
                pen += g["advance"]
    return W, H, bytes(canvas)


def dump_png(pack_path: Path, outdir: Path, scale: int = 2) -> list[Path]:
    """Render every HUDT texture (checkerboard under the alpha), a font specimen and a composed preview of the first
    unit HUD plus the default weapon HUD to PNG files in ``outdir``."""
    outdir.mkdir(parents=True, exist_ok=True)
    pk = hp2.parse(pack_path.read_bytes())
    view = texconv.parse_texs(pk.read("HUDT"))
    hud0 = parse_hud0(pk.read("HUD0"))
    fonts = parse_font(pk.read("FONT"))
    strs = parse_strs(pk.read("STRS"))
    written: list[Path] = []
    for i, t in enumerate(view.textures):
        w, h, px = texture_rgba(view, i)
        cw, ch = w * scale, h * scale
        out = bytearray(cw * ch * 4)
        for y in range(ch):
            for x in range(cw):
                bgv = 70 if (x // 8 + y // 8) % 2 else 40
                r, g, b, a = px[((y // scale) * w + x // scale) * 4:((y // scale) * w + x // scale) * 4 + 4]
                o = (y * cw + x) * 4
                out[o] = (r * a + bgv * (255 - a)) // 255
                out[o + 1] = (g * a + bgv * (255 - a)) // 255
                out[o + 2] = (b * a + bgv * (255 - a)) // 255
                out[o + 3] = 255
        p = outdir / f"{pack_path.name.split('.')[0]}_tex{i:02d}_{w}x{h}.png"
        write_png(p, cw, ch, bytes(out))
        written.append(p)
    # font specimen: the printable ASCII of the first font on 640 x 480
    msgs = [m for s in strs["sets"] for m in s["messages"]]
    sample = ["The quick brown fox jumps over the lazy dog 0123456789", "ABCDEFGHIJKLMNOPQRSTUVWXYZ abcdefghijklmnopqrstuvwxyz"]
    for m in msgs[:6]:
        sample.append("".join(t if k == 0 else f"[icon {v}]" for k, v, t in m["segments"]))
    y = 40
    lines = []
    for f in fonts["fonts"][:1]:
        for s in sample:
            lines.append((20, y, s))
            y += f["ascent"] + f["descent"] + f["leading_h"] + 4
    unit = next((h["tag"] for h in hud0["huds"] if h["kind"] == K_UNIT and h["elem_count"] and h["anchor"] == A_TL), None)
    w, h, px = render_preview(hud0, view, [], lines, fonts, bg=(30, 30, 30))
    p = outdir / f"{pack_path.name.split('.')[0]}_font.png"
    write_png(p, w, h, px)
    written.append(p)
    # composed HUD: the cyborg HUD (first unit HUD with a shield meter) and the assault-rifle-like weapon HUD
    cyborg = next((h["tag"] for h in hud0["huds"] if h["kind"] == K_UNIT and any(
        m["role"] == M_SHIELD and m["sprite"] != NONE16 for m in hud0["meters"][h["meter_first"]:h["meter_first"] + h["meter_count"]])), unit)
    weapon = next((h["tag"] for h in hud0["huds"] if h["kind"] == K_WEAPON and h["child"]), None)
    tags = [t for t in (cyborg, weapon) if t is not None]
    for ctype, label in ((0, "aim"), (3, "reload")):
        w, h, px = render_preview(hud0, view, tags, cross_types=(ctype,))
        p = outdir / f"{pack_path.name.split('.')[0]}_hud_{label}.png"
        write_png(p, w, h, px)
        written.append(p)
    return written


# ---- CLI ---------------------------------------------------------------------------------------------------------------
def info(path: Path) -> None:
    pk = hp2.parse(path.read_bytes())
    h = parse_hud0(pk.read("HUD0"))
    f = parse_font(pk.read("FONT"))
    s = parse_strs(pk.read("STRS"))
    v = texconv.parse_texs(pk.read("HUDT"))
    print(f"HUD0: {len(h['huds'])} HUDs, {len(h['elems'])} elements, {len(h['meters'])} meters, {len(h['numbers'])} numbers, "
          f"{len(h['crosses'])} crosshairs, {len(h['sprites'])} sprites, {len(h['sounds'])} sounds, {len(h['links'])} links")
    print(f"FONT: {len(f['fonts'])} fonts, {sum(len(x['glyphs']) for x in f['fonts'])} glyphs")
    print(f"STRS: {sum(len(x['strings']) for x in s['lists'])} strings, {sum(len(x['messages']) for x in s['sets'])} messages")
    print(f"HUDT: {len(v.textures)} textures, {sum(len(v.mip(t, 0)[2]) for t in v.textures) // 1024} KiB of level 0")


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("input", type=Path, help="decompressed .map (or, with --info / --png, a .hud.hp2)")
    ap.add_argument("-o", "--out", type=Path)
    ap.add_argument("--info", action="store_true", help="input is a .hud.hp2: print its section summary")
    ap.add_argument("--png", type=Path, metavar="DIR", help="input is a .hud.hp2: render textures, font specimen and "
                    "composed HUD previews as PNG files into DIR")
    args = ap.parse_args(argv)
    if args.info:
        info(args.input)
        return 0
    if args.png:
        for p in dump_png(args.input, args.png):
            print(p)
        return 0
    if not args.out:
        ap.error("-o is required")
    hs = collect(args.input)
    bad = verify(hs)
    pack = build_pack(hs)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_bytes(pack)
    print(report(hs))
    print(f"  pack {len(pack) / 1024:.0f} KiB")
    print(f"wrote {args.out}")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
