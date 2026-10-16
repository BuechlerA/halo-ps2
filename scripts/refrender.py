#!/usr/bin/env python3
"""Independent software reference for a BSPS section: decodes the baked batches back to world
triangles, projects them with the engine's camera maths (render/camera.c) and paints them far to
near with a simple normal-based shade. Used to tell data problems from VU1/GS problems:
compare with a framebuffer dump of the same pose.

    python3 -I scripts/refrender.py assets/hp2/a10.hp2 0  X Y Z YAW_DEG PITCH_DEG  out.png [--clusters]

Development aid only (needs Pillow).
"""
import math
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from PIL import Image, ImageDraw  # noqa: E402
from halo2ps2 import bspconv, hp2  # noqa: E402

W, H = 512, 448
DEPTH = 0.0


def basis(yaw, pitch):
    cy, sy, cp, sp = math.cos(yaw), math.sin(yaw), math.cos(pitch), math.sin(pitch)
    return (sy, -cy, 0.0), (-sp * cy, -sp * sy, cp), (cp * cy, cp * sy, sp)


def main() -> int:
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    path, idx = Path(args[0]), int(args[1])
    x, y, z, yaw, pitch = (float(v) for v in args[2:7])
    out = args[7]
    global DEPTH
    for a in sys.argv[1:]:
        if a.startswith('--depth='):
            DEPTH = float(a.split('=')[1])
    near, hfov, aspect = 0.05, math.radians(70), 4 / 3
    pk = hp2.parse(path.read_bytes())
    sec = bspconv.parse_section(pk.read(bspconv.section_tag(idx)))
    r, u, f = basis(math.radians(yaw), math.radians(pitch))
    kx = 1 / math.tan(hfov / 2)
    ky = kx * aspect
    pos = (x, y, z)
    dot = lambda a, b: a[0] * b[0] + a[1] * b[1] + a[2] * b[2]  # noqa: E731
    tris = []
    ONLY = next((a.split('=')[1] for a in sys.argv if a.startswith('--batch=')), None)
    lo, hi = (int(v) for v in (ONLY.split('-') if '-' in ONLY else (ONLY, ONLY))) if ONLY else (0, 1 << 30)
    for bi, b in enumerate(sec.batches):
        if not lo <= bi <= hi:
            continue
        vs = bspconv.decode_batch(sec, b)
        for t in range(b["triangles"]):
            tri = [vs[3 * t + k]["pos"] for k in range(3)]
            proj = []
            ok = True
            for p in tri:
                d = (p[0] - x, p[1] - y, p[2] - z)
                w = dot(f, d)
                if w <= near:
                    ok = False
                    break
                proj.append((W / 2 + W / 2 * kx * dot(r, d) / w, H / 2 - H / 2 * ky * dot(u, d) / w, w))
            if not ok:
                continue
            e1 = [tri[1][k] - tri[0][k] for k in range(3)]
            e2 = [tri[2][k] - tri[0][k] for k in range(3)]
            n = (e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0])
            ln = math.sqrt(dot(n, n)) or 1.0
            shade = int(60 + 150 * abs(dot(n, f)) / ln)
            if DEPTH:
                wc = sum(p[2] for p in proj) / 3
                shade = max(0, min(255, int(128 * (1 - (wc - 0.5) / (DEPTH - 0.5)))))
            tris.append((max(p[2] for p in proj), proj, shade))
    tris.sort(key=lambda t: -t[0])
    img = Image.new("RGB", (W, H), (20, 24, 40))
    d = ImageDraw.Draw(img)
    for _, proj, shade in tris:
        d.polygon([(p[0], p[1]) for p in proj], fill=(shade, shade, shade))
    img.save(out)
    print(f"{out}: {len(tris)} triangles in front of the camera")
    return 0


if __name__ == "__main__":
    sys.exit(main())
