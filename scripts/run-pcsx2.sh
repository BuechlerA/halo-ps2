#!/usr/bin/env bash
# Boot the engine ELF in PCSX2 (macOS app bundle). Console output goes to PCSX2's log window.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ELF="${1:-$ROOT/engine/halo-ps2.elf}"
PCSX2="${PCSX2:-/Applications/PCSX2.app/Contents/MacOS/PCSX2}"
[ -f "$ELF" ] || { echo "ELF not found: $ELF (run: make -C engine)" >&2; exit 1; }
exec "$PCSX2" -elf "$ELF" -- 
