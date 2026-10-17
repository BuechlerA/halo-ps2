# a10 playtest review (2026-10-17) and fix plan

The first mission looks broken in a playtest: the intro cutscene shows a blurry close-up with a dark claw in the corner for
its whole length, the first-person arms and gun are an unrecognisable dark mass, doors never open, the HUD sits in the wrong
corners. This report says why (root causes, with the evidence) and orders the fixes. It covers the tree at `origin/main`
(5aa9a55, Phase 4 work in progress, ADR-043..054). Nothing was fixed yet; the host tests pass (83 ok) and the ELF builds.

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
