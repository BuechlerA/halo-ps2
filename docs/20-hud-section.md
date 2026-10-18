# `HUD0` (version 2), `FONT`, `STRS`, `HUDT` (version 1) sections (HUD layouts, fonts, strings, HUD textures)

Output of `tools/halo2ps2/hudconv.py` (workstream F of [Phase 3](14-phase3-plan.md)), read by `engine/src/ui/huddef.c`
(contract: `engine/src/ui/huddef.h`). The four sections live in one [`.hp2` pack](07-hp2-format.md),
`assets/hp2/<map>.hud.hp2`: `HUD0` (every HUD element, resolved to a 640 x 480 virtual screen), `FONT` (glyph tables of the
HUD fonts), `STRS` (string lists and message sets), `HUDT` (a [`TEXS`](09-tex-section.md) blob with every sprite sheet image and
font atlas page; `world/tex.c` reads it unchanged). Little-endian, tables 16-byte aligned, offsets relative to the section
start. Sprites and glyphs name HUDT textures by index; everything else refers to Xbox **tag ids** (u32), as the other packs do.

```
python3 -I tools/halo2ps2/hudconv.py assets/decompressed/a10.map -o assets/hp2/a10.hud.hp2
python3 -I tools/halo2ps2/hudconv.py --info assets/hp2/a10.hud.hp2
python3 -I tools/halo2ps2/hudconv.py --png /tmp/hudpng assets/hp2/a10.hud.hp2     # host check: PNG files, see "Checking by eye"
```

## What goes in
For one map: **every** `unit_hud_interface` (`unhi`), `weapon_hud_interface` (`wphi`) and `grenade_hud_interface` (`grhi`) tag
(a map only contains the HUDs of the units and weapons it uses), `hud_globals` (+ its `hud_number` digits), the two fonts
`hud_globals` names, the two `unicode_string_list`s it names (item and icon messages), the two `hud_message_text` tags (the global
one and the level's, `scenario +0x594`), the sprites of every bitmap those tags draw, and three kinds of **links**: weapon tag ->
weapon HUD (`weapon +0x480`), unit tag -> unit HUD(s), grenade type -> grenade HUD. Game data never enters the repository: the
section holds converted copies in `assets/` only, this document describes formats.

### Conventions
* **Virtual screen**: positions and sizes are pixels of 640 x 480, origin top left, y down; the engine scales to 512 x 448.
  Every drawable already carries its resolved top-left corner (`x`, `y`) and drawn size (`w`, `h`, sprite size x scale) and
  also the tag's raw anchor / offset / scales so the integrator can re-derive them.
* **Anchors** (`0 top left, 1 top right, 2 bottom left, 3 bottom right, 4 centre`): the anchor points are the corners of the
  **title-safe frame**, 7.5 % in from every screen edge (`48, 36` / `592, 36` / `48, 444` / `592, 444`), and the screen
  centre (`320, 240`). An element's matching corner (its centre for 4) is placed at the anchor point plus the tag's offset,
  x right and y down at every anchor: `x = anchor_x + off_x - (w if right, w/2 if centre)`, likewise `y`. Crosshair overlays
  are always centre anchored. The **motion sensor** (background, foreground, centre) is laid out from the bottom-left anchor
  whatever the unit HUD's anchor says, and its centre offset is counted *inward* from that corner (`x = 48 + off_x`,
  `y = 444 - off_y`). Verified against an Xbox frame (ADR-060, "Source facts" below). `verify` checks that every element,
  meter and number of all ten maps is drawn entirely on screen under this rule.
* **Colours** are RGBA words on the GS scale, R in the low byte (`0x80808080` = opaque white): they go straight into vertex
  colours of a modulated texture. Tag colours are `ColorARGBInt` (bytes B, G, R, A).
* **Time** is in ticks of 1/30 s; floats in the records are `f32`.
* **Blend**: every HUD bitmap is a (luminance +) alpha mask (`ay8`, `a8y8`) tinted by the element colour; the converter stores
  `ay8` as a white alpha ramp (premultiplied-over equals straight alpha blend with white), so *all elements use alpha blend*
  (`blend = 2`, `TEX_BLEND_ALPHA`). Additive blends only occur in the multitexture overlays, which are not converted.
