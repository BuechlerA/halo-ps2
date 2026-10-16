#!/usr/bin/env bash
# Run the engine headless in PCSX2 against a scratch directory holding a copy of the ELF, an
# optional halo.cfg (see engine/src/core/config.h) and a symlink to assets/, until the engine
# prints "[done]" or the timeout expires.
#
#   scripts/run-headless.sh [--interpreter] [-t SECONDS] [-c halo.cfg] [-l emu.log] [ELF]
#
# Prints the engine's own lines ([prof] [video] [vu1] [bsp] [done] ...); the whole PCSX2 log goes
# to -l (default: discarded). Framebuffer dumps in that log become PNGs with scripts/fb2png.py.
# Runs against a scratch copy of your PCSX2.ini (never modifies it); needs the BIOS configured.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PCSX2="${PCSX2:-/Applications/PCSX2.app/Contents/MacOS/PCSX2}"
DATA="${PCSX2_DATA:-$HOME/Library/Application Support/PCSX2}"
SECS=60
INTERP=0
CFG=""
LOG_OUT=""
while [ $# -gt 0 ]; do
	case "$1" in
		--interpreter) INTERP=1 ;;
		-t) SECS="$2"; shift ;;
		-c) CFG="$2"; shift ;;
		-l) LOG_OUT="$2"; shift ;;
		*) break ;;
	esac
	shift
done
ELF="${1:-$ROOT/engine/halo-ps2.elf}"
[ -f "$ELF" ] || { echo "ELF not found: $ELF (run: make -C engine)" >&2; exit 1; }
[ -f "$DATA/inis/PCSX2.ini" ] || { echo "no PCSX2.ini under $DATA" >&2; exit 1; }

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
mkdir -p "$WORK/data/PCSX2/inis" "$WORK/run"
cp "$DATA/inis/PCSX2.ini" "$WORK/data/PCSX2/inis/PCSX2.ini"
sed -i '' -E 's/^HostFs = false/HostFs = true/' "$WORK/data/PCSX2/inis/PCSX2.ini"
if [ "$INTERP" = 1 ]; then
	sed -i '' -E '/^\[EmuCore\/CPU\/Recompiler\]/,/^\[/ s/^(Enable(EE|IOP|VU0|VU1)) = true/\1 = false/' "$WORK/data/PCSX2/inis/PCSX2.ini"
fi
cp "$ELF" "$WORK/run/halo-ps2.elf"
[ -n "$CFG" ] && cp "$CFG" "$WORK/run/halo.cfg"
ln -s "$ROOT/assets" "$WORK/run/assets"

"$PCSX2" -nogui -batch -datapath "$WORK/data" -logfile "$WORK/emu.log" -elf "$WORK/run/halo-ps2.elf" -- >/dev/null 2>&1 &
PID=$!
for ((i = 0; i < SECS * 4; i++)); do
	sleep 0.25
	grep -q '\[done\]' "$WORK/emu.log" 2>/dev/null && break
	kill -0 "$PID" 2>/dev/null || break
done
sleep 0.5
kill "$PID" 2>/dev/null || true
for ((i = 0; i < 20; i++)); do kill -0 "$PID" 2>/dev/null || break; sleep 0.25; done
kill -9 "$PID" 2>/dev/null || true
wait "$PID" 2>/dev/null || true
[ -n "$LOG_OUT" ] && cp "$WORK/emu.log" "$LOG_OUT"
grep -E 'halo-ps2|\[(prof|video|vu1|bsp|tex|vis|cfg|done|err|warn|pack|cam|mem|dbg|pad|obj)\]' "$WORK/emu.log" || {
	echo "no engine output; tail of log:" >&2; tail -20 "$WORK/emu.log" >&2; exit 1; }
