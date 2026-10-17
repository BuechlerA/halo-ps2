#!/usr/bin/env bash
# Gate M1 / M2 measurement: noclip fly-through of a30 and b40 (plus an a10 interior loop), printing the
# engine's per-second [prof] lines, the dropped-frame percentage and the memory report.
#
#   scripts/profile-m1.sh [--interpreter] [--maps "a30 b40 a10"]
#
# On PCSX2 this is the ADR-015 stand-in; the real gate runs the same configs on the console:
#   ps2client -h <ip> execee host:engine/halo-ps2.elf   (with halo.cfg = scripts/paths/<map>.cfg, see docs/10-m1-hardware-check.md)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
INTERP=""
MAPS="a30 b40 a10 a10obj a10obj-stress a10game"
while [ $# -gt 0 ]; do
	case "$1" in
		--interpreter) INTERP="--interpreter" ;;
		--maps) MAPS="$2"; shift ;;
		*) echo "unknown option $1" >&2; exit 2 ;;
	esac
	shift
done
for m in $MAPS; do
	cfg="$ROOT/scripts/paths/$m.cfg"
	[ -f "$cfg" ] || { echo "no $cfg" >&2; exit 1; }
	echo "== $m ${INTERP:-(recompiler)}"
	"$ROOT/scripts/run-headless.sh" $INTERP -t 400 -c "$cfg" | grep -E '\[prof\] frames=[0-9]*[05]0 |\[mem\]|\[done\]|\[err\]|\[warn\]' | sed -E 's/^\[ *[0-9.]+\] //' | tail -8
done