* **Sprites** (24 B) are what elements draw: a texture index, the texel rectangle, the size in virtual pixels and the registration
  point. A bitmap *sequence* becomes one sprite per sprite of a sprite sheet, or one sprite per whole image of an interface
  bitmap; an element's `frames` consecutive sprites starting at `sprite` are that sequence (an animation or, for a few
  reticle warnings, a composite of parts: **unverified which**; `frame_rate` is carried raw).

## Source facts (Xbox, engine version 5; all ten campaign maps)
Layouts are in the docstring of `hudconv.py` with the offsets. "verified" means the data confirmed it (the cross-check is named).

| Fact | Status |
|---|---|
| `bitmap` sequences (+0x54, 64 B) and sprites (32 B: image, left/right/top/bottom as 0..1 of the image, registration x / y as 0..1 of the image) | verified: every sequence index of every HUD element of ten maps (214 HUD tags counted per map) is in range; sprite rectangles lie inside 0..1; rendering the sprite sheets with these rectangles gives intact digits, meter segments and reticles |
| `ColorARGBInt` byte order B, G, R, A | verified: `hud_globals` stores the same colour as float ARGB and as ARGBInt |
| Common element header (offset s16 x2, scales f32 x2, scaling flags u16, 0x24 B) + static body (bitmap +0x24, default / flash colour, period, delay, count, flags, length, disabled colour, sequence +0x54): 104 B | verified: the stride of every unit HUD element is the distance to the next bitmap reference; weapon statics are 36 B state header + 104 B + 40 B pad = 180 |
| Meter body (colours min, max, flash, empty at +0x34; flags, min value, sequence at +0x44; alpha multiplier / bias, value scale; opacity, translucency, disabled colour) | verified for positions (sequence in range, opacity and translucency in 0..1 on all 196 meters (summed over ten maps), colours min / max non-zero on all that draw); the *meaning* of flags, min value, alpha multiplier / bias and value scale is from the public names: retail values flags 0 / 1, min value 0 / 31, value scale 0 / 198 / 255 |
| Meter and crosshair-overlay colours carry alpha 0 in every retail tag (the aim reticle and shield bar are drawn by default) | verified (every meter colour except the 46 weapon-meter disabled colours; 198 of the 302 crosshair overlays, summed over ten maps: the others, the zoom reticles, carry real alphas such as 0x3C); converter rule: alpha 0 with non-zero RGB becomes opaque, an all-zero colour stays 0 ("not set") |
| Static element colour 0 (weapon statics of rifle-like HUDs) | seen: element is invisible by default and its flash colour is orange-red with alpha 1: it is a low-ammo warning drawn only while flashing (**interpretation unverified**) |
| Unit HUD fixed block is 0x56C B: seven elements from 0x24, sounds reflexive at +0x3C0 (56 B), auxiliary overlays at +0x3CC (324 B) | verified by the reflexive strides of nine a10 unit HUDs |
| Auxiliary overlay: u16 type, 20 B header, static 104 B, meter 104 B, f32 minimum fraction cutoff, u32 flags | verified (cyborg: type 0, cutoff 0.25, flags 1; its bitmaps are the unit backgrounds / meters) |
| Weapon HUD: reflexives at +0x60 statics (180 B), +0x6C meters (180), +0x78 numbers (160), +0x84 crosshairs (104), +0x90 overlays (104 + 136 B sub overlays), u32 crosshair-type bit set at +0x9C | verified (strides, bit set equals the crosshair list on every weapon HUD) |
| Crosshair overlay (108 B): **sequence index at +0x46**, flags u32 at +0x48 | verified: +0x46 is in range for the crosshair bitmap on all 302 crosshair overlays (summed over ten maps) and differs per weapon (+0x44 is 0 except one vehicle warning); flag 4 appears exactly on the zoom reticles |
| Weapon sub overlay (136 B): sequence u16 at +0x48, type u16 at +0x4A (16 on every weapon HUD), flags u32 at +0x4C; grenade overlays (136 B at grhi +0x15C) use the same layout (type 1 / 4) | verified for the sequences (in range); type and flags are raw |
| Weapon pickup icon: u16 sequence, s16 width offset, s16 x, y at +0x13C of a weapon HUD (sequence of the `hud_globals` icon bitmap) | verified on the nine real weapon HUDs (sequences 4..12 distinct, width offset 2); child HUDs and one vehicle turret hold a text fragment there (value 25193): the converter drops icon data whose sequence is past the bitmap's list; vehicle gun HUDs hold (0, 0, 0, 0) (no pickup: never drawn, but `icon_seq` reads 0 there, so use the icon only for weapons the player can pick up) |
| `hud_number`: digit widths at +0x10, 15 sprites (0..9 digits, 10 `.`, 11 `:`, 12 `-`, 13 `m`, 14 a wide blank) | verified by rendering; the order of 10..14 by looking at the sheet |
| Number element body (colours, flash fields at +0x24.., max digits / flags / fraction digits at +0x44) with width / height scale **0 = unset** on weapon HUDs | verified (scales 0 on all weapon numbers, 1.0 on grenade numbers); digits 1..4, flags 1 or 3, fraction digits 0 or 3 |
| `font` characters (20 B), glyph pixels as 8-bit coverage in one data block, running offsets | verified: pixel sizes sum to the data size on both fonts of ten maps; glyph placement: quad left = pen + origin x, top = baseline - origin y (advance = origin x + width within a pixel for 90 % of the glyphs) |
| `unicode_string_list` (20 B entries, UTF-16LE, NUL included in the length) and `hud_message_text` (2 B elements: type 0 text of `value` units / type 1 icon `value`; 64 B messages) | verified: text units of a message add up to the next message's start on every message of ten maps |
| unit tag -> unit HUD references | found by scanning the first 4 KiB of every `bipd` / `vehi` tag for `unhi` references (offsets vary: biped 0x8F0 / 0x920 on the Master Chief, vehicle seats elsewhere); **order** = order of appearance, which of them is single / multiplayer or which seat is unverified |
| Anchor values 0 top left, 4 centre | verified (passenger HUDs stack from the top left, the Warthog driver reticle is centre anchored 40 px up, crosshairs are centred) |
| Anchor points on the 7.5 % title-safe frame, the matching corner at the anchor, offsets x right / y down | verified against a gameplay frame of the Xbox build (xemu, 640 x 480, 2 px accuracy): the top-left weapon panel art of `master plasma` at offset (12, 1) is at x 61, y 36; its heat meter at (4, 21) at x 52..157, y 57..73; the grenade panel (82, 1) at x 131; the top-right `cyborg` shield panel (0, 1) with its 128 px image at x 464..592, y 37; the motion sensor circle centre at (89, 403) with radius 41. All 24 HUD tags of the ten maps use anchors 0 (20 weapon + 2 grenade + 6 unit HUDs), 1 (`cyborg`, `cyborg_mp`) or 4 (`warthog driver`): the bottom anchors are never used by retail data, their rule is the mirror of the top ones |
| Motion sensor always bottom left, centre offset inward from the corner | verified: both unit HUDs with a sensor are top-right anchored, yet the Xbox frame draws the sensor bottom left; the sensor images (`hud_weapon_backgrounds` 3 / 4, 128 and 64 px) carry the radar circle in the image's bottom-left corner, and the tag's centre offset (41, 41) / (31, 31) is the circle centre's distance from that corner (radius = the same number) |
| Motion sensor blip: `ui\hud\bitmaps\hud_sensor_blip` (32 x 32 dxt3 disc), sequence 0 | verified present in all ten maps; no HUD tag references it (the engine finds it by path, like the converter); `hud_sensor_blip_custom` (dxt1) is not used. The Xbox frame shows blips about 6 px wide, friendly pale yellow, hostile red; still enemies are hidden there (velocity sensitivity), which the port does not model |
| Meter drawing (clip the sprite to the value, colour between `colour_min` and `colour_max`, empty colour behind) and flash / disabled states | **unverified**, from the public tag names; the data only gives the fields |

