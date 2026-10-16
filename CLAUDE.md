# halo-ps2 — working notes for agents

Port of Halo: Combat Evolved (Xbox) to PlayStation 2 at 60 fps. Read `docs/` in
order (01 → 06) before changing architecture; `docs/06-decisions.md` is the ADR log.

## Build / run
- Toolchain: `~/ps2dev` (EE gcc 15.2, `dvp-as`, ps2sdk, gsKit). The Makefile sets
  `PS2DEV`/`PS2SDK`/`PATH` itself; no shell setup needed.
- `make -C engine` → `engine/halo-ps2.elf`. `make -C engine clean`.
- `scripts/run-pcsx2.sh` boots the ELF in PCSX2. Hardware: `ps2client -h <ip> execee host:engine/halo-ps2.elf`.
- Tools: `for t in tools/tests/test_*.py; do python3 -I $t; done` (no game data needed); engine host tests: `make -C engine/tests`.
- Frame pacing in PCSX2 without hardware: `scripts/profile-pcsx2.sh --interpreter` (ADR-015).
- Game data lives only in `assets/` and `*.iso` (both gitignored). Never commit it,
  never copy tag data into docs or tests.

## Conventions
- C11, no `double`, no heap after level load, 16-byte aligned DMA structs, VU1 data
  layouts documented next to the `.vsm` that consumes them.
- Every new decision gets an ADR entry; every milestone needs a hardware check.
