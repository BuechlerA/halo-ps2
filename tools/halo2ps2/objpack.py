#!/usr/bin/env python3
"""Build a map's object pack (assets/hp2/<map>.obj.hp2): SCEN (placements, definitions, player, lighting),
MODL + OTEX (models and their textures), ANIM (animation graphs) and MOUT (lip-sync data of the dialogue lines), restricted to what the scenario places
(plus the player's first-person set).

    python3 -I tools/halo2ps2/objpack.py assets/decompressed/a10.map -o assets/hp2/a10.obj.hp2
    python3 -I tools/halo2ps2/objpack.py --info assets/hp2/a10.obj.hp2
"""
from __future__ import annotations

import argparse
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from halo2ps2 import aiconv, animconv, ansmconv, chgconv, devconv, cinconv, fxconv, hp2, modelconv, mouthconv, scenconv, skyconv, unitconv, weapconv  # noqa: E402


def build(map_path: Path) -> tuple[bytes, dict]:
    t0 = time.time()
    extra = aiconv.referenced_object_tags(map_path)          # AI units and their weapons: SCEN definitions for run-time spawning
    objs = scenconv.collect(map_path, extra)
    scen = scenconv.build_section(map_path, extra=extra)
    sky = skyconv.read_sky(map_path)
    model_tags = set(objs.model_tags)
    anim_tags = set(objs.anim_tags)
    # Phase 3 roots: models and animation graphs that weapons, units and effects reference but no placement mentions
    for mod in (unitconv, weapconv, fxconv, aiconv, devconv):
        model_tags |= set(mod.referenced_models(map_path))
    for mod in (unitconv, weapconv, ansmconv, aiconv, devconv, cinconv):
        anim_tags |= set(mod.referenced_anims(map_path))
    if sky:
        model_tags |= {t for t in (sky["model_tag"], sky["model_tag2"]) if t}      # the sky models are drawn by the engine's sky pass
    model_sections = modelconv.build_sections(map_path, sorted(model_tags))
    anim = animconv.build_section(map_path, sorted(anim_tags))
    sections = [("SCEN", scen), ("SKY0", skyconv.build_section(map_path)), ("CCHG", chgconv.build_section(map_path, model_tags))] + list(model_sections) + [("ANIM", anim), ("MOUT", mouthconv.build_section(map_path))]
    info = {"models": len(model_tags), "graphs": len(anim_tags), "seconds": time.time() - t0,
            "sizes": {tag: len(blob) for tag, blob in sections}, "counts": getattr(objs, "counts", {})}
    return hp2.build(map_path.stem, sections), info


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("input", type=Path)
    ap.add_argument("-o", "--out", type=Path)
    ap.add_argument("--info", action="store_true", help="input is an object .hp2: print its sections")
    args = ap.parse_args(argv)
    if args.info:
        pk = hp2.parse(args.input.read_bytes())
        for s in pk.sections:
            print(f"{s.tag:<5} {s.raw_size / 2**20:7.2f} MiB raw  {s.stored_size / 2**20:7.2f} MiB stored")
        return 0
    if not args.out:
        ap.error("-o is required")
    data, info = build(args.input)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_bytes(data)
    sizes = ", ".join(f"{k} {v / 2**20:.2f} MiB" for k, v in info["sizes"].items())
    print(f"{args.input.stem}: {info['models']} models, {info['graphs']} animation graphs; {sizes}; {info['seconds']:.1f} s")
    print(f"wrote {args.out} ({len(data) / 2**20:.2f} MiB)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
