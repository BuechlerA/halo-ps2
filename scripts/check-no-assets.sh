#!/usr/bin/env bash
# Fail if game data is tracked or staged (spec R7: the repository stays asset-free).
set -euo pipefail
cd "$(dirname "$0")/.."
bad=$(git ls-files -- 'assets/*' '*.iso' '*.xiso' '*.xbe' '*.map' '*.bik' '*.bitmap' '*.hp2' ':!:docs/reports/*' || true)
if [ -n "$bad" ]; then
	echo "game data or build output is tracked:" >&2
	echo "$bad" >&2
	exit 1
fi
echo "ok: no tracked game data"
