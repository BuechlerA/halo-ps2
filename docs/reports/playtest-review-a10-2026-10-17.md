# a10 playtest review (2026-10-17) and fix plan

The first mission looks broken in a playtest: the intro cutscene shows a blurry close-up with a dark claw in the corner for
its whole length, the first-person arms and gun are an unrecognisable dark mass, doors never open, the HUD sits in the wrong
corners. This report says why (root causes, with the evidence) and orders the fixes. It covers the tree at `origin/main`
(5aa9a55, Phase 4 work in progress, ADR-043..054). Section 4 records what was fixed the same day and what the Xbox reference showed.

## 1. How it was checked

| Run | Config | What it shows |
|---|---|---|
| intro | `scripts/paths/a10mission.cfg` + `fbdump` every 10 s for 100 s (recompiler) | the intro cinematic: `[mission]` log, 12 frame dumps |
| gameplay | `scripts/paths/a10game.cfg` + 8 `fbdump`s (gate M3 scenario, no scripts) | first person, HUD, effects, crewmen |
| code | `main.c`, `mission_app.c`, `sim/mission.c`, `sim/cine.[ch]`, `script/hs_host_misc.c`, `sim/fpview.c`, `render/obj_render.c`, `render/hud_render.c`, `sim/objects.c`, `sim/devices.h` | the wiring between the pure runtimes and the frame loop |
| data | a names-only dump of the a10 script tree and the ANIM / MODL sections of `a10.obj.hp2` (scratch only, not kept) | what the intro expects, the first-person node order |

Frames are converted with `scripts/fb2png.py`; the two contact sheets were handed over with this report.

## 2. Findings, most visible first

### F1. Cutscenes render from the player's eye, never from the script camera (the whole intro)
`main.c` fills the render camera with `game_camera(&s_game, &s_cam)` on every frame and nothing else. The cinematic camera
(`cine_state.cam`, driven by `camera_set`, `camera_control`, `camera_set_relative` in `hs_host_misc.c`) is advanced every tick
and `cine_cam_pose()` is never read. The intro script teleports and suspends the player in the cryo bay, then `switch_bsp`s to
the exterior (BSP 7) and the bridge (BSP 1) for its camera moves; the player stays at the cryo position, so the render camera
sits inside a wall of a BSP it is not in. That is the blurry lightmap close-up from frame 150 to frame 2990 (0:05 to 1:40).
The log confirms it: `cin 1` for the whole run, `switched to BSP 7` at 0.5 s and `BSP 1` at 48 s, player position constant.
Also `cine_pose_to_camera` drops roll (camera_t has none) and `near_clip` from `cinematic_set_near_clip_distance` is unused.

### F2. First-person arms and weapon are garbled: the graph's node order is not the model's
`fpview.c` / `game.c` pose the hands model (37 nodes) with graph nodes 0..36 of the weapon's first-person graph (42 nodes) and
the gun model (5 nodes) with graph nodes 37..41 (`weapon_node_offset = 37`), i.e. "hands first, then the gun" (scenconv's
assumption, docs/13). The real graph is ordered breadth-first over the whole rig; the gun nodes sit at graph indices 7, 18, 24,
25 and 31 between the hand nodes. Read from the a10 pack (rifle graph `e62c04b8`): the model-to-graph index map of the
hands is `0 1 2 3 4 5 6 8 9 10 11 12 13 14 15 16 17 19 20 21 22 23 26 27 28 29 30 32 .. 41`. So 30 of the 37 hand bones get
another bone's rotation and translation and the gun is posed with the five fingertip tracks. The pistol graph (44 nodes) has
the same structure. Result: the dark fingers filling the lower half of every gameplay frame (frames 160, 240, 320) and no
recognisable gun. The gate-M2 "first person" claim was never checked visually against the real game.
The ANIM section does not store graph node names (only animation names), so the engine cannot build the mapping today.
Secondary, to check after the remap: the arms render almost black (the view-model light floor is applied; the hands and rifle
first-person shaders may be falling back to a flat material: compare the `[tex] materials ... flat` style count for OTEX), and
the camera near plane (0.05 wu) against the nearest hand vertex (about 0.03 wu from the eye at the thumb).

### F3. No letterbox in cutscenes
The a10 scripts call `cinematic_show_letterbox` six times, all in the title-card scripts; the intro and the other cinematics
only call `cinematic_start`. In the original, cinematic mode itself shows the bars (`cinematic_show_letterbox` exists for bars
outside a cinematic). `hud_render.c` draws bars only from `screen.letterbox`, so the intro has none. Unverified against the
console; the xemu reference frame settles it.

### F4. First-person arms are drawn during cinematics
`obj_render_pass` draws the view model whenever `draw_fp` is set; `mission.cinematic` is not consulted (the HUD is). Every intro
frame carries the dark claw in the lower right.

