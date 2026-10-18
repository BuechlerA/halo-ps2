#!/usr/bin/env bash
# Intro parity sheets: run the a10 mission headless, dump the framebuffer every N ticks, and tile each dump next to the
# frame of a reference video (the Xbox intro) at the same time. Development aid (needs Pillow, ffmpeg, a reference mp4).
#
#   scripts/cmp-intro.sh REF.mp4 OUTDIR [first_tick last_tick step_ticks [ref_offset_s [extra cfg lines...]]]
#
# PACK / TEX / OBJ env vars replace the cfg's pack paths (trying a re-converted pack without touching assets/hp2/a10.*).
# OUTDIR gets fb-<tick>.png, ref-<tick>.png and cmp-<tick>.png (PS2 left, reference right, same time) and sheet-N.png contact sheets.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
REF="$1"; OUT="$2"; FIRST="${3:-60}"; LAST="${4:-3700}"; STEP="${5:-120}"; OFF="${6:-0}"
shift $(( $# < 6 ? $# : 6 )) || true
mkdir -p "$OUT"
{ sed -e "s/^frames .*/frames $((LAST + 2))/" ${PACK:+-e "s#^pack .*#pack $PACK#"} ${TEX:+-e "s#^tex .*#tex $TEX#"} ${OBJ:+-e "s#^obj .*#obj $OBJ#"} "$ROOT/scripts/paths/a10mission.cfg"
  for ((f = FIRST; f <= LAST; f += STEP)); do echo "fbdump $f"; done
  for l in "$@"; do echo "$l"; done; } > "$OUT/halo.cfg"
SPEED="${SPEED:-8}" "$ROOT/scripts/run-headless.sh" -t 1200 -c "$OUT/halo.cfg" -l "$OUT/emu.log" > "$OUT/out.txt" 2>&1 || true
python3 -I "$ROOT/scripts/fb2png.py" "$OUT/emu.log" "$OUT/fb" > /dev/null
for ((f = FIRST; f <= LAST; f += STEP)); do
	[ -f "$OUT/fb-$f.png" ] || continue
	t=$(python3 -c "print(max(0, $f / 30.0 + $OFF))")
	ffmpeg -v error -y -ss "$t" -i "$REF" -frames:v 1 -vf "scale=512:-1" "$OUT/ref-$f.png"
done
python3 -I - "$OUT" "$FIRST" "$LAST" "$STEP" <<'PY'
import sys, os
from PIL import Image, ImageDraw
out, first, last, step = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4])
ticks = [t for t in range(first, last + 1, step) if os.path.exists(f"{out}/fb-{t}.png") and os.path.exists(f"{out}/ref-{t}.png")]
for t in ticks:
    a, b = Image.open(f"{out}/fb-{t}.png"), Image.open(f"{out}/ref-{t}.png")
    w = Image.new("RGB", (1024, 448)); w.paste(a, (0, 0)); w.paste(b, (512, 80))
    ImageDraw.Draw(w).text((4, 4), f"tick {t} ({t / 30:.1f} s)", fill=(255, 255, 0)); w.save(f"{out}/cmp-{t}.png")
per = 3
for n in range(0, len(ticks), per):
    grp = ticks[n:n + per]
    sh = Image.new("RGB", (1024, 448 * len(grp)))
    for i, t in enumerate(grp):
        sh.paste(Image.open(f"{out}/cmp-{t}.png"), (0, 448 * i))
    sh.save(f"{out}/sheet-{n // per}.png")
print(len(ticks), "comparisons in", out)
PY
