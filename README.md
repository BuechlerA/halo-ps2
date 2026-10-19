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
| [docs/07-hp2-format.md](docs/07-hp2-format.md), [08-bsp-section.md](docs/08-bsp-section.md), [09-tex-section.md](docs/09-tex-section.md) | Pack container, BSP geometry and texture/lightmap sections |
| [docs/11-model-section.md](docs/11-model-section.md), [12-anim-section.md](docs/12-anim-section.md), [13-scen-section.md](docs/13-scen-section.md) | Object models, animations, scenario placements and lighting |
| [docs/14-phase3-plan.md](docs/14-phase3-plan.md) and [15](docs/15-coll-section.md) [16](docs/16-cmod-section.md) [17](docs/17-unit-section.md) [18](docs/18-weap-section.md) [19](docs/19-ansm-section.md) [20](docs/20-hud-section.md) [21](docs/21-snd-section.md) [22](docs/22-fx-section.md) | Phase 3 workstreams: collision, object hit shapes, units, weapons, animation state tables, HUD, sound, effects |
| [docs/23-phase4-plan.md](docs/23-phase4-plan.md) and [24](docs/24-script-section.md) [25](docs/25-ai-section.md) [26](docs/26-path-section.md) [27](docs/27-dev-section.md) [28](docs/28-cine-section.md) [29](docs/29-strm-section.md) [30](docs/30-mission-host.md) [31](docs/31-units.md) [32](docs/32-ai-runtime.md) [33](docs/33-host-objects.md) [34](docs/34-host-misc.md) | Phase 4 workstreams: scripts, AI data, pathfinding, devices, cinematics, streamed audio, mission host, units, AI runtime, script built-ins |
| [docs/35-xbox-reference.md](docs/35-xbox-reference.md) | xemu setup for Xbox parity checks (boot files are user-supplied) |
| [docs/10-m1-hardware-check.md](docs/10-m1-hardware-check.md) | Console procedure (deferred to Phase 6) and the `halo.cfg` reference |

## Layout
```
engine/   EE C sources + VU1 microprograms → halo-ps2.elf
iop/      IOP modules — none needed so far; gameplay and streamed audio use ps2sdk's audsrv.irx (ADR-030, ADR-048)
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
Phases 0–3 done (gates M0–M3 met on PCSX2; console verification deferred to Phase 6, ADR-022). Next: Phase 4 (AI, scripting, mission 1). See the roadmap for the gate checklist.

## PCSX2 notes
`printf` from the EE only shows in PCSX2's log (`~/Library/Application Support/PCSX2/logs/emulog.txt`)
when **EE Console** logging is enabled in PCSX2's log settings (it is on this machine). stdout
is set unbuffered in `main.c` so lines appear immediately.
