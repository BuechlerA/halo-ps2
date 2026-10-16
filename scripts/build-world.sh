#!/usr/bin/env bash
# Convert the Phase 1 world data of one or more maps: assets/hp2/<map>.hp2 (BSP geometry, BSPS v2)
# and assets/hp2/<map>.tex.hp2 (TEXS + LMn). Needs assets/decompressed/<map>.map (tools/halo2ps2/mapinfo.py).
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
	python3 -I "$ROOT/tools/halo2ps2/texconv.py" "$src" -o "$ROOT/assets/hp2/$m.tex.hp2" --lm-gain "${LM_GAIN:-1.5}"
done
