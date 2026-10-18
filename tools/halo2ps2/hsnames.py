"""Script built-in function names, read from the Xbox executable.

Cache maps store only a function *index*. `default.xbe` holds an array of pointers
(index -> definition); each definition is: return type, name, parse fn, evaluate fn,
description, usage, then a variable argument list. Definitions are not laid out in
index order, so the pointer array is authoritative. Names are read at report time and
never committed (CLAUDE.md: no game data).
"""
from __future__ import annotations

import struct
from pathlib import Path

MAX_RET = 0x40          # return-type enum is small
CODE_LIMIT = 0x200000   # parse/evaluate handlers live in .text, below the data sections


def load_names(xbe: Path) -> list[str]:
    b = xbe.read_bytes()
    base = struct.unpack_from("<I", b, 0x104)[0]
    nsec, secaddr = struct.unpack_from("<II", b, 0x11C)
    secs = [struct.unpack_from("<4I", b, secaddr - base + 56 * i + 4) for i in range(nsec)]

    def v2f(v: int) -> int | None:
        for va, vsize, raw, rsize in secs:
            if va <= v < va + vsize and v - va < rsize:
                return raw + v - va
        return None

    def name_at(v: int) -> str | None:
        """Name of the definition starting at virtual address v, else None."""
        o = v2f(v)
        if o is None or o + 16 > len(b):
            return None
        ret, name_p, parse_p, eval_p = struct.unpack_from("<4I", b, o)
        no = v2f(name_p)
        if (ret > MAX_RET or no is None or not (parse_p or eval_p)
                or parse_p >= CODE_LIMIT or eval_p >= CODE_LIMIT):
            return None
        s = b[no:b.index(b"\0", no)]
        return s.decode("ascii") if s and all(32 <= c < 127 for c in s) else None

    # Find `begin_random`'s definition, then the pointer array that references it.
    needle = b"begin_random\0"
    sv = next((va + i - raw for va, vsize, raw, rsize in secs
               if (i := b.find(needle, raw, raw + rsize)) >= 0), None)
    namep = struct.pack("<I", sv) if sv is not None else b""
    at = b.find(namep) if namep else -1
    if at < 0:
        raise ValueError("script function table not found in xbe")
    def_va = next(va + (at - 4) - raw for va, vsize, raw, rsize in secs if raw <= at - 4 < raw + rsize)
    arr = b.find(struct.pack("<I", def_va))
    if arr < 0:
        raise ValueError("script function pointer array not found in xbe")
    arr -= 4                                    # `begin` is entry 0, `begin_random` entry 1
    names: list[str] = []
    while arr + 4 * len(names) < len(b) - 4 and \
            (n := name_at(struct.unpack_from("<I", b, arr + 4 * len(names))[0])) is not None:
        names.append(n)
    if names[:2] != ["begin", "begin_random"]:
        raise ValueError("unexpected script function table layout")
    return names
