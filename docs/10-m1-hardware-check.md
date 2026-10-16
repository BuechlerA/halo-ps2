# Gate M1 — hardware check procedure and `halo.cfg` reference

> **Status: deferred to Phase 6 (ADR-022).** Day-to-day gates run on the PCSX2 interpreter; this document is kept
> as the console procedure, the results so far, and the `halo.cfg` reference.

M1 ([04-roadmap.md](04-roadmap.md)): **noclip fly-through of a30 and b40 at 30 fps (the console runs below were made
when the target was still 60, ADR-023) on hardware with
< 1 % dropped frames; memory report under budget.** Everything below the line "Hardware" is the part
that cannot be done in PCSX2 (ADR-009/015) and has not been done yet.

## What is already measured (PCSX2, `scripts/profile-m1.sh`, recompiler and interpreter)
Scripted 36 s loops (`scripts/paths/{a30,b40,a10}.cfg`; 1080/1080/900 frames at the 30 fps target, 2160/2160/1800 when run with `fps 60`): **0 dropped frames** at 30 fps and at 60 fps on all
three, in both modes (identical to within 0.1 ms: the interpreter changes how PCSX2 executes, not its cycle
model). Final tree at 30 fps (EE build 6.3–8.5 ms average, peak 9.2 ms, `[prof]` reports fps=30.0; the 60 fps figures below are the earlier measurement) (6 ms clip budget, colour-keyed trees, chroma-baked colours): EE frame build 4.8–8.7 ms on
average with peaks of 9.1 ms (b40 BSP 11 is the heaviest; the console measured 8.4/8.9 ms there, so the model is
close for EE), VIF1 and GS 0.4–1.8 ms in the model (it does not model GS fill: the console showed ~7.9 ms and
drops on b40, ADR-022), visible triangles 1.5–7.5 k, level arena 3.0–4.2 MB, texture cache at most 47 % full. These numbers only prove the pipeline, the EE-side cost and the
absence of stalls; they are not hardware numbers.

## Hardware results so far (2026-10-16, console at 192.168.0.10)
- **Self test (`selftest 2`)**: matches on hardware: PSMT4 with the 8x2 CLUT, PSMT8 with the CSM1 swap and the
  deliberately wrong 16x1 variant behave exactly as in PCSX2 (ADR-019/020 confirmed).
- **a30 fly-through** (first run, *before* the CLIPW fix below): ~59 fps, 12 of ~2154 frames dropped (0.56 %,
  under the 1 % gate), but the picture was a mess of stretched triangles: vertices behind the camera were not
  being rejected (CLIPW compares against |w|). Fixed (ADR-017) and verified against `scripts/refrender.py` on
  PCSX2.