Not converted, with the reason: multitexture overlays of static elements (2 of them, both on the sniper HUD: scope scroll /
noise layers) and the weapon HUD `screen effects` (pistol, sniper, rocket launcher: zoom masks); waypoint arrows and the
`postgame_carnage_report` bitmap (multiplayer / end screens); `metr` meter tags and `smet` shaders (3D model meters); the
remaining `hud_globals` fields (help-text colours, damage indicator geometry beyond the sprite and colour); button icon
semantics (the Xbox controller glyph sprites are exported, the PS2 port draws its own).

## `HUD0` layout
Header, 128 bytes: `char[4] "HUD0"`, `u16 version (2)`, `u16 reserved`, `u32 total size` (= section size), then counts at
`0x0C` huds, `0x10` elements, `0x14` meters, `0x18` numbers, `0x1C` crosshairs, `0x20` sounds, `0x24` sprites, `0x28` icons,
`0x2C` links; `0x30` zero; `0x34` globals offset; table offsets from `0x38`: huds, elements, meters, numbers, crosshairs,
sounds, sprites, icons, links. Each table sits after the previous one, 16-aligned.

**Globals, 192 B** (one record): `u8 anchor`, `s16 off_x, off_y` (+2, +4; the retail value (0, 60) is not used by the HUD),
`u16 scaling` (+6), `f32 width scale, height scale` (+8, +0xC; 0 on retail), `u32 font_sp, font_mp` (+0x10, +0x14: font tag
ids), `f32 item message scale x, y` (+0x18), `u32 text colour a, b` (+0x20: GS colours), `u32 default weapon HUD` (+0x28), item
strings / icon strings (ustr tag ids, +0x2C / +0x30), messages / level messages (hmt tag ids, +0x34 / +0x38, 0 = none),
`icon bitmap`, `hud_number tag` (+0x3C / +0x40), `f32 motion range, velocity sensitivity, scale` (+0x44; 20, 0.0002, 32 on
retail, **names unverified**), `u16 damage sprite, multiplayer damage sprite` (+0x50: arrows in the damage bitmap),
`u32 damage colour` (+0x54), `u16 digit_first, digit_count` (+0x58: the 15 digit sprites), `s8 x 6` (+0x5C: bitmap digit width,
screen digit width, x offset, y offset, decimal point width, colon width), `u16 icon count` (+0x62), `u16 blip sprite` (+0x64:
the motion sensor blip, 0xFFFF when the map has no `hud_sensor_blip` bitmap; version 2).

