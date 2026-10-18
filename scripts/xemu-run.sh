#!/usr/bin/env bash
# Boot the user's Halo ISO in xemu with the project-local config written by scripts/xemu-setup.sh.
#   scripts/xemu-run.sh [iso]        (default: halo.xiso.iso in the main checkout; override with HALO_XISO=)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
XEMU="${XEMU:-/Applications/xemu.app/Contents/MacOS/xemu}"
CFG="$ROOT/assets/xemu/xemu.toml"
# The ISO lives in the main checkout's root; worktrees do not have a copy.
MAIN="$(cd "$(git -C "$ROOT" rev-parse --path-format=absolute --git-common-dir)/.." && pwd)"
ISO="${1:-${HALO_XISO:-}}"
if [ -z "$ISO" ]; then
  for c in "$ROOT/halo.xiso.iso" "$MAIN/halo.xiso.iso"; do [ -f "$c" ] && ISO="$c" && break; done
fi
[ -x "$XEMU" ] || { echo "xemu not found at $XEMU" >&2; exit 1; }
[ -f "$CFG" ] || { echo "no $CFG (run: scripts/xemu-setup.sh)" >&2; exit 1; }
[ -n "$ISO" ] && [ -f "$ISO" ] || { echo "Halo ISO not found (pass a path or set HALO_XISO)" >&2; exit 1; }
exec "$XEMU" -config_path "$CFG" -dvd_path "$ISO"
