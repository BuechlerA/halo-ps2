# The main menu (ADR-090)

The Xbox's menu is not a picture: it is the scenario `levels\ui\ui` (a Halo ring in space, `scenery\halo\halo`, the sky `sky_ui`, ten camera points and the continuous script
`ring_loop` that tours them), 485 `DeLa` widget tags, their bitmaps, string lists and fonts. The port runs that scenario as a mission (the script machine, the cinematic camera,
the streamed music) and draws the screens over it.

## Data (`tools/halo2ps2/menuconv.py` -> `assets/hp2/ui.menu.hp2`, built by `scripts/build-world.sh ui`)
- `ui.hp2` / `ui.tex.hp2` / `ui.obj.hp2` / `ui.dev.hp2` / `ui.script.hp2` / `ui.snd.hp2` / `ui.strm.hp2`: the scenario through the ordinary converters. `objpack.py` tolerates a map without campaign
  globals (`ui.map` has none); `soundconv.py` takes the `sound\sfx\ui\` clicks as roots (nothing in the tags references them: the executable plays them); the music is the looping sound
  `sound\music\title1\title1` (tag 0xe54903d3), whose two streams are in `ui.strm.hp2`.
- `ui.menu.hp2`: `MENU` (44 sprites in the order of `menudef.h`, the ids of 7 string lists and 2 fonts), `FONT` + `STRS` (hudconv's writers: `ui\small_ui`, `ui\large_ui`, the lists below), `HUDT` (the menu bitmaps and
  the font atlases as PSMT8 textures: the logo at 512 x 128, the rest at most 256 a side).
- Widget layout read off the `DeLa` tags (the field list is in the docstring of `menuconv.py`): a widget sits at its own bounds (left, top) plus its child record's offsets, absolute on 640 x 480.

| screen | widgets (position) |
|---|---|
| title | `halo_logo` (0, 28), `button_key[13]` "Press START to Continue" |
| main menu | logo (0, 28), `main_menu_item_load_camp` (194, 247), `_multiplayer` (192, 283), `_settings` (192, 319), `_game_demos` (192, 355): 256 x 64 bitmaps, frame 0 normal, frame 1 highlighted; they overlap, the text is in the top 33 px |
| difficulty | `gradient` full screen, `header_choose_difficulty` (35, 11), four items at (51, 78 + 33 i) 232 x 32 on `list_item_bkd_top` / `list_item_bkd` (frame 1 highlighted), `list_field` panel (275, 78), emblem `difficulty_options[i]` (333, 88), the description below it, `button_key` (371, 414): `b_butn` + "BACK", `a_butn` (471) + "ACCEPT" |
| level select | header (35, 11), three cards (72 / 239 / 406, 75) on `spinner_list_3_wide_item_background` (frame 1 the centre), picture `sp_levels[i]` at (card + 13, 87) cropped to 140 x 114, name `map_list[i]` (large_ui, centred, y 204), description `map_data[i]` (small_ui, y 258); a level whose packs are missing shows `sp_levels[10]` and `map_list[10]` "This Level Unavailable" |

## Flow and camera (`engine/src/ui/menu.c`, pure C, `engine/tests/test_menu.c`)
Title -> main menu (START / X) -> CAMPAIGN (one playable level: the difficulty select, several: the level select first) -> difficulty (EASY NORMAL HEROIC LEGENDARY, NORMAL first) -> the game. MULTIPLAYER, SETTINGS and GAME DEMOS show a notice box
(they do not exist in this port). Circle / Triangle go back. Every item moves the cinematic camera to its named point (`load_campaign`, `multiplayer`, `settings`; `new_campaign` for the difficulty and level screens)
over 60 ticks and the script's own tour (`ring_loop`: points 0, 5 .. 14, every 200 ticks) is held while the menu owns the camera; after 450 ticks without a key the tour resumes.
The clicks are `sound\sfx\ui\cursor`, `forward`, `back` and `flag_failure` (denied). The mission host gets three entry points (`mission_menu_begin / _camera / _camera_hold / _loop / _sound`, `hs_host_misc.c`);
the quads go through `hud_render_t.overlay` (the HUD's overlay pass, same texture source mechanism).

## Starting the game
`menu 1` in `halo.cfg` (`scripts/paths/halo.cfg`) makes the engine boot into the menu with the `menu_*` packs. A start from the menu calls `LoadExecPS2(elf, "play:<level>:<difficulty>")`: the engine boots again, reads the argument
(`apply_play_args`: the campaign level's packs `assets/hp2/<map>.*`, `difficulty`, `menu 0`) and plays. Checked on PCSX2 (audio comes back up in the second boot). On a console the same call needs the ELF's own path to stay valid (`argv[0]`);
that is a Phase 6 check (ADR-022).

## Debug and tests
`menukey <tick> <up|down|left|right|accept|back|start>` scripts keys for runs without a pad; `menuexit 1` ends the run at the launch instead of restarting; `menu_levels_or <mask>` pretends levels are installed (the level select
layout), `menu_skiptitle 1`. `engine/tests/test_menu.c` (flow, quads on screen and inside their textures, corrupt sections), `tools/tests/test_menuconv.py` (the sprite table against `menudef.c`).

## Not done
The pause menu ("quit to the main menu"), profiles and saved campaigns (LOAD CAMPAIGN is the level select), SETTINGS (controls, audio), MULTIPLAYER, the attract-mode movies (`bink\attract*.bik`, `intro.bik`: Bink is not decoded here), the
fade from black when the menu starts, the widget runtime itself (the screens are laid out in code from the tag data), the other campaign levels' packs (`scripts/build-world.sh a30 ...`).