**HUD, 64 B**: `u32 tag_id` (0), `u8 kind` (4: 0 unit, 1 weapon, 2 grenade), `u8 anchor` (5), `u16 flags` (6: weapon HUD
flags raw, bit 0 = use the parent's flashing parameters), `u32 child_tag` (8: weapon HUD whose elements and crosshairs are
also in effect), `u16 total / loaded / heat / age ammo cutoff` (0x0C..0x12: below these values the matching state flashes;
meaning from the public names), then `u16 first, count` pairs at 0x14 elements (**includes the crosshair overlay elements, which
are reached through the crosshair list**), 0x18 meters, 0x1C numbers, 0x20 crosshairs, 0x24 sounds, `u32 crosshair mask`
(0x28: bit n = a crosshair of type n), `u16 icon_seq` (0x2C, 0xFFFF none), `s16 icon width offset, x, y` (0x2E..0x32),
`u16 icon_sprite` (0x34).

**Element, 64 B**: `u8 role` (0), `u8 state` (1: weapon "state attached to" 0 total ammo, 1 loaded ammo, 2 heat, 3 age,
4/5 secondary total/loaded, 6 distance to target, 7 elevation to target; 0xFF none), `u8 allowed` (2: 0 any map, 1 solo, 2
multiplayer only: **the campaign skips 2**), `u8 frames` (3), `u16 sprite` (4, 0xFFFF none), `u16 reserved` (6), `u32 flags`
(8: weapon sub overlay flags / crosshair overlay flags), `s16 x, y, w, h` (0x0C), `s16 off_x, off_y` (0x14),
`f32 scale x, y` (0x18; 1.0 when the tag has 0), `u16 scaling flags` (0x20), `u8 anchor` (0x22), `u8 blend` (0x23),
`u32 colour, flash colour, disabled colour` (0x24, 0x28, 0x2C), `u16 flash period, delay, count, flags, length`
(0x30..0x38: ticks; flags bit 0 only on 10 elements), `u16 frame rate` (0x3A), `u16 seq` (0x3C: sequence index in the source
bitmap), `u16 type` (0x3E: weapon sub overlay type / crosshair type).