### F5. Doors, lifts, the cryo tube and every other device never move visually
`sim/devices.c` computes positions and `devices_anim_frame()` (the frame of the device graph's position overlay, docs/27 §
reader) but no caller reads it outside `devices.c` itself; device objects keep frame 0 of their idle. Doors are passable when
powered (ADR-046: no device collision), so the playtest symptom is "walking through closed doors".

### F6. HUD anchors
Gameplay frames: ammo and grenade counters top-left, shield top-right, motion tracker top-right under the shield (it belongs
bottom-left), no tracker blips. docs/04 already lists "anchors other than top-left unverified"; it is the most visible HUD defect.

### F7. Effects
Muzzle-flash and impact sprites show rectangular sprite edges (frame 240, 560): the additive particles are drawn with alpha
blend or the FXTX alpha ramp keeps the sheet background. A translucent dark box hangs in frame 700 (a decal or smoke sprite).

### F8. Things that are fine for a10 as they are
The intro does not animate the player's body: it creates its own cinematic objects (`object_create_anew` + `custom_animation`)
and only teleports / suspends the player, so the hidden player body (`mission_attach_player`) and the ignored `recording_play`
on the player cost nothing in this mission. World geometry, lightmaps, crewmen, weapon fire and the `[game]` statistics look
right in the gameplay run (0 dropped frames beyond the dump frames).

### F9. Branch hygiene (blocks the xemu comparison)
`origin/main` holds Phases 1-4; the repository's PR base `claude/open-decisions-review-75968c` holds the xemu work
(`scripts/xemu-setup.sh`, `scripts/xemu-run.sh`, `docs/14-xbox-reference.md`, tag index tools) and none of the engine. A merge
of the xemu branch into main conflicts in `CLAUDE.md`, `README.md` and `docs/06-decisions.md` (both lines used the number
ADR-030). `docs/14-xbox-reference.md` also collides in numbering with `docs/14-phase3-plan.md`.

### Not bugs: the deliberate PS2 downgrades
Half-resolution palettised textures, luminance lightmaps with vertex chroma, no bump / specular / detail maps, 512x448 16-bit
colour, 30 fps (ADR-004, 019, 020, 023). They make the picture flatter than the Xbox but are decisions, not defects.

## 3. Fix plan, in order

Each step is one commit with its verification; format changes get an ADR. Steps 1, 3 and 4 are independent of each other.

| # | Fix | Where | Verify |
|---|---|---|---|
| 0 | Merge the xemu branch into main (resolve the three conflicts, renumber its ADR and doc), capture reference frames of the a10 intro at the same seconds as the dumps and of the first seconds of gameplay | git, `scripts/xemu-*.sh`, `docs/` | reference PNGs next to ours |
| 1 | Cinematic camera: after the tick, if the mission is active and `cine.cam.control` with mode POSE, fill `s_cam` from `cine_cam_pose()` (position, yaw, pitch, fov, near clip); FIRST_PERSON / DEAD / PLAYER keep `game_camera`. Add roll to `camera_t` and `camera_basis` so banked shots work. The object pass already culls by camera, the BSP is the loaded one | `main.c`, `render/camera.[ch]`, `sim/cine.c` | intro dumps at 150, 600, 1500 show the exterior and the bridge; `[mission]` log unchanged |
| 2 | First-person node remap: ANIM v2 adds the graph node names (32 B each, about 60 KB on a10); `anim.[ch]` exposes them; `objects_pose_model` takes a per-node remap (model node -> graph node, -1 = rest pose) instead of `node_offset`; `fpview` / `game.c fp_apply_slot` build the remap by name once per weapon for the hands and the gun; `custom_animation` on objects whose model order differs uses the same path. Drop `weapon_node_offset`. Rebuild the packs (`scripts/build-world.sh a10 a30 b40`), ADR | `tools/halo2ps2/animconv.py`, `engine/src/world/anim.[ch]`, `sim/objects.[ch]`, `sim/fpview.[ch]`, `sim/game.c`, `engine/tests/test_anim.c`, docs/12 | gameplay dumps: rifle held in two hands lower right, like the reference; host test asserts the a10 hands map above |
| 3 | Cinematic screen: letterbox bars while `screen.cinematic` (not only `letterbox_on`); skip the view model and the HUD damage wash during a cinematic; titles with the cutscene title style's position and colour from `CINE` instead of the fixed 120/330 | `render/hud_render.c`, `render/obj_render.c` | intro dumps show bars and no claw; title card at the training section |
| 4 | Device animation: a post-tick pass sets each placed device object's animation to the graph's position overlay at `devices_anim_frame()` (frame driven, `OBJ_F_ANIMATED` clear) | `mission_app.c` or `sim/mission.c`, `sim/objects.h` | the first cryo-bay door opens on approach in a dump pair |
| 5 | HUD anchors and the motion tracker: resolve the anchor enum of `unit_hud_interface` / `weapon_hud_interface` in `hudconv.py` (top-left, top-right, bottom-left, bottom-right, centre) and draw the tracker's blips from the units in range | `tools/halo2ps2/hudconv.py`, `ui/huddef.[ch]`, `ui/hud.c`, docs/20 | layout matches the reference frame |
| 6 | View-model shading and sprite edges: log the shader bindings of the hands and gun models, fix the fallback; additive blend for the "add" particle class and the FXTX alpha ramp | `render/obj_render.c`, `tools/halo2ps2/fxconv.py`, `render/fx_render.c` | muzzle flash without a box in the frame-240 pose |
| 7 | Playable config and regression: `scripts/paths/a10play.cfg` (`mission 1`, `bot 0`, no frame limit) documented with the pad mapping; rerun `a10game`, `a10mission` (interpreter) and the M1/M2 configs for the dropped-frame numbers | `scripts/paths/`, docs/10, docs/04 | gates still 0 dropped; the intro plays end to end on the screen |

