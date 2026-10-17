#!/usr/bin/env python3
"""List camera positions that lie inside BSP clusters (for halo.cfg `camera` / `pathkey` lines).

    python3 -I scripts/campos.py assets/hp2/a10.hp2 [bsp_index] [step]

Samples a grid over the BSP bounds, asks the converter's find_cluster which samples are in empty
space, and prints for the biggest clusters the sample closest to the cluster's mean (head height
is not known: this is just a point in the air inside the cluster).
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from halo2ps2 import bspconv, hp2  # noqa: E402


def main() -> int:
    path = Path(sys.argv[1])
    idx = int(sys.argv[2]) if len(sys.argv) > 2 else 0
    step = float(sys.argv[3]) if len(sys.argv) > 3 else 2.0
    pk = hp2.parse(path.read_bytes())
    sec = bspconv.parse_section(pk.read(bspconv.section_tag(idx)))
    b = sec.bounds
    print(f"# bounds {[round(v, 1) for v in b]}", file=sys.stderr)
    pts = {}
    x = b[0]
    while x < b[3]:
        y = b[1]
        while y < b[4]:
            z = b[2]
            while z < b[5]:
                c = bspconv.find_cluster(sec, (x, y, z))
                if c >= 0:
                    pts.setdefault(c, []).append((x, y, z))
                z += step
            y += step
        x += step
    for c, p in sorted(pts.items(), key=lambda kv: -len(kv[1]))[:12]:
        mx = [sum(q[k] for q in p) / len(p) for k in range(3)]
        best = min(p, key=lambda q: sum((q[k] - mx[k]) ** 2 for k in range(3)))
        print(f"cluster {c:3d}  samples {len(p):6d}  pos {best[0]:.1f} {best[1]:.1f} {best[2]:.1f}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
