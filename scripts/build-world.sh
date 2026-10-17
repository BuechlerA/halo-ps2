#!/usr/bin/env bash
# Convert the Phase 1 world data of one or more maps: assets/hp2/<map>.hp2 (BSP geometry, BSPS v2)
# assets/hp2/<map>.tex.hp2 (TEXS + LMn), assets/hp2/<map>.obj.hp2 (SCEN, MODL, OTEX, ANIM: objects) and the Phase 3 packs
# <map>.{coll,cmod,unit,weap,ansm,hud,snd,fx}.hp2 (docs/14-phase3-plan.md) and the Phase 4 packs <map>.{script,ai,path,dev,cin,strm}.hp2
# (docs/23-phase4-plan.md; strm is ~120 MB per map and takes ~10 s with a cold cache). Needs assets/decompressed/<map>.map (tools/halo2ps2/mapinfo.py).
#
#   scripts/build-world.sh a10 a30 b40 c10
#   LM_GAIN=2.0 scripts/build-world.sh a10     # lightmap luminance gain (tunable at M1; default 1.5)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
[ $# -ge 1 ] || { echo "usage: $0 map [map...]" >&2; exit 2; }
mkdir -p "$ROOT/assets/hp2"
for m in "$@"; do
	src="$ROOT/assets/decompressed/$m.map"
	[ -f "$src" ] || { echo "missing $src (decompress the map first)" >&2; exit 1; }
	python3 -I "$ROOT/tools/halo2ps2/bspconv.py" "$src" -o "$ROOT/assets/hp2/$m.hp2"
	python3 -I "$ROOT/tools/halo2ps2/objpack.py" "$src" -o "$ROOT/assets/hp2/$m.obj.hp2" || echo "(no object pack for $m)" >&2
	for kind in collconv:coll cmodconv:cmod unitconv:unit weapconv:weap ansmconv:ansm hudconv:hud soundconv:snd fxconv:fx scriptconv:script aiconv:ai pathconv:path devconv:dev cinconv:cin; do
		python3 -I "$ROOT/tools/halo2ps2/${kind%%:*}.py" "$src" -o "$ROOT/assets/hp2/$m.${kind##*:}.hp2" || echo "(no ${kind##*:} pack for $m)" >&2
	done
	python3 -I "$ROOT/tools/halo2ps2/streamconv.py" "$src" -o "$ROOT/assets/hp2/$m.strm.hp2" --cache "$ROOT/assets/hp2/.strmcache" || echo "(no strm pack for $m)" >&2
	python3 -I "$ROOT/tools/halo2ps2/texconv.py" "$src" -o "$ROOT/assets/hp2/$m.tex.hp2" --lm-gain "${LM_GAIN:-1.5}"
done
