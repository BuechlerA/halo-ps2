#!/usr/bin/env python3
"""Turn "[fb]" framebuffer dumps in a PCSX2 emulog / TTY capture into PNG files.

    python3 -I scripts/fb2png.py emu.log OUT_PREFIX     -> OUT_PREFIX-<frame>.png per dump

Dumps come from engine/src/platform/fbdump.c (PSMCT16S: 5-5-5-1 little-endian). Development aid
only: needs Pillow, which the pipeline tools themselves never import.
"""
import re
import sys

from PIL import Image


def decode16(rows: dict[int, bytes], w: int, h: int) -> Image.Image:
    img = Image.new("RGB", (w, h))
    px = img.load()
    for y in range(h):
        row = rows.get(y)
        if row is None:
            continue
        for x in range(w):
            v = row[2 * x] | (row[2 * x + 1] << 8)
            r, g, b = v & 31, (v >> 5) & 31, (v >> 10) & 31
            px[x, y] = ((r << 3) | (r >> 2), (g << 3) | (g >> 2), (b << 3) | (b >> 2))
    return img


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    log, prefix = sys.argv[1], sys.argv[2]
    cur = None
    rows: dict[int, bytes] = {}
    written = 0
    for line in open(log, errors="replace"):
        line = line.rstrip("\n")
        m = re.search(r"\[fb\] begin frame=(\d+) w=(\d+) h=(\d+) psm=(\d+)", line)
        if m:
            cur = tuple(int(g) for g in m.groups())
            rows = {}
            continue
        m = re.search(r"\[fb\] (\d+) ([0-9a-f]+)$", line)
        if m and cur:
            rows[int(m.group(1))] = bytes.fromhex(m.group(2))
            continue
        if "[fb] end" in line and cur:
            frame, w, h, psm = cur
            decode16(rows, w, h).save(f"{prefix}-{frame}.png")
            print(f"{prefix}-{frame}.png  ({len(rows)}/{h} rows)")
            written += 1
            cur = None
    return 0 if written else 1


if __name__ == "__main__":
    sys.exit(main())