| role | where | meaning |
|---|---|---|
| 1..7 | unit HUD (always 1..6 in this order, then one 7 per auxiliary overlay) | hud background, shield panel background, health panel background, motion sensor background, foreground (both anchor 2, bottom left), motion sensor centre (no sprite: `x, y` = the radar centre on screen, `off_x, off_y` = its distance from the sensor corner = the radar radius in pixels), auxiliary overlay background |
| 8, 9 | weapon HUD | static element (state attached, solo / multiplayer), overlay sub element (the parent overlay's state and bitmap) |
| 10 | weapon HUD | crosshair overlay (in the HUD's element range after the others; use the crosshair list) |
| 11..13 | grenade HUD | grenade background, total grenades background, overlay (type 1 / 4) |

**Meter, 80 B**: `u8 role` (1 shield, 2 health, 3 auxiliary, 4 weapon), `u8 state` (weapon: as above; auxiliary: aux type,
0 = integrated light), `u8 allowed`, `u8 flags` (meter flags raw), `u16 sprite`, `u8 min value`, `u8 anchor`, `s16 x, y, w, h`,
`s16 off_x, off_y`, `f32 scale x, y` (0x14), `u16 scaling` (0x1C), `u8 alpha multiplier, bias` (0x1E, 0x1F),
`u32 colour_min, colour_max, colour_flash, colour_empty, colour_disabled` (0x20..0x30), `u16 value scale` (0x34),
`u16 seq` (0x36), `f32 opacity, translucency` (0x38, 0x3C; 0..1), `f32 min fraction cutoff` (0x40) and `u32 flags`
(0x44) of auxiliary meters. Every HUD kind with meters has its shield and health meters even when they draw nothing (sprite
0xFFFF).

**Number, 64 B**: `u8 role` (1 weapon, 2 grenade), `state`, `allowed`, `u8 max digits` (3), `u8 flags` (4: 1 leading zeros,
2 only when zoomed, 4 trailing `m`), `u8 fraction digits`, `u8 anchor`, `s16 x, y, w, h` (w = digits x screen digit width
+ the decimal point when there are fractions, scaled; h = digit sprite height), `s16 off_x, off_y`, `f32 scale x, y`,
`u16 scaling`, colours at 0x20 / 0x24 / 0x28, flash `u16 period, delay, count, flags, length` at 0x2C.
Digit sprites come from `globals.digit_first` (0..9, `.`, `:`, `-`, `m`).

**Crosshair, 16 B**: `u8 type` (0 aim, 1 zoom, 2 charge, 3 should reload, 4 flash heat, 5 flash total ammo, 6 flash battery,
7 reload overheat, 8 firing with no ammo, 9 throwing with no grenade, 10 low ammo none left, 11..17 secondary / ready
variants, 18 firing with a depleted battery; HEK order, names unverified except 0 and 1), `u16 elem_first, elem_count` (2, 4:
overlay elements, role 10), `u32 bitmap tag` (8). Crosshair overlay `flags`: 1 flashes when active, 2 not a sprite, 4 only when
zoomed (verified), 8.. other HEK names, unverified. A weapon HUD has the crosshairs listed in its `crosshair mask`; its **child
HUD** supplies the rest (the shared warning reticles): `huddef_find_crosshair` walks the chain.

**Sound, 32 B**: `u32 sound tag`, `char[4] group` (`snd!` or `lsnd`), `u32 latched to` (bit set: 1 shield recharging,
2 shield damaged, 4 shield low, 8 shield empty, 16 health low, 32 health empty, 64 minor damage, 128 major damage: verified for
the cyborg HUD by the sound names), `f32 scale`, `u32 hud tag`. The sounds belong to workstream G's world.

**Icon, 32 B** (hud_globals button icons): `u16 sprite, seq`, `s16 width offset, x, y`, `u32 colour` (0x0C), `u8 b12, b13`
(0x10: raw), `u16 index` (0x12). **Link, 16 B**: `u32 owner, kind` (0 weapon tag, 1 unit tag, 2 grenade type index),
`u32 hud tag`, `u32 order`.

## `FONT` layout
Header, 64 B: `"FONT"`, `u16 version`, `u32 total` (+8), `u32 fonts` (+0x0C), `u32 glyphs` (+0x10), `u32 fonts offset` (+0x14),
`u32 glyphs offset` (+0x18). **Font, 48 B**: `u32 tag_id`, `u32 flags`, `s16 ascent, descent, leading height, leading
width` (+8: 17, 4, 4, 0 for the large font), `u32 glyph_first, glyph_count` (+0x10), `u16 tex_first, tex_count` (+0x18: the
atlas pages in HUDT). **Glyph, 24 B**, sorted by code point per font (binary search): `u16 code`, `s16 advance`, `u16 w, h`
(the quad), `s16 ox, oy` (quad left = pen x + ox, quad top = baseline y - oy), `u16 tex` (0xFFFF for blanks), `u16 u0, v0, u1,
v1`. 224 glyphs per font (code points 0x20..0x2122, Latin and punctuation, no kerning table in the tag).
The atlas: shelf packed, 256 wide, 1 px gutter, **PSMT8 with a white alpha ramp CLUT** (index = coverage, entry i = (255, 255,
255, i)): the 8-bit coverage of the tag is stored exactly. The large font fits one 256 x 256 page, the small one 256 x 128.

## `STRS` layout
Header, 64 B: `"STRS"`, `u16 version`, `u32 total` (+8), counts at +0x0C lists, +0x10 message sets, +0x14 messages, +0x18
segments, +0x1C strings, +0x20 character units; offsets at +0x24 lists, +0x28 sets, +0x2C messages, +0x30 segments, +0x34
string table, +0x38 characters (UTF-16LE, every string followed by a NUL unit). List (32 B): `u32 tag, count, first string`. String
(8 B): `u32 offset, length` in units. Set (32 B): `u32 tag, count, first message`. Message (48 B): `char[32] name`, `u16 first
segment, segment count`. Segment (8 B): `u8 kind` (0 text, 1 icon), `u8 icon value`, `u16 text length`, `u32 text offset` in units. A
message is a sequence of text and icon segments ("Press <icon 18> to ..."): icon values are the tag's element values (button /
placeholder ids: 18 and 32 on the pick-up message, i.e. the action button and the item name; **meaning unverified**). The `%d`
style placeholders of the item strings are kept as they are. The reader copies strings into caller buffers (`huddef_get_string`,
`huddef_get_segment`).

## `HUDT`
A `TEXS` blob: one texture per (bitmap tag, image) a sprite uses plus the font atlas pages, no bindings. Images are kept at full
size when possible: sizes are padded to a power of two (>= 8 on each side; a 100 x 20 image becomes 128 x 32, `SF_PADDED`),
and an image over 256 is halved (`SF_SCALED`; the sniper scope mask and two scope tick strips). `ay8` / `a8` images become exact
white alpha ramps (PSMT8), the others go through `texconv.convert_rgba` (alpha class chosen from the pixels; PSMT4 is used where
16 colours are enough). One mip level only (HUD is drawn at 1:1). Sprite rectangles are texel coordinates of the stored
texture (`u1`, `v1` exclusive); the sprite's `w`, `h` are the drawn size at scale 1 in virtual pixels.

## Measured sizes (`python3 -I tools/halo2ps2/hudconv.py <map> -o ...`)
| map | HUDs (unit/weapon/grenade) | elements | meters | numbers | crosshairs | sprites | HUD0 | FONT | STRS | HUDT textures (PSMT4) | HUDT KiB | pack KiB | seconds |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---|---:|---:|---:|
| a10 | 21 (9/10/2) | 95 | 24 | 6 | 19 | 97 | 13872 | 10912 | 10880 | 42 (11) | 470 | 108 | 0.3 |
| a30 | 21 (6/13/2) | 97 | 20 | 8 | 27 | 118 | 14464 | 10912 | 8336 | 58 (15) | 550 | 116 | 0.3 |
| a50 | 16 (2/12/2) | 71 | 12 | 8 | 25 | 106 | 11440 | 10912 | 6608 | 54 (15) | 530 | 112 | 0.3 |
| b30 | 23 (7/14/2) | 91 | 22 | 6 | 25 | 108 | 14000 | 10912 | 7312 | 49 (11) | 501 | 112 | 0.2 |
| b40 | 29 (10/17/2) | 129 | 29 | 8 | 34 | 125 | 18112 | 10912 | 6400 | 63 (15) | 573 | 120 | 0.3 |
| c10 | 16 (2/12/2) | 59 | 13 | 6 | 22 | 97 | 10352 | 10912 | 6256 | 44 (11) | 474 | 108 | 0.2 |
| c20 | 17 (2/13/2) | 58 | 13 | 6 | 21 | 97 | 10336 | 10912 | 6352 | 43 (11) | 469 | 108 | 0.2 |
| c40 | 26 (7/17/2) | 111 | 24 | 8 | 33 | 128 | 16416 | 10912 | 7328 | 64 (15) | 576 | 122 | 0.3 |
| d20 | 23 (4/17/2) | 91 | 18 | 8 | 31 | 124 | 14272 | 10912 | 7024 | 61 (15) | 561 | 120 | 0.3 |
| d40 | 22 (6/14/2) | 86 | 21 | 6 | 25 | 110 | 13584 | 10912 | 7008 | 49 (11) | 499 | 114 | 0.2 |

Raw section sizes are in bytes. The largest map (c40) needs 0.56 MiB of HUDT pixels + CLUTs, 17 KiB of tables and 11 KiB of
glyph tables: about 0.6 MiB resident, ~10 % of the 6 MiB texture pool, and 122 KiB in the pack after LZ4. The a10 HUDT is 436 KiB of
pixels + 32 KiB of CLUTs in 42 textures (11 PSMT4).

## Using it (integrator notes)
* **Which HUD**: `huddef_find_link` gives the unit HUD of the player's biped (order 0) or of the vehicle seat, the weapon HUD
  of the held weapon, the grenade HUD of the selected grenade type; `globals.default_weapon_hud` is the HUD to use when the unit holds nothing (on retail one with only
  crosshairs). Draw the unit HUD, then the weapon HUD **and its child chain** (`child_tag`), then the grenade HUD.
* **Weapon HUD elements** are selected by `state` and the weapon's numbers (ammo cutoffs flash the matching state), skip
  `allowed == 2` (multiplayer). Statics and overlays are alpha-blended sprites with the element's colour; the flash fields
  (period / delay / count / length in ticks, colours) describe the low-ammo / low-health flashing. Meters clip their sprite to
  the value (fraction of the magazine, heat, shield, health); retail meters have alpha 0 colours converted to opaque.
* **Crosshairs**: pick the crosshair type by weapon state (`HUDDEF_X_*`), `huddef_find_crosshair(weapon_hud_tag, type)`;
  draw its elements at the screen centre (positions are already centre resolved); skip `HUDDEF_CF_ONLY_ZOOMED` elements when not
  zoomed. The pickup icon of a weapon is `hud.icon_sprite` (0xFFFF = none).
* **Text**: `huddef_find_glyph(fonts, font, code, &g)`, then a quad at `(pen_x + g.ox, baseline - g.oy)` of size `g.w x g.h`
  with texel rect `g.u0..g.u1 x g.v0..g.v1` of HUDT texture `g.tex`, pen += `g.advance`; line height = `ascent + descent +
  leading_h`. `huddef_text_width` measures. Numbers use `globals.digit_first + digit` sprites with `screen_digit_width`
  advance. Strings: `huddef_get_string` (item / icon lists), `huddef_find_message` + `huddef_get_segment` (hud messages).
* **Textures**: upload HUDT texture `s.tex` (PSMT8 / PSMT4 + CLUT) with the CLUT swap documented in
  [09](09-tex-section.md); the fonts and meters need alpha blending with `TFX = MODULATE`. `huddef_check_textures` validates
  the sprite rectangles against the HUDT table at load.

## Checking by eye
`hudconv.py --png DIR pack.hud.hp2` writes: every HUDT texture on a checkerboard (`<map>_texNN_WxH.png`, 2x), `<map>_font.png`
(a font specimen of the first font with the messages of the pack), and `<map>_hud_aim.png` / `<map>_hud_reload.png` (the first
unit HUD with a shield meter and the first weapon HUD with a child, composed at 640 x 480 with default colours, meters full and
the aim / should-reload crosshair). On a10 the specimen reads cleanly, the composite shows the shield and health panels with the
radar in the top right corner, the ammo panel in the top left and the circular rifle reticle in the centre, and a contact sheet of
the aim reticles of all weapon HUDs of b40 shows the recognisable shapes (rifle ring, pistol dot, plasma wedges, needler cross, shotgun
ring, sniper / rocket scope ticks). The PNGs are made with `zlib` only.

## Reader and tests
`engine/src/ui/huddef.h` (`huddef_open`, `huddef_check_textures`, `huddef_get_{globals,sprite,hud,elem,meter,number,crosshair,
sound,icon,link}`, `huddef_find_{hud,crosshair,link}`, `huddef_fonts_open`, `huddef_find_font`, `huddef_find_glyph`,
`huddef_text_width`, `huddef_strings_open`, `huddef_find_{list,set,message}`, `huddef_get_string`, `huddef_get_segment`). The
open functions validate every table (counts, 16-alignment, ranges with 64-bit products, enum values, finite floats, sprite and
element ranges, glyph order and rectangles, string bounds and NULs, name terminators); accessors on a failed open return neutral
records. `engine/tests/test_huddef.c` opens the pack written by `tools/tests/test_hudconv.py` (a synthetic map with every tag
kind, hand-computed positions, scales, colours, sprite UVs of padded and halved images, a child-chain crosshair, links, fonts,
strings), then truncates each section at every length, corrupts every table, 90 targeted fields and then every byte
(ASan/UBSan). `test_huddef <pack> smoke` opens and walks any real pack (all ten maps pass). `test_hudconv.py` also converts
and verifies every campaign map found in `assets/decompressed` (ranges, sequences, meter colours, crosshair masks, fonts), and
checks the reference parsers, the corrupt-tag cases and the PNG writer.

## Motion tracker (engine, `ui/hud.c`)
`hud_build` draws one blip per living biped or vehicle object (not the player's body) within `motion range` world units of the
player (20 on retail), at `centre + (right, -forward) / range * radius` with the player's facing up, where `radius` is the
motion sensor centre's corner distance (41 px on the cyborg HUD; `motion scale` 32 is exported but not used: unverified
whether it is this radius). The blip is the `hud_sensor_blip` sprite drawn 7 px wide (10 for vehicles), tinted pale yellow for
teams 1 / 2 (player, human), red for 3 / 4 / 5 (covenant, flood, sentinel), grey otherwise; the team is the unit tag's default
team (`game_unit_t.team`, set when the object is created) or the actor's team once `units_attach` took it over. Not modelled:
the Xbox hides enemies that stand still (`motion velocity sensitivity`), shows the player's own vehicle and lets blips fade.

## Open points
* The meaning of meter flags / min value / alpha multiplier / bias / value scale, and of the overlay `type` / `flags` words need
  a comparison with the Xbox picture or more of the public tag documentation; the bottom anchors (2, 3) are unused by retail
  data and mirror the verified top ones.
* Whether the 5-sprite reticle warning sequence is an animation or a composite.
* The sniper scope (multitexture overlays, screen effects) is not converted: the first pass draws its crosshair overlays only.
* Motion tracker: the velocity rule and whether `motion scale` is the radar radius.
