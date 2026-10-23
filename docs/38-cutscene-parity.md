# Cutscene parity: how the opening cutscene is checked, and what it taught (2026-10-23)

Ground truth is the Xbox executable and the tag data, **not** the reference video (an approximation, probably not a plain 4:3 Xbox capture). The video finds
*where to look*; the decompile (docs/36) decides. When they disagree, record both in the ADR (ADR-084) and follow the executable.

## Workflow
- Parity frames: `scripts/cmp-intro.sh REF.mp4 OUTDIR first last step 1.41 ["cfg line" ...]` (PS2 left, video right; the offset is 1.18 s up to tick ~1400, 1.41 s after; `CFG=`, `PACK=`, `TEX=`,
  `OBJ=` env, `SPEED=8` for a fast run whose timings mean nothing). For a list of ticks write the cfg by hand (`fbdump <tick>` lines, `frames`), as `cmp-intro.sh` does.
- Name what is on screen: `nearlog <tick>` + `nearrad <wu>` prints every object near the cinematic camera with its permutations, animation, position and screen position; `nearnodes 1`,
  `nearaxes 1` add node and axis dumps. `trackperm N` tracks the objects whose first permutation is N (Johnson: 1 on the marine model) every 30 ticks, `objtrace first last step` the cull inputs,
  `animlog 1` the animation a unit is given and why, `scripttrace 1|2` the script's built-in calls (2 is very slow), `forceanim obj anim frame`, `forcemodel`, `camlock x y z yaw pitch`
  (degrees: the cinematic camera stays there, to look at a scene from outside the shot), `unitspeeds 1` (tag speeds), `widescreen 0..3` (projection variants), `objects 0` (BSP only).
- Script text: `python3 -I scripts/hsdump.py assets/decompressed/a10.map [name]`; object and flag indices in scripts are scenario name indices, not engine slots (`nearlog` shows slots).
  Recordings: `tools/halo2ps2/cinconv.py` (`cinconv.collect`, `cinconv.play`) prints the controls of a recording per tick.
- Scoring many frames: render the ticks for each variant, auto-crop the letterbox, compare 64 x 26 luminance by normalised correlation. It is crude (it preferred 70 degrees horizontal to the
  Xbox projection) and a tie-breaker at best.

## Findings worth remembering
1. **Recordings carry controls, not positions** (docs/28). Where a recorded unit ends up depends on its speed: Johnson's 134 ticks of throttle (`johnson_enter`) end 0.5 wu from the camera of the next
   shot at the alert animation's 0.42 wu/s and 9 wu away at the standing 2.10. Unit speed follows the move animation played (ADR-083/084, docs/31).
2. **The script decides the posture** (`unit_set_seat unit "alert"`), a recording's `FORCE_ALERT` bit does not; units created anew (`object_create_anew`) start without it. Six marines that run past
   a low camera in the video are exactly the ones created anew.
3. The desired permutation of a biped placement (+0x06) selects `-<number>` permutations (Johnson `-100`, the cap `-101`); no permutation = the random pool `-1 .. -99` (ADR-083).
4. An object must be drawn whole or not at all: parts dropped one by one by the batch budget leave a head and a rifle floating (ADR-083). Detail levels are chosen so that every region has parts.
5. **The camera is the executable's** (ADR-084): `euler_angles3d_to_matrix` (0xb67f0), `forward = row 0`, `up = row 2`; the observer (0x11c500 .. 0x11cda0) with analytic velocity and acceleration;
   verified against the emulated executable. A settled camera pose is exact; mid-flight retargets used to differ by up to 1.3 units.
6. **Projection** (ADR-084): `vfov = 2 atan(0.6375 tan(fov / 2))`, window 544 x 408, horizontal half tangent `0.85 tan(fov / 2)`. It makes the cryo console shot (ticks 6100 .. 6240) overflow its
   frame, which the video does not show: unresolved, probably an input (view window, field of view) not yet found in the code.
7. The display text of that shot is not the problem: the text quad (`hushed casket-on/off`, 0.208 x 0.068 wu) fits the console screen with margins when seen from above (`camlock`).

8. **The cryo exit is the player's own body** (ADR-085): `fast_setup` / `tutorial_setup` seat the player in the pod (`unit_enter_vehicle (player0) <pod> "ct-driver"`), open it (`unit_open`),
   `unit_exit_vehicle` plays `CT-driver exit` in the pod's frame, `object_beautify (player0)` shows the body, the player continues at the exit animation's last pelvis position. The pod itself stands up
   through its animation (node 0 rotated 90 degrees), so the marker and the Chief must follow the animated pose, not the placement.
9. Fades to white were grey (untextured GS quads take 255 as white, 128 is half) and `object_beautify` was registered twice with the later no-op winning: when a built-in "does nothing" check the registration order
   of the host domains (`hs_host_register_*`, the later one overrides).
10. Compare the end of a cutscene with the bot off (`bot 0`): the bot steers the player the moment control returns, which hides where the hand-off really puts the camera.

11. **The video continues after the pod** (it is 467 s long): the first minutes of gameplay are a reference too (empty hands, one help line and `Checkpoint... done`, the technician walking up). Compare the hand-off with
    `scripts/cmp-intro.sh` at ticks 7300 .. 8700 (offset 1.41) with `bot 0` in the cfg (`sed 's/^bot 1/bot 0/'`).
12. A HUD element that is "missing" may only have failed to get texture pages: the overlay pass runs after the world, the 8-page font needs a contiguous run, and scattered free pages do not count (ADR-085, pinned font).

13. **Later gameplay, by events** (the video's player cannot be reproduced, the scripted events can be lined up): the video's first 45 s after the pod (ticks 7337 .. 8717) show the corridor, the technician walking up
    (7457), the lights going out with the red alarm and marines fighting (7697 ..), an explosion in the corridor (7697, 8057), `Checkpoint... done` (7399 .. 7519, 8177 .. 8237), a blue flash (8237), marines running
    along the corridor (8300) and `Loading...` / `Loading... done` (8537 .. 8597). Our bot run shows the same sequence: checkpoints at 7319 and 8079 (the message lasts 150 ticks), AI placed at 7536 with the playfight
    and berserk scripts at 7554, the red-lit fight room with dead marines (7820), the marines running at the camera (8300). Paths differ (the bot walks its own route, died once and reverted at 7940), the corridors are lit
    more brightly than the video's (the known colour grade), and the Xbox shows `Loading...` while streaming a BSP: ours shows `Loading... done` after its blocking load.

14. **Explosions** (`animlog 1` prints `effect_new` with tick and place): the corridor explosion of the video (7697) corresponds to our `effect_new e92707b3` x3 at 7600 .. 7620 near (-50, -13) (and again at 7746 .. 7766
    after the bot's revert) and a second kind (`ed2a0bb6`) at 7620 / 7665 / 7829 / 8401. Rendered (ticks 7604 .. 7670) ours shows two small orange flames and grey smoke puffs at the crates and a body on the floor where the video
    has a large fireball with a silhouetted body thrown through it: the particle effect is much smaller than the Xbox's (open: the fx particle fidelity, not the hand-off).

## Open (see ADR-081 .. 085)
The hangar's colour grade and Johnson's skin tone, the cryo console crew framing (ticks 6420 .. 6500), the tutorial's exact convolution kernel (a sprite blur and a veil stand in, ADR-085), the scorpions' colour, the ship hum, the title-safe
margins of the Xbox view (we fill the whole buffer), the `camera_ticks` durations (the director divides ticks by 30 as an integer).
