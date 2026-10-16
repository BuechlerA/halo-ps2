# Halo: Combat Evolved (Xbox) Data — What We Convert

All facts here describe file *formats* so the converter can be written. The
repository never contains the data itself (see spec R7).

## 1. Disc layout (NTSC retail, from the user's ISO listing)
```
default.xbe            Xbox executable — NOT used by this project
maps/*.map             cache files: 10 campaign, 13 multiplayer, ui.map
bink/*.bik             intro, attract loops, credits (Bink video)
xdemos/                Halo 2 trailers — ignored
```
Campaign maps: `a10 a30 a50 b30 b40 c10 c20 c40 d20 d40` (≈ 1.49 GiB
compressed). Extract with `scripts/extract-iso.sh` (uses `extract-xiso`) into
`assets/xbox/` (gitignored).

## 2. Xbox cache file (`.map`) format

### 2.1 Header (2048 bytes, little-endian)
| Offset | Type | Field |
|---|---|---|
| 0x000 | u32 | `head` magic (`0x68656164`, bytes `d a e h` on disk) |
| 0x004 | u32 | engine version: **5 = Xbox**, 6 = trial, 7 = PC retail, 609 = Custom Edition |
| 0x008 | u32 | **decompressed** file size |
| 0x00C | u32 | padding length |
| 0x010 | u32 | tag data offset (in the decompressed file) |
| 0x014 | u32 | tag data size |
| 0x018 | u8[8] | unused |
| 0x020 | char[32] | map name |
| 0x040 | char[32] | build string |
| 0x060 | u16 | map type: 0 singleplayer, 1 multiplayer, 2 UI |
| 0x062 | u16 | padding |
| 0x064 | u32 | CRC32 |
| 0x068 | u8[1940] | padding |
| 0x7FC | u32 | `foot` magic |

### 2.2 Compression
Xbox maps store the 2048-byte header uncompressed and the **rest of the file as a
single zlib (deflate with zlib wrapper) stream**. Decompressing it and
concatenating after the header yields a file identical in layout to the
uncompressed cache the game sees in memory. `tools/halo2ps2/mapinfo.py` does this.

### 2.3 Memory model the data assumes
- Tag data is loaded to a fixed virtual base: **`0x803A6000` on Xbox**
  (`0x40440000` on PC). Every pointer inside the tag data is an absolute address
  relative to that base; the converter subtracts the base to get file offsets.
- Xbox tag space limit: **0x1700000 (23 MiB)**. Everything the game keeps
  resident for a map fits in that, except:
- **Model vertex and index buffers live inside the tag region on Xbox**: the
  index header stores them as `0x80xxxxxx` pointers right after the path strings
  (3.9–4.9 MiB of vertices per campaign map, see
  [reports/tag-inventory.md](reports/tag-inventory.md)). PC stores a file offset
  and size there instead. Bitmap pixels and sound samples are raw data outside
  tag space, referenced by file offset and streamed by the Xbox engine (sounds on
  demand, BSPs on switch).
- Structure BSPs are at the end of the tag region and only one is loaded at a
  time into a separate region; `scenario` holds the list and the `switch_bsp`
  script command selects.

### 2.4 Tag index
At `tag data offset`: index header (tag array address, scenario tag id, tag
count, model vertex/index counts and offsets — Xbox stores model vertex data
pointers here, *not* in the tags), then `tag count` × 32-byte entries
(group ids as four-char codes, tag id, path address, data address, indexed
flag). Paths are null-terminated strings in the tag region.

### 2.5 Xbox-specific differences vs. PC (the converter must handle)
| Area | Xbox | PC/CE |
|---|---|---|
| Models | `model` tag (`mode`) with **compressed vertices** (s16 pos/normal/uv packed), vertex buffers referenced through the index header | `gbxmodel` (`mod2`), uncompressed vertices |
| Bitmaps | Pixel data inside the map, **swizzled** for the NV2A, DXT1/3/5 or A8R8G8B8/R5G6B5/A4R4G4B4/P8; cube maps and sprites | Separate `bitmaps.map`, unswizzled |
| Sounds | **Xbox ADPCM** (IMA-like 4-bit, 64-byte blocks) inside the map | `sounds.map`, Ogg Vorbis or PCM |
| Animations | Compressed keyframe format (same across versions) | Same |
| Strings | `unicode_string_list` (UTF-16LE) | Same |
| Scripts | Compiled node tree in `scenario` (same opcodes) | Same |

Tag *struct layouts* for engine version 5 are otherwise the same as version 7
in nearly every group; the community definitions (Invader's JSON, Reclaimer's
Python structs — Reclaimer explicitly supports "Halo 1 Xbox") document the few
exceptions.

