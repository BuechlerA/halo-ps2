#!/usr/bin/env bash
# Run the engine ELF headless in PCSX2 for N seconds and print the engine's [prof] lines.
#
#   scripts/profile-pcsx2.sh [--interpreter] [-t SECONDS] [ELF]
#
# --interpreter turns off the EE/IOP/VU0/VU1 recompilers (no JIT; much slower, much closer to
# real-hardware instruction cost than the default recompiler). It runs against a scratch copy of
# your PCSX2.ini, so your own configuration is never modified. Needs the user's PCSX2 data dir
# (BIOS configured) at ~/Library/Application Support/PCSX2 (override with PCSX2_DATA).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PCSX2="${PCSX2:-/Applications/PCSX2.app/Contents/MacOS/PCSX2}"
DATA="${PCSX2_DATA:-$HOME/Library/Application Support/PCSX2}"
SECS=12
INTERP=0
while [ $# -gt 0 ]; do
	case "$1" in
		--interpreter) INTERP=1 ;;
		-t) SECS="$2"; shift ;;
		*) break ;;
	esac
	shift
done
ELF="${1:-$ROOT/engine/halo-ps2.elf}"
[ -f "$ELF" ] || { echo "ELF not found: $ELF (run: make -C engine)" >&2; exit 1; }
[ -f "$DATA/inis/PCSX2.ini" ] || { echo "no PCSX2.ini under $DATA" >&2; exit 1; }

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
mkdir -p "$WORK/data/PCSX2/inis"
cp "$DATA/inis/PCSX2.ini" "$WORK/data/PCSX2/inis/PCSX2.ini"
if [ "$INTERP" = 1 ]; then
	sed -i '' -E '/^\[EmuCore\/CPU\/Recompiler\]/,/^\[/ s/^(Enable(EE|IOP|VU0|VU1)) = true/\1 = false/' "$WORK/data/PCSX2/inis/PCSX2.ini"
fi
grep -A5 '^\[EmuCore/CPU/Recompiler\]' "$WORK/data/PCSX2/inis/PCSX2.ini" | tr '\n' ' ' | sed 's/^/config: /'; echo

"$PCSX2" -nogui -batch -datapath "$WORK/data" -logfile "$WORK/emu.log" -elf "$ELF" -- >/dev/null 2>&1 &
PID=$!
sleep "$SECS"
kill "$PID" 2>/dev/null || true
wait "$PID" 2>/dev/null || true
grep -E 'halo-ps2|\[prof\]|\[video\]|\[vu1\]' "$WORK/emu.log" || { echo "no engine output; tail of log:" >&2; cp "$WORK/emu.log" "${PROFILE_LOG:-/dev/null}"; tail -20 "$WORK/emu.log" >&2; exit 1; }
