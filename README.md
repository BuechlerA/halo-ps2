# halo-ps2

A from-scratch PlayStation 2 engine that plays the **Halo: Combat Evolved
campaign** from the user's own Xbox disc, targeting a **locked 30 fps** (60 fps is a deferred stretch goal, ADR-023).

> The repository contains no game assets. The pipeline converts a user-supplied
> Xbox ISO locally; converted data stays in the gitignored `assets/` folder.

## Documents
| | |
|---|---|
| [docs/01-ps2-architecture.md](docs/01-ps2-architecture.md) | Hardware research: EE, VU0/VU1, GS, IOP, SPU2, DVD, toolchain |
| [docs/02-specs.md](docs/02-specs.md) | Goals, hard requirements, frame/memory/VRAM budgets, conversion spec |
| [docs/03-engine-architecture.md](docs/03-engine-architecture.md) | Processor assignment, 30 Hz tick and frame (60 fps stretch), rendering pipeline, subsystems |
| [docs/04-roadmap.md](docs/04-roadmap.md) | Phases 0–6 with milestone gates and the risk register |
| [docs/05-halo-ce-data.md](docs/05-halo-ce-data.md) | Xbox cache file format and what each tag group becomes |
| [docs/06-decisions.md](docs/06-decisions.md) | Architecture decision records |

## Layout
```
engine/   EE C sources + VU1 microprograms → halo-ps2.elf
iop/      IOP modules (streaming, sound) — Phase 3+
tools/    Python pipeline (halo2ps2) + tests
scripts/  extract-iso.sh, inventory-maps.sh, run-pcsx2.sh
docs/     specs, architecture, roadmap, reports/
assets/   gitignored: extracted ISO, decompressed maps, converted packs
```

## Quick start
```bash
make -C engine                        # builds engine/halo-ps2.elf
scripts/run-pcsx2.sh                  # boots it in PCSX2
scripts/extract-iso.sh halo.xiso.iso  # extracts the disc to assets/xbox/
scripts/inventory-maps.sh             # header report → docs/reports/map-headers.txt
python3 -I tools/tests/test_mapinfo.py
```

## Status
Phase 0 (foundation). See the roadmap for the gate checklist.

## PCSX2 notes
`printf` from the EE only shows in PCSX2's log (`~/Library/Application Support/PCSX2/logs/emulog.txt`)
when **EE Console** logging is enabled in PCSX2's log settings (it is on this machine). stdout
is set unbuffered in `main.c` so lines appear immediately.
