#!/usr/bin/env python3
"""Convert the Xbox main menu of ``ui.map`` into the PS2 pack ``ui.menu.hp2`` (docs/39-main-menu.md).

The menu of Halo is data: the scenario ``levels\\ui\\ui`` (a Halo ring in space, ten camera points and the script ``ring_loop`` that
tours them), 485 ``DeLa`` widget tags (``ui_widget_definition``) and their bitmaps, string lists and fonts. The widget runtime of the
executable is not ported; the screens (main menu, difficulty select, level select, a notice box) are laid out in ``engine/src/ui/menu.c``
from the positions this file's notes record, which were read off the widget tags (offsets inside a ``DeLa`` block):

    +0x00 u16 widget type (0 container, 1 text box, 2 spinner list, 3 column list, 4 game model), +0x02 u16 controller, +0x04 char[32] name,
    +0x24 s16 bounds top, left, bottom, right, +0x2C u32 flags, +0x38 background bitmap reference, +0x48 / +0x54 / +0x60 reflexives of game
    data inputs / event handlers / search and replace, +0xEC string list reference, +0xFC font reference, +0x3E0 reflexive of child widgets
    (80 B: widget reference at +0, char[32] name at +0x10, u32 flags +0x30, s16 controller, vertical offset, horizontal offset at +0x34).
    A widget sits at its own bounds (left, top) plus its child record's offsets: positions are absolute on the 640 x 480 screen.

Sections of the pack (all little endian): ``HUDT`` (a TEXS blob: every menu bitmap and the font atlases, read by world/tex.c like the HUD's),
``FONT`` and ``STRS`` (the HUD writers of hudconv: glyph tables, UTF-16 string lists) and ``MENU``::

    header 64 B: "MENU", u16 version (1), u16 0, u32 total, u32 sprite_count, u32 list_count, u32 font_count, u32 sprites offset, u32 lists
                 offset, u32 fonts offset
    sprite 28 B: u16 texture, u16 u0, v0, u1, v1 (texels, u1 / v1 exclusive), u16 w, h (source pixels: the size it is drawn at), u16 flags,
                 char[12] name (a check against ``menudef.h``'s enum order)
    list 8 B: u32 ustr tag id, u32 0;  font 8 B: u32 font tag id, u32 0.

Usage: ``menuconv.py assets/decompressed/ui.map -o assets/hp2/ui.menu.hp2``  (``--png DIR`` writes every sprite for a look).
"""
from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from halo2ps2 import bitmaps, hp2, hudconv, texconv  # noqa: E402

MENU_MAGIC, MENU_VERSION, MENU_HEADER, SPRITE_REC = b"MENU", 1, 64, 28

# Sprite slots, in the order of engine/src/ui/menudef.h: (name, bitmap tag path, image index, texture cap per side).
SH = "ui\\shell\\"
MM = SH + "main_menu\\"
SPRITES: list[tuple[str, str, int, int]] = [
    ("logo", MM + "halo_logo", 0, 512),
    ("campaign0", MM + "menu_load_campaign", 0, 256), ("campaign1", MM + "menu_load_campaign", 1, 256),
    ("multi0", MM + "menu_multiplayer", 0, 256), ("multi1", MM + "menu_multiplayer", 1, 256),
    ("settings0", MM + "menu_settings", 0, 256), ("settings1", MM + "menu_settings", 1, 256),
    ("demos0", MM + "menu_game_demos", 0, 256), ("demos1", MM + "menu_game_demos", 1, 256),
    ("gradient", SH + "bitmaps\\gradient", 0, 512),
    ("itemtop0", SH + "bitmaps\\list_item_bkd_top", 0, 256), ("itemtop1", SH + "bitmaps\\list_item_bkd_top", 1, 256),
    ("item0", SH + "bitmaps\\list_item_bkd", 0, 256), ("item1", SH + "bitmaps\\list_item_bkd", 1, 256),
    ("hdr_diff", MM + "difficulty_select\\header_choose_difficulty", 0, 512),
    ("emblem0", MM + "difficulty_select\\difficulty_options", 0, 256), ("emblem1", MM + "difficulty_select\\difficulty_options", 1, 256),
    ("emblem2", MM + "difficulty_select\\difficulty_options", 2, 256), ("emblem3", MM + "difficulty_select\\difficulty_options", 3, 256),
    ("listfield", SH + "bitmaps\\list_field", 0, 256),
    ("x_butn", "@x_butn", 0, 32), ("o_butn", "@o_butn", 0, 32),
    ("hdr_level", MM + "solo_level_select\\header_load_level", 0, 512),
    ("alert", SH + "bitmaps\\alert_bkd", 0, 256),
    ("white", SH + "bitmaps\\white", 0, 8),
    ("arrow_l", SH + "bitmaps\\arrow_big_left", 0, 32), ("arrow_r", SH + "bitmaps\\arrow_big_right", 0, 32),
    ("option0", SH + "bitmaps\\option_bkds", 0, 512), ("option1", SH + "bitmaps\\option_bkds", 1, 512),
    ("card0", SH + "bitmaps\\spinner_list_3_wide_item_background", 0, 256), ("card1", SH + "bitmaps\\spinner_list_3_wide_item_background", 1, 256),
] + [(f"ind{i}", MM + "difficulty_select\\difficulty_options_small", i, 64) for i in range(4)] + [(f"level{i}", SH + "bitmaps\\sp_levels", i, 256) for i in range(11)]

