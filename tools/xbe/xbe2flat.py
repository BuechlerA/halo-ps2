#!/usr/bin/env python3
"""default.xbe -> a flat memory image (file offset = virtual address - 0x10000) for Ghidra's raw binary loader and for ndisasm.

    python3 -I tools/xbe/xbe2flat.py assets/xbox/default.xbe OUT/halo_flat.bin

Development aid for docs/36-xbe-analysis.md; reads the user's own XBE, writes nothing into the repository."""
import struct
import sys

BASE = 0x10000


def main() -> int:
    d = open(sys.argv[1], "rb").read()
    if d[:4] != b"XBEH":
        raise SystemExit("not an XBE")
    nsec, = struct.unpack_from("<I", d, 0x11C)
    secaddr, = struct.unpack_from("<I", d, 0x120)
    secs = [struct.unpack_from("<IIIIII", d, secaddr - BASE + i * 56) for i in range(nsec)]
    img = bytearray(max(va + vsz for _f, va, vsz, _r, _s, _n in secs) - BASE)
    for _f, va, _vsz, raw, rsz, _n in secs:
        img[va - BASE:va - BASE + rsz] = d[raw:raw + rsz]
    open(sys.argv[2], "wb").write(img)
    print(f"{len(img):#x} bytes, {nsec} sections")
    return 0


if __name__ == "__main__":
    sys.exit(main())
