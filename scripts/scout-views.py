#!/usr/bin/env python3
"""Find heavy viewpoints for the M1 fly-through: runs the engine over candidate poses (cluster
sample points from campos.py x several yaws), reads the triangle counts from the profiler and
prints the heaviest as `pathkey` lines.

    python3 -I scripts/scout-views.py a30 [bsp_index] [top_n]

Development aid; needs PCSX2 and the converted packs in assets/hp2.
"""
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def candidates(m: str, bsp: int):
    out = subprocess.run([sys.executable, "-I", str(ROOT / "scripts/campos.py"), str(ROOT / f"assets/hp2/{m}.hp2"), str(bsp), "6"],
                         capture_output=True, text=True, check=True).stdout
    pts = [tuple(float(v) for v in re.search(r"pos (\S+) (\S+) (\S+)", ln).groups()) for ln in out.splitlines() if "pos" in ln]
    return [(p, yaw) for p in pts[:6] for yaw in (0, 90, 180, 270)]


def main() -> int:
    m = sys.argv[1]
    bsp = int(sys.argv[2]) if len(sys.argv) > 2 else 0
    top = int(sys.argv[3]) if len(sys.argv) > 3 else 8
    cands = candidates(m, bsp)[:24]
    cfg = [f"pack assets/hp2/{m}.hp2", f"tex assets/hp2/{m}.tex.hp2", f"bsp {bsp}", "overlay 0", f"frames {60 * len(cands) + 1}"]
    for i, ((x, y, z), yaw) in enumerate(cands):
        cfg.append(f"pathkey {i} {x} {y} {z} {yaw} 0")
    cfg.append(f"pathkey {len(cands)} {cands[0][0][0]} {cands[0][0][1]} {cands[0][0][2]} 0 0")
    with tempfile.NamedTemporaryFile("w", suffix=".cfg", delete=False) as f:
        f.write("\n".join(cfg) + "\n")
    res = subprocess.run([str(ROOT / "scripts/run-headless.sh"), "-t", "600", "-c", f.name], capture_output=True, text=True).stdout
    tris = {}
    for ln in res.splitlines():
        mm = re.search(r"\[prof\] frames=(\d+) .*tris=(\d+) batches=(\d+)", ln)
        if mm and int(mm.group(1)) % 60 == 0:
            tris[int(mm.group(1)) // 60 - 1] = (int(mm.group(2)), int(mm.group(3)))
    best = sorted(tris.items(), key=lambda kv: -kv[1][0])[:top]
    for i, (t, b) in best:
        (x, y, z), yaw = cands[i]
        print(f"# tris {t} batches {b}\npathkey_candidate {x} {y} {z} {yaw} 0")
    return 0


if __name__ == "__main__":
    sys.exit(main())