Step 2 is the only format change. Steps 1 to 4 together make the playtest look like the game; 5 to 7 are polish and proof.

## 4. Status after the first fixes (same day)

Done on this branch (host tests 83 ok, PCSX2 recompiler runs of `a10mission` for four minutes and `a10game`):

| Step | Result |
|---|---|
| 0 | xemu branch merged (ADR-055, docs/35); `scripts/xemu-setup.sh` binds the keyboard to controller port 1; reference frames of the a10 intro (bridge, hangar) and an attract-mode gameplay frame captured from xemu by window id |
| 1 | the script camera renders: the intro shows the exterior, Threshold, the Longswords, the bridge, the hangar and the cryo bay in turn; `camera_t` has roll; the near clip follows `cinematic_set_near_clip_distance` |
| 2 | ANIM v2 (graph node names) + name-based node map (ADR-056): rifle and pistol are held like the reference, reload and ready poses are right; overlay-type animations (`moving`, `overlays`) are no longer played as base poses |
| 3 | letterbox bars during cinematics (verified against xemu, which shows them for the whole intro), view model and damage wash hidden while a cinematic runs |
| 4 | device objects follow their device position (`mission_sync_devices`, `OBJ_F_POSED`); not yet checked on a door in a frame dump |
| 5 | HUD anchors and tracker (2026-10-18, ADR-060): anchors on the 7.5 % title-safe frame, motion sensor bottom left, blips from the units in range. Measured on an xemu attract-demo frame: the ammo and grenade counters are **top left** (an earlier version of this paragraph said "beside the shield", which was wrong), the shield top right, the tracker bottom left |

F10 and F11 (2026-10-18): both had one cause, not the ones guessed above. The sky pass wrote its uv scale and GIF tag into VU1 addresses 2..3,
the BSP program's clip matrix rows for z and translation, so every frame with the sky model loaded drew the world with a corrupted projection
(the "blob" and the "slivers"; ADR-057, docs/03 §5.2). With the pass fixed to GS state only, the exterior shows the hull plating from its
`poa-*` base maps and the hangar is clean; the 14 flat materials of BSP 7 are cube-map glass and a self-illumination shader (docs/09) and the
glass ones are now skipped instead of adding grey. The bridge brightness is reported in docs/09 (gain unchanged), the hangar's EE clip budget in docs/08 (no visible loss at the dump poses).

Still open from the list above: F7 (sprite edges), the hands' colour: the gloves render navy because the colour-change / multipurpose maps are not converted (docs/11), and the Xbox tints them olive.

New findings from the fixed intro (compare `frames/intro2`, `frames/cryo` with the xemu captures):

* **F10. The exterior BSP (7) is a flat grey blob.** 14 of its 55 materials are flat (no decodable base map) and the lightmap dominates; the Xbox shows the ship's hull plating. Needs the shader kinds of BSP 7 listed and converted (probably `shader_transparent_*` and environment shaders with detail-only bases).
* **F11. The hangar BSP (8) draws streaky black-and-white triangles** across the upper half of every frame (frames 4500-5700): large triangles crossing the near plane or the guard band are not clipped (ADR-017 clips nothing on VU1). The bridge (BSP 1) is also far darker than the Xbox: lightmap gain or the luminance-only conversion for that BSP.
* **F12. Text and pacing.** The objective text is drawn twice at mission start (help and objective kinds hold the same string). The player starts the cryo bay holding a rifle; whether the Xbox start is unarmed is to be checked against the reference. The four-minute intro run drops 6.8 % of frames (BSP switches and the 50-unit hangar scene), so Gate M4's timing is still open.