## 3. Tag groups and where they go on PS2

| Group (code) | Count/map (typ.) | PS2 section | Converter output |
|---|---|---|---|
| `scenario` (scnr) | 1 | RES | placements, encounters, scripts, cutscene flags, BSP list, trigger volumes |
| `structure_bsp` (sbsp) | 1–8 | BSPn | render clusters (VIF packets), lightmap atlases, collision BSP, portals, pathfinding, markers, weather/sound palettes |
| `globals` (matg) | 1 | RES | player constants, material responses, grenade/vehicle tables |
| `bitmap` (bitm) | 400–800 | TEX | PSMT8/4 mips + CLUT, sprite sheets, cube maps → sphere maps |
| `model` (mode) | 100–300 | RES | VU1 strips per region/permutation/LOD, markers, nodes |
| `model_animations` (antr) | 50–150 | RES | quantised tracks, graph/state tables |
| `collision_model` (coll) | 100–200 | RES | BVH of pathfinding-compatible collision tris, regions |
| `physics` (phys) | 10–30 | RES | mass points, powered masses |
| `biped`/`vehicle`/`weapon`/`equipment`/`projectile`/`scenery`/`device_*`/`placeholder`/`sound_scenery` (bipd vehi weap eqip proj scen mach ctrl lifi plac ssce) | 200–400 | RES | object definitions (verbatim fields, re-laid out) |
| `actor`/`actor_variant` (actr actv) | 30–80 | RES | AI tuning |
| `damage_effect` (jpt!), `effect` (effe), `particle` (part), `particle_system` (pctl), `decal` (deca), `contrail` (cont), `light` (ligh), `lens_flare` (lens), `glow` (glw!), `light_volume` (mgs2) | 200–500 | RES | effect graphs; particle shaders → `sprite` materials |
| `shader_*` (senv soso schi scex swat sgla smet spla sotr) | 300–600 | RES | material records: PS2 program id, blend mode, texture refs, animation params |
| `sound` (snd!), `sound_looping` (lsnd), `sound_environment` (snde) | 300–700 | SND | PS2 ADPCM banks; stream table for dialogue/music |
| `weapon_hud_interface`/`unit_hud_interface`/`grenade_hud_interface`/`hud_globals` (wphi unhi grhi hudg), `ui_widget_*`, `font`, `unicode_string_list` (ustr), `string_list` (str#) | 50–150 | RES/STR | HUD layouts, bitmap fonts, strings |
| `sky`, `fog`, `weather_particle_system` (rain), `wind`, `camera_track` (trak), `dialogue`, `item_collection` (itmc), `tag_collection`, `color_table` | small | RES | as-is |

## 4. Gameplay semantics we must reproduce (sources: public tag docs + observation)
- 30 Hz tick; object update order; projectile and damage timing in ticks.
- World units: 1 wu = 10 ft; biped radii, jump heights, gravity from `globals`.
- Shield recharge, health, difficulty multipliers (`globals` difficulty block).
- AI: perception (vision cones, hearing), danger zones, retreat thresholds,
  grenade usage, vehicle entry, squad spawning rules, encounter "respawn"
  behaviour, `ai_*` script commands.
- Scripting: `(sleep n)`, `(wake script)`, `begin_random`, `cond`, globals,
  object naming, `switch_bsp`, cinematic commands, objectives, `game_save`.
- Checkpoints: `game_save`/`game_save_no_timeout` conditions (no enemies aware,
  player not in danger).

## 5. Host tools in this repo
- `tools/halo2ps2/mapinfo.py` — header parse / decompress / summary (works today).
- `tools/halo2ps2/tagindex.py` (tag index, report), `inventory.py` (bitmap, sound
  and script-node statistics), `bsp.py` (BSP blocks and counts), `bspconv.py` (BSP geometry → `BSPn` section, see [08-bsp-section.md](08-bsp-section.md)), `hp2.py` (pack
  container, see [07-hp2-format.md](07-hp2-format.md)). Each documents the byte
  layouts it verified in its module docstring.
- Planned modules: `bitmaps.py`, `models.py`, `sounds.py`, `scripts.py`, `pack.py`.
- Third-party helpers (not vendored): `extract-xiso` (installed), Invader
  (`invader-info`, `invader-extract` — GPLv3, use as external executables),
  Reclaimer/Refinery (Python, GPLv3; candidate parsing backend — decision pending
  in Phase 0).

## 6. Legal posture
The user supplies the ISO. Tools read it locally; outputs land in `assets/`
which git ignores. Documentation describes formats only. No tag data, textures,
models, sounds, strings or video are committed. The engine is an original work.
