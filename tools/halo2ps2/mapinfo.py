#!/usr/bin/env python3
"""Inspect and decompress Halo: Combat Evolved cache files (.map).

Xbox maps (engine version 5) store a 2048-byte header followed by one zlib
stream holding the rest of the file. See docs/05-halo-ce-data.md.

    python3 tools/halo2ps2/mapinfo.py info assets/xbox/maps/*.map
    python3 tools/halo2ps2/mapinfo.py decompress assets/xbox/maps/a10.map -o assets/decompressed/
"""
from __future__ import annotations

import argparse
import json
import struct
import sys
import zlib
from dataclasses import asdict, dataclass
from pathlib import Path

HEADER_SIZE = 2048
MAGIC_HEAD = 0x68656164  # "head" read as little-endian u32
MAGIC_FOOT = 0x666F6F74  # "foot"
ENGINES = {5: "xbox", 6: "trial", 7: "pc-retail", 609: "custom-edition"}
MAP_TYPES = {0: "singleplayer", 1: "multiplayer", 2: "ui"}
XBOX_TAG_BASE = 0x803A6000
XBOX_TAG_SPACE = 0x1700000


@dataclass
class MapHeader:
    engine: int
    engine_name: str
    decompressed_size: int
    padding_length: int
    tag_data_offset: int
    tag_data_size: int
    name: str
    build: str
    map_type: int
    map_type_name: str
    crc32: int

    @classmethod
    def parse(cls, buf: bytes) -> "MapHeader":
        if len(buf) < HEADER_SIZE:
            raise ValueError("file shorter than a cache header")
        head, engine, size, pad, tag_off, tag_size = struct.unpack_from("<6I", buf, 0)
        (foot,) = struct.unpack_from("<I", buf, 0x7FC)
        if head != MAGIC_HEAD or foot != MAGIC_FOOT:
            raise ValueError("not a Halo cache file (bad head/foot magic)")
        name = buf[0x20:0x40].split(b"\0", 1)[0].decode("ascii", "replace")
        build = buf[0x40:0x60].split(b"\0", 1)[0].decode("ascii", "replace")
        (map_type,) = struct.unpack_from("<H", buf, 0x60)
        (crc,) = struct.unpack_from("<I", buf, 0x64)
        return cls(
            engine=engine,
            engine_name=ENGINES.get(engine, f"unknown({engine})"),
            decompressed_size=size,
            padding_length=pad,
            tag_data_offset=tag_off,
            tag_data_size=tag_size,
            name=name,
            build=build,
            map_type=map_type,
            map_type_name=MAP_TYPES.get(map_type, f"unknown({map_type})"),
            crc32=crc,
        )


def is_compressed(path: Path, hdr: MapHeader) -> bool:
    """Xbox maps are compressed when the on-disk size differs from the header's
    decompressed size. The zlib stream starts right after the header."""
    return hdr.engine == 5 and path.stat().st_size != hdr.decompressed_size


def read_header(path: Path) -> MapHeader:
    with path.open("rb") as f:
        return MapHeader.parse(f.read(HEADER_SIZE))


def decompress(src: Path, dst: Path, chunk: int = 1 << 20) -> int:
    """Write the decompressed cache file to dst; returns bytes written."""
    hdr = read_header(src)
    dst.parent.mkdir(parents=True, exist_ok=True)
    with src.open("rb") as fin, dst.open("wb") as fout:
        header = fin.read(HEADER_SIZE)
        fout.write(header)
        written = len(header)
        if not is_compressed(src, hdr):
            while True:
                b = fin.read(chunk)
                if not b:
                    break
                fout.write(b)
                written += len(b)
            return written
        d = zlib.decompressobj()
        while True:
            b = fin.read(chunk)
            if not b:
                break
            out = d.decompress(b)
            fout.write(out)
            written += len(out)
        tail = d.flush()
        fout.write(tail)
        written += len(tail)
    if written != hdr.decompressed_size:
        print(f"warning: {src.name}: wrote {written} bytes, header says {hdr.decompressed_size}",
              file=sys.stderr)
    return written


def info_rows(paths: list[Path]) -> list[dict]:
    rows = []
    for p in paths:
        hdr = read_header(p)
        row = asdict(hdr)
        row["file"] = str(p)
        row["file_size"] = p.stat().st_size
        row["compressed"] = is_compressed(p, hdr)
        row["ratio"] = round(hdr.decompressed_size / max(1, p.stat().st_size), 2)
        row["tag_space_used_pct"] = round(100.0 * hdr.tag_data_size / XBOX_TAG_SPACE, 1)
        rows.append(row)
    return rows


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    ai = sub.add_parser("info", help="print header summary")
    ai.add_argument("maps", nargs="+", type=Path)
    ai.add_argument("--json", action="store_true")
    ad = sub.add_parser("decompress", help="write decompressed cache files")
    ad.add_argument("maps", nargs="+", type=Path)
    ad.add_argument("-o", "--out", type=Path, required=True, help="output directory")
    args = ap.parse_args(argv)

    if args.cmd == "info":
        rows = info_rows(args.maps)
        if args.json:
            print(json.dumps(rows, indent=2))
            return 0
        print(f"{'map':<16}{'engine':<16}{'type':<14}{'on disk':>10}{'decomp.':>10}{'tags':>9}{'tag%':>7}  build")
        for r in rows:
            print(f"{r['name']:<16}{r['engine_name']:<16}{r['map_type_name']:<14}"
                  f"{r['file_size']/2**20:>9.1f}M{r['decompressed_size']/2**20:>9.1f}M"
                  f"{r['tag_data_size']/2**20:>8.1f}M{r['tag_space_used_pct']:>6.1f}%  {r['build']}")
        return 0

    for p in args.maps:
        n = decompress(p, args.out / p.name)
        print(f"{p.name}: {n/2**20:.1f} MiB -> {args.out / p.name}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
