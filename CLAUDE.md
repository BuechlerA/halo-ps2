# halo-ps2 — working notes for agents

Port of Halo: Combat Evolved (Xbox) to PlayStation 2 at 30 fps (60 fps is the deferred
stretch goal, ADR-023). Read `docs/` in
order (01 → 06) before changing architecture; `docs/06-decisions.md` is the ADR log.

## Build / run
- Toolchain: `~/ps2dev` (EE gcc 15.2, `dvp-as`, ps2sdk, gsKit). The Makefile sets
  `PS2DEV`/`PS2SDK`/`PATH` itself; no shell setup needed.
- `make -C engine` → `engine/halo-ps2.elf`. `make -C engine clean`.
- `scripts/run-pcsx2.sh` boots the ELF in PCSX2. Hardware: `ps2client -h <ip> execee host:engine/halo-ps2.elf`.
- Tools: `for t in tools/tests/test_*.py; do python3 -I $t; done` (no game data needed); engine host tests: `make -C engine/tests`.
- Frame pacing in PCSX2 without hardware: `scripts/profile-pcsx2.sh --interpreter` (ADR-015).
- Phase 1-3 world: `scripts/build-world.sh a10 a30 b40` converts maps to `assets/hp2/` (BSP, textures, objects, and the gameplay packs `.coll .cmod .unit .weap .ansm .hud .snd .fx .hp2`, docs/14-phase3-plan.md); `scripts/run-headless.sh -c halo.cfg`
  runs the ELF in PCSX2 against a config (`docs/10-m1-hardware-check.md`); `scripts/profile-m1.sh` runs the M1
  fly-throughs (incl. `a10game`, the Gate M3 shooting scenario with scripted `input` lines; `game <prefix>` in halo.cfg turns gameplay on); `fbdump <frame>` in the config + `scripts/fb2png.py` is how rendering is checked without a screen.
- Phase 4 mission: `build-world.sh` also writes `.script .ai .path .dev .cin .strm .hp2` (strm is ~120 MB per map); `scripts/paths/a10mission.cfg` is the Gate M4 run (`mission 1`, `bot 1`: the in-engine
  mission bot, ADR-054; `exitonend 1` stops at game_won/lost; `SPEED=n scripts/run-headless.sh ...` speeds PCSX2 up for development only, timings then mean nothing; `threaddump 1` lists what scripts wait on).
  Reports must state the bot's assists (teleports) and what it does not cover.
- Xbox reference: `scripts/xemu-setup.sh` then `scripts/xemu-run.sh` boot the ISO in xemu for parity checks (`docs/35-xbox-reference.md`, ADR-055); the
  MCPX/BIOS/HDD files are user-supplied (`~/Developer/xbox-bios/`) and never committed.
- Game data lives only in `assets/` and `*.iso` (both gitignored). Never commit it,
  never copy tag data into docs or tests.

## Conventions
- C11, no `double`, no heap after level load, 16-byte aligned DMA structs, VU1 data
  layouts documented next to the `.vsm` that consumes them.
- Every new decision gets an ADR entry. Milestones are gated on the PCSX2 interpreter; real-console checks are
  deferred to Phase 6 (ADR-022) — do not run or request hardware tests before then unless the user asks. PCSX2
  does not model GS fill/DMA: treat GS cost as unmeasured and keep EE budgets with margin.

## Ground truth and tools (cutscene work, ADR-083 / ADR-084)
- **The Xbox executable and the tag data are the ground truth; the reference video is only an approximation.** Read the executable with `tools/xbe` (flatten, Ghidra headless
  `analyzeHeadless <abs project dir that exists> ...`, `fn.py` / `dis.py`; docs/36) and, when a behaviour is subtle, **run its own functions under the unicorn x86 emulator**
  (`tools/xbe/observer_oracle.py`: `python3 -m venv`, `pip install unicorn`): the camera observer is tested against it (`engine/tests/test_cine_observer.c`). Do not reconstruct a convention from
  pictures; do not drive xemu (docs/35: the menu cannot be driven reliably).
- Cutscene parity workflow, debug keys and the findings so far: `docs/38-cutscene-parity.md`. Offsets for `scripts/cmp-intro.sh`: 1.18 s before tick ~1400, 1.41 s after.
- Things that bit: recordings carry controls, not positions, so a unit's speed (the move animation's ground speed) decides where it ends; `unit_set_seat unit "alert"` sets the posture, not
  the recording's flags; objects are drawn whole or not at all; `scripttrace 2` and `find /` are slow, run them in the background; worktree `assets/` are symlinks to the main checkout (do not overwrite
  through them); `zsh` does not split string variables (use arrays).
- Keep the user's request literal ("take the camera info from the reverse-engineered code" meant the executable, not an emulator session). When two sources disagree, say so and ask which wins.