Reference capture recipe (macOS): bind the keyboard in the xemu config, send key codes with System Events (Return = Start, arrows = D-pad; character keystrokes do not reach SDL), find the window id with `CGWindowListCopyWindowInfo` and capture it with `screencapture -l <id>` even while another window covers it. A save state at the cryo bay would remove the menu crawl next time.

## 5. Gate M4 run, 2026-10-18: the bot leaving the world and the frame budget (ADR-061)

`a10mission` under the bot on the recompiler: 20749 frames, 1635 late (7.88 %), 31 assists, `game_won` at 691 s. The log showed the player at z = -38.7, -16, -15 for about 100 s and frame dumps of a flat blue void.

| Question | Finding |
|---|---|
| Why does the player leave the world | the bot's "goal unreachable" hop teleports onto the awaited volume's centre; the volumes of the next BSP lie in its space, beyond BSP 0's floor (probe of `COL0`: floor to x = -33.1, `COL1` has floor at x = -29.4). Nothing held the player, so it fell for ever |
| Why did the mission still win | the script chain 91 -> 25 -> 28 -> 30 -> 122 -> `switch_bsp 1` advanced because the falling player crossed the volumes. With a plain "rescue" the chain stalled: scripts poll their volumes, so the bot must stay inside for a few ticks |
| Why the frame budget blew out | a camera in no BSP cluster draws every cluster in the frustum: 25-55k triangles, 1000-2400 batches, EE 35-50 ms. About 1450 of the 1635 late frames fell in the 14400-19800 frame windows where the player was out of the world |
| Why the bot stalled once rescued | the ignore list had 4 slots and was overwritten by the dozen awaited volumes before it expired; off-mesh volumes were only ever ignored, never visited |
| In-world cost, BSP 1 cutscenes | sim 8-11 ms (`units` 5 ms; 28-38 of 45 live units report `on_ground` 0 at floor height, cause not investigated) + build 12-18 ms (EE clip budget 6 ms, object pass 3-9 ms) |

After the fixes (same config): 24256 frames, 712 late (2.94 %), `game_won` at 773 s, 60 assists of which 24 rescues, no fall, `a10game` 2 of 1081 late. Still open: the BSP 6 finale cutscene (frames 21975-22147, 173 late frames in a row, EE 29-52 ms:
sim 11-17 ms plus build 15-28 ms with the object pass at its 900-batch cap) and the crowd scenes of BSP 0 / 8 (EE 16.5 ms, GS late: 21k object triangles, 964 batches, `lod=4`). The bot still takes 60 assists, so Gate M4's timing claim stays "script, AI and device
control under a bot, not a human run".

### 5.1 The remaining late frames, measured (ADR-062)

| Question | Finding |
|---|---|
| Why the crowd scenes of BSP 0 / 8 were late | not the GS: EE 16.6 ms, GPU wait 0.0, vsync burn 16.8, present 16.7. The burn loop pumped the stream after deciding which vsync to wait for; a frame whose EE work ended within a pump of the vblank missed it and landed 3 vblanks out. Fixed in `main.c`; about 470 of the 703 late frames were this |
| Why the BSP 6 finale is EE-bound | sim 11-17 ms: `units` 5 ms (18 body integrations per tick), `ai` 1.5, `post` 1.2, `dev` 1.2, `stream` 1.4, `game` 1.1; build 15-28 ms: world 15k tris plus objects 20k tris at the 900-batch cap, every visible object posed before the cap dropped it |
| What carried units cost | 29-38 of 42 units in the cryo bay and the dropship scenes reported `on_ground` 0: `unit_suspended` and `objects_attach` re-teleported them every tick (`units_teleport` clears `on_ground`), so each swept its capsule for nothing. `units_pin` fixes it (BSP 0 cryo `units` 0.5 -> 0.01 ms, BSP 6 `units` 3.4 -> 2.4 ms) |
| Devices | `mission_sync_devices` decoded every device record each tick: 1.17 -> 0.50 ms per tick |
| Result (same config, normal speed, recompiler) | 90 of 24256 late (0.37 %, was 703 = 2.90 %); `game_won` at tick 23210, 60 assists (24 rescues), identical to the run before; `a10game` 0 of 1081 |
| Still late | finale 60 frames (EE 34-41 ms), BSP 6 load 14, one stall per BSP switch; `cinelod 1` / `unitlod 20` would cut the finale further but send the bot down a different route (73-89 assists), so they stay off |

Not covered: the bot's assists are unchanged (60); a human run was not measured; real-console GS cost is still unmeasured (ADR-022).
