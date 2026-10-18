#!/usr/bin/env python3
"""Print the compiled scripts of a decompressed map as s-expressions (development aid).

    python3 -I scripts/hsdump.py assets/decompressed/a10.map [name-substring ...]
"""
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from halo2ps2 import scriptconv as sc  # noqa: E402


def main() -> int:
    c = sc.convert_map(Path(sys.argv[1]))
    N, pool = c.nodes, c.strings

    def pstr(off: int) -> str:
        return pool[off:pool.find(b"\0", off)].decode("latin1")

    def show(i: int) -> str:
        if i == sc.NONE16:
            return "()"
        n = N[i]
        if n.kind == sc.K_CONST:
            if n.cls == sc.C_STRING:
                return '"' + pstr(n.data) + '"'
            if n.cls == sc.C_REAL:
                return "%.3g" % struct.unpack("<f", struct.pack("<I", n.data & 0xFFFFFFFF))[0]
            if n.cls == sc.C_BOOL:
                return "true" if n.data else "false"
            if n.cls in (sc.C_SHORT, sc.C_LONG):
                v = n.data & 0xFFFFFFFF
                return str(v - (1 << 32) if v & 0x80000000 else v)
            return f"<{n.vtype}:{n.data}>"
        if n.kind == sc.K_GLOBAL:
            return "$" + (c.globals[n.data].name if n.data < len(c.globals) else str(n.data))
        if n.kind in (sc.K_CALL, sc.K_SCALL):
            name = c.builtins[n.data].name if n.kind == sc.K_CALL else c.scripts[n.data].name
            args, k = [], n.first
            while k != sc.NONE16:
                args.append(show(k))
                k = N[k].next
            return "(" + name + (" " + " ".join(args) if args else "") + ")"
        return f"<{n.kind}>"

    want = sys.argv[2:]
    for s in c.scripts:
        if not want or any(w in s.name for w in want):
            print(f"=== {s.name} type {s.type}\n{show(s.root)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