# String lists the screens read (ENUM order of menudef.h) and the fonts (small_ui, large_ui).
LISTS = [MM + "main_menu_options", MM + "button_key", MM + "difficulty_select\\difficulty_options",
         MM + "difficulty_select\\difficulty_descriptions", MM + "difficulty_select\\warning_message", MM + "map_list",
         MM + "solo_level_select\\map_data", SH + "main_menu\\settings_select\\player_setup\\player_profile_edit\\advanced_controls\\advanced_controller_labels",
         SH + "main_menu\\settings_select\\player_setup\\player_profile_edit\\advanced_controls\\var_invert_joystick",
         SH + "main_menu\\settings_select\\player_setup\\player_profile_edit\\advanced_controls\\var_look_sensitivity"]
FONTS = ["ui\\small_ui", "ui\\large_ui"]


def find(m: texconv.MapData, group: str, path: str):
    for e in m.index.entries:
        if e.group == group and e.path == path:
            return e
    raise SystemExit(f"menuconv: no {group} tag {path!r} in {m.path.name}")


def add_image(tex: hudconv.Textures, m: texconv.MapData, tid: int, idx: int, cap: int) -> hudconv.TexInfo:
    """One bitmap image as a PSMT8 texture of at most ``cap`` texels a side (the HUD writer caps at 256: the logo and the headers want 512)."""
    imgs = m.images(tid)
    img = imgs[idx]
    w, h, rgba = bitmaps.decode_level(img, m.pixels(img), 0)
    pw, ph = max(texconv.MIN_TEX, hudconv.pow2_ceil(w)), max(texconv.MIN_TEX, hudconv.pow2_ceil(h))
    tw, th = pw, ph
    while tw > cap or th > cap:
        tw, th = max(texconv.MIN_TEX, tw // 2), max(texconv.MIN_TEX, th // 2)
    padded = (pw, ph) != (w, h)
    if padded:
        rows = [rgba[y * w * 4:(y + 1) * w * 4] + bytes((pw - w) * 4) for y in range(h)]
        rgba = b"".join(rows) + bytes((ph - h) * pw * 4)
    found = texconv.alpha_class(rgba)
    klass = texconv.CLASS_BLEND if found != texconv.CLASS_OPAQUE else texconv.CLASS_OPAQUE
    rgba = texconv.resample_rgba(rgba, pw, ph, tw, th, klass != texconv.CLASS_OPAQUE)
    t = texconv.convert_rgba(tid, rgba, tw, th, klass)
    return hudconv.TexInfo(tex.add_raw(t), tw, th, w, h, tw / pw, th / ph, padded)


def ps2_button(kind: str, size: int = 32) -> bytes:
    """A PlayStation face button as RGBA (the menus show the DualShock's Cross and Circle where the Xbox shows A and B): a dark disc with a light rim and the coloured symbol, antialiased."""
    import math
    out = bytearray(size * size * 4)
    c = (size - 1) / 2.0
    rad = size / 2.0 - 1.0
    col = (120, 160, 235) if kind == "x" else (235, 80, 80)
    for y in range(size):
        for x in range(size):
            dx, dy = x - c, y - c
            d = math.hypot(dx, dy)
            cover = max(0.0, min(1.0, rad + 0.5 - d))
            if cover <= 0.0:
                continue
            r, g, b = 36, 38, 48                                      # the disc
            if d > rad - 1.6:
                r, g, b = 150, 154, 170                               # the rim
            elif dy < -c * 0.2:                                       # a faint gloss on the upper half
                r, g, b = 52, 55, 68
            sym = 0.0
            if kind == "x":                                           # two crossing strokes
                a1 = abs(dx - dy) / math.sqrt(2.0)
                a2 = abs(dx + dy) / math.sqrt(2.0)
                if abs(dx) < size * 0.27 and abs(dy) < size * 0.27:
                    sym = max(0.0, min(1.0, 2.0 - min(a1, a2) * 1.15))
            else:                                                     # a ring
                sym = max(0.0, min(1.0, 1.9 - abs(d - size * 0.25) * 1.15))
            r, g, b = (round(r + (col[0] - r) * sym), round(g + (col[1] - g) * sym), round(b + (col[2] - b) * sym))
            i = (y * size + x) * 4
            out[i:i + 4] = bytes((r, g, b, round(255 * cover)))
    return bytes(out)


def add_generated(tex: hudconv.Textures, tid: int, rgba: bytes, size: int) -> hudconv.TexInfo:
    found = texconv.alpha_class(rgba)
    klass = texconv.CLASS_BLEND if found != texconv.CLASS_OPAQUE else texconv.CLASS_OPAQUE
    t = texconv.convert_rgba(tid, rgba, size, size, klass)
    return hudconv.TexInfo(tex.add_raw(t), size, size, size, size, 1.0, 1.0, False)


def build(map_path: Path):
    m = texconv.MapData.open(map_path)
    c = hudconv.Collector(m)
    tex = c.tex
    sprites = bytearray()
    shown: list[tuple[str, int, int, int, int, int, int, int]] = []
    cache: dict[tuple[int, int, int], hudconv.TexInfo] = {}
    for name, path, idx, cap in SPRITES:
        if path.startswith("@"):
            tid = 0xFB000000 | len(cache)
            key = (tid, idx, cap)
            cache[key] = add_generated(tex, tid, ps2_button(path[1]), cap)
        else:
            tid = find(m, "bitm", path).tag_id
            key = (tid, idx, cap)
            if key not in cache:
                cache[key] = add_image(tex, m, tid, idx, cap)
        ti = cache[key]
        u1, v1 = min(ti.tw, round(ti.sw * ti.kx)), min(ti.th, round(ti.sh * ti.ky))
        flags = (hudconv.SF_PADDED if ti.padded else 0) | (hudconv.SF_SCALED if ti.kx != 1 or ti.ky != 1 else 0)
        sprites += struct.pack("<8H12s", ti.index, 0, 0, u1, v1, ti.sw, ti.sh, flags, name.encode()).ljust(SPRITE_REC, b"\0")
        shown.append((name, ti.index, 0, 0, u1, v1, ti.sw, ti.sh))
    fonts = []
    for p in FONTS:
        e = find(m, "font", p)
        f = c.font(e)
        fonts.append(f)
    for f in fonts:
        hudconv.pack_font_atlas(f, tex)
    lists = []
    for p in LISTS:
        e = find(m, "ustr", p)
        lists.append((e.tag_id, c.ustr(e)))
    hs = hudconv.HudSet("ui.menu", hudconv.Globals(), [], fonts, lists, [], [], [], tex)
    font_tab = b"".join(struct.pack("<II", f.tag_id, 0) for f in fonts)
    list_tab = b"".join(struct.pack("<II", t, 0) for t, _s in lists)
    off_s = MENU_HEADER
    off_l = off_s + len(sprites)
    off_f = off_l + len(list_tab)
    total = off_f + len(font_tab)
    head = struct.pack("<4sHHIIIIIII", MENU_MAGIC, MENU_VERSION, 0, total, len(SPRITES), len(lists), len(fonts), off_s, off_l, off_f)
    menu = bytes(head.ljust(MENU_HEADER, b"\0") + sprites + list_tab + font_tab)
    pack = hp2.build("ui.menu", [("MENU", menu), ("FONT", hudconv.build_font(hs)), ("STRS", hudconv.build_strs(hs)), ("HUDT", hudconv.build_hudt(hs))])
    return pack, hs, shown


def dump_png(pack: bytes, outdir: Path, shown) -> None:
    pk = hp2.parse(pack)
    view = texconv.parse_texs(pk.read("HUDT"))
    outdir.mkdir(parents=True, exist_ok=True)
    for name, ti, u0, v0, u1, v1, w, h in shown:
        tw, th, rgba = hudconv.texture_rgba(view, ti)
        rows = [rgba[(v0 + y) * tw * 4 + u0 * 4:(v0 + y) * tw * 4 + u1 * 4] for y in range(v1 - v0)]
        hudconv.write_png(outdir / f"{name}.png", u1 - u0, v1 - v0, b"".join(rows))


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("input", type=Path)
    ap.add_argument("-o", "--out", type=Path)
    ap.add_argument("--png", type=Path, help="write every sprite as a PNG into this directory")
    args = ap.parse_args(argv)
    pack, hs, shown = build(args.input)
    kb = sum(t.pixel_bytes for t in hs.textures.textures) // 1024
    print(f"ui.menu: {len(SPRITES)} sprites, {len(hs.fonts)} fonts ({sum(len(f.glyphs) for f in hs.fonts)} glyphs), {len(hs.lists)} string lists, "
          f"{len(hs.textures.textures)} textures, about {kb} KiB of pixels, pack {len(pack) / 1024:.0f} KiB")
    if args.out:
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_bytes(pack)
        print(f"wrote {args.out}")
    if args.png:
        dump_png(pack, args.png, shown)
        print(f"sprites in {args.png}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
