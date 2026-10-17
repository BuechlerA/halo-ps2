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
- Phase 1/2 world: `scripts/build-world.sh a10 a30 b40` converts maps to `assets/hp2/` (BSP, textures, objects: `.hp2`, `.tex.hp2`, `.obj.hp2`); `scripts/run-headless.sh -c halo.cfg`
  runs the ELF in PCSX2 against a config (`docs/10-m1-hardware-check.md`); `scripts/profile-m1.sh` runs the M1
  fly-throughs; `fbdump <frame>` in the config + `scripts/fb2png.py` is how rendering is checked without a screen.
- Xbox reference: `scripts/xemu-setup.sh` then `scripts/xemu-run.sh` boot the ISO in xemu for parity checks (`docs/14-xbox-reference.md`, ADR-030); the
  MCPX/BIOS/HDD files are user-supplied (`~/Developer/xbox-bios/`) and never committed.
- Game data lives only in `assets/` and `*.iso` (both gitignored). Never commit it,
  never copy tag data into docs or tests.

## Conventions
- C11, no `double`, no heap after level load, 16-byte aligned DMA structs, VU1 data
  layouts documented next to the `.vsm` that consumes them.
- Every new decision gets an ADR entry. Milestones are gated on the PCSX2 interpreter; real-console checks are
  deferred to Phase 6 (ADR-022) — do not run or request hardware tests before then unless the user asks. PCSX2
  does not model GS fill/DMA: treat GS cost as unmeasured and keep EE budgets with margin.
