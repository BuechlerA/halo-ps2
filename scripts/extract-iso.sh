#!/usr/bin/env bash
# Extract the user's Xbox ISO into assets/xbox/ (gitignored). Read-only on the ISO.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ISO="${1:-$ROOT/halo.xiso.iso}"
OUT="$ROOT/assets/xbox"
XISO="${EXTRACT_XISO:-$HOME/.local/bin/extract-xiso}"
[ -f "$ISO" ] || { echo "ISO not found: $ISO" >&2; exit 1; }
mkdir -p "$OUT"
cd "$OUT"
"$XISO" -x -d . "$ISO"
echo "extracted to $OUT"
ls -la "$OUT/maps" | head -30
