#!/usr/bin/env bash
# Phase 0: header summary of every extracted map, written to docs/reports/ (no game data).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
MAPS="$ROOT/assets/xbox/maps"
[ -d "$MAPS" ] || { echo "run scripts/extract-iso.sh first" >&2; exit 1; }
mkdir -p "$ROOT/docs/reports"
python3 -I "$ROOT/tools/halo2ps2/mapinfo.py" info "$MAPS"/*.map | tee "$ROOT/docs/reports/map-headers.txt"