- **a30 fly-through, corrected build**: 60 fps, **17 of 2154 frames dropped (0.79 %)**, under the 1 % gate (read
  from the on-screen overlay; the picture's correctness on the TV was not yet confirmed in words).
- **a30 with the 6 ms clip budget and colour-keyed trees**: EE 4.60 ms average / 5.02 peak on the console (PCSX2's
  model says 3.7–4.9, so it is a fair predictor of EE time), **14 of 2154 frames dropped (0.65 %)**. VIF1/GS ms read
  -1 (not finished at the last poll); `bsp_render_wait_gpu` now spin-polls so the next run reports them.
- Still open: b40 fly-through, memory report from the console (EE printf is not on ps2link's TTY), the picture check.

## Hardware
Needs: a PS2 running ps2link, the ELF, converted packs, `ps2client`, the TTY on a PC.

1. Build and convert (host):
   ```bash
   make -C engine
   scripts/build-world.sh a10 a30 b40        # assets/hp2/<map>.hp2 + <map>.tex.hp2
   ```
2. `ps2client`'s `host:` root is the directory it is started in. Start it in the repository root, so
   `host:assets/hp2/a30.hp2` and `host:halo.cfg` resolve:
   ```bash
   cp scripts/paths/a30.cfg halo.cfg         # halo.cfg is gitignored
   ps2client -h <console ip> execee host:engine/halo-ps2.elf
   ```
   **ps2link's TTY does not carry the EE's `printf`** (only IOP messages and fragments), so add `overlay 1`
   and `hold 1` to `halo.cfg`: the profiler overlay shows FPS, frame count and DROPPED on screen, and
   `frames 1080` ends the run (36 s at 30 fps) and holds the last frame so the final DROPPED can be read. On PCSX2
   the `[prof]`/`[done]` TTY lines are available. A running ELF cannot be replaced by `ps2client execee`:
   restart ps2link between runs (and do not use `ps2client reset`, it drops ps2link).
3. Repeat with `scripts/paths/b40.cfg` (and a10 for a tight-interior look). **Pass: `dropped` < 1 % of
   frames for a30 and b40**, `[mem]` shows the level arena and texture cache well inside budget, and the
   fly-through looks like the PCSX2 one.
4. Texture path self test: `selftest 2` in `halo.cfg` (plus `pack assets/hp2/a10.hp2`) draws known
   patterns and holds the picture. Expected: top-left a 4x4 grid of 16 smoothly changing colours
   (PSMT4 with an 8x2 CLUT), top-right the same with only the first two rows right and the lower half
   black (the deliberately wrong 16x1 CLUT upload), bottom-left a smooth 16x16 gradient (PSMT8 with the
   CSM1 swap applied, ADR-019). Any other result means the CLUT order or the PSMT4 nibble order differs
   on hardware.
5. Tune on screen, record in the roadmap: lightmap gain (`LM_GAIN=… scripts/build-world.sh`), the
   `clipbudget` that keeps the frame inside 16.7 ms, whether the frame needs mipmaps (distant terrain
   shimmer / GS texture cache stalls), `debug` bits if something looks wrong.
6. Optionally capture frames from the console: `fbdump <frame>` prints a hex dump on the TTY;
   `python3 -I scripts/fb2png.py <captured log> out` converts it (needs Pillow).

Phase 0's missing hardware pacing numbers (roadmap, M0) come out of the same runs.

## Controls (DualShock 2, port 0)
Left stick move, right stick look, R1/L1 up/down, R2 fast, Start toggles the overlay. Without a pad (or
with `pathkey` lines in the config) the camera follows the scripted path.

## `halo.cfg`
One `key value…` per line, `#` comments, read from `host:halo.cfg` at boot (optional).

| Key | Meaning | Default |
|---|---|---|
| `pack <path>` | BSP pack (BSPS v2) | `assets/hp2/a10.hp2` |
| `tex <path>` | TEXS + LMn pack; omitted = everything flat | none |
| `bsp <n>` | BSP index inside the pack | 0 |
| `arena_mb <n>` | level arena size (the one boot-time malloc) | 20 |
| `camera x y z yaw pitch` | start pose (degrees) | centre of the BSP bounds |
| `pathkey t x y z yaw pitch` | scripted camera keys (seconds, looped, up to 24) | none |
| `frames <n>` | stop after n frames, print `[done]` | run forever |
| `fps 30/60` | frame rate: 30 = one frame per two vblanks (default, ADR-023), 60 = one per vblank | 30 |
| `overlay 0/1` | profiler text | 1 |
| `fog 1 near far r g b` | linear distance fog | off |
| `sky r g b`, `horizon r g b` | gradient behind the world (top, horizon) | dark blue |
| `textures 0/1`, `lightmaps 0/1` | disable a pass for debugging | 1, 1 |
| `clipbudget ms` | EE time for clipping near triangles (ADR-017) | 6 |
| `fbdump <frame>` | print that frame's framebuffer on the TTY (repeatable) | none |
| `obj <path>` | object pack (SCEN, MODL, OTEX, ANIM) of the map; objects of the chosen BSP are placed | none |
| `objects 0/1`, `fp 0/1` | draw the placed objects / the first-person arms and weapon | 1, 1 |
| `objanim 0/1`, `fpanim 0/1` | play idle animations on placed objects / on the first-person arms | 1, 1 |
| `objhidden 1` | also spawn the placements scripts would create later (stress population) | 0 |
| `objdist <wu>`, `objcap <n>` | object draw distance (units) and per-frame batch cap | 200, 900 |
| `sky3d 0/1`, `skyfog 0/1` | draw the sky model from the object pack's `SKY0` section / take the world fog from the sky tag (ADR-029) | 1, 0 |
| `lodctl 0/1` | dynamic LOD controller | 1 |
| `objlist 1` | print every placed object on the TTY | 0 |
| `chief x y z yaw` / `chiefcam 1` | spawn the player's biped (third-person Master Chief) at a pose / 3 units ahead of the start camera | off |
| `selftest 1/2` | GS texture-path self test (1: dump and exit, 2: hold the picture) | 0 |
| `printcam 1` | print the free-fly camera pose each second | 0 |
| `debug <bits>` | 1 skip pass-1 batches, 2 skip pass 2, 4 skip overlay, 8 PSMT4 flat, 16 PSMT4 CLUT 16x1, 32 no EE clipping, 64 no portal culling | 0 |

Maps and views: `scripts/campos.py` lists camera positions inside clusters, `scripts/scout-views.py`
runs the engine over candidates and reports the heaviest views, `scripts/refrender.py` is an independent
software projection of a BSP (compare against a framebuffer dump).
