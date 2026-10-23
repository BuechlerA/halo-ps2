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

## Open (see ADR-081 .. 084)
The hangar's colour grade and Johnson's skin tone, the cryo console crew framing (ticks 6420 .. 6500), Chief stepping out of the pod (7020 .. 7200), the scorpions' colour, the ship hum, the title-safe
margins of the Xbox view (we fill the whole buffer), the `camera_ticks` durations (the director divides ticks by 30 as an integer).
