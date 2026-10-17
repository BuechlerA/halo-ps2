"""Round-trip test for the tag index parser with a synthetic two-tag region and a
synthetic decompressed cache file (no game data needed)."""
import struct
import sys
import tempfile
from pathlib import Path

sys.path[:0] = [str(Path(__file__).resolve().parents[1]), str(Path(__file__).resolve().parent)]
from halo2ps2 import mapinfo, tagindex  # noqa: E402
from test_mapinfo import make_header  # noqa: E402

BASE = tagindex.XBOX_TAG_BASE
TAGS = [("scnr", "levels/test/test", 0x40),
        ("sbsp", "levels/test/test_bsp0", 0x10)]
VERTS = bytes(range(64))          # two fake 32-byte vertices
INDICES = struct.pack("<6H", 0, 1, 2, 0xFFFF, 1, 2)


def build_region() -> bytes:
    return tagindex.TagIndex.build(TAGS, scenario_index=0, model_parts=1,
                                   vertex_bytes=VERTS, index_bytes=INDICES, checksum=0x1234)


def check(idx: tagindex.TagIndex, tag_data_offset: int) -> None:
    h = idx.header
    assert h.magic == tagindex.TAGS_MAGIC and h.tag_count == 2
    assert h.tag_array_address == BASE + tagindex.INDEX_HEADER_SIZE
    assert h.checksum == 0x1234 and h.model_part_count == h.model_part_count2 == 1
    assert [e.group for e in idx.entries] == ["scnr", "sbsp"]
    assert [e.path for e in idx.entries] == [t[1] for t in TAGS]
    assert [e.index for e in idx.entries] == [0, 1]
    assert idx.scenario.group == "scnr" and idx.scenario.path == TAGS[0][1]
    assert idx.entries[0].parent is None and idx.entries[1].indexed == 0
    va, vs = idx.vertex_region
    ia, isz = idx.index_region
    assert vs == len(VERTS) and isz == len(INDICES)
    assert ia == va + vs and idx.entries[0].data_address == ia + isz
    assert idx.addr_to_off(BASE) == tag_data_offset
    assert idx.off_to_addr(idx.addr_to_off(va)) == va
    assert idx.problems == []
    s = idx.summary("test")
    assert s["groups"] == {"sbsp": 1, "scnr": 1} and s["structure_bsps"] == [TAGS[1][1]]
    assert s["tag_struct_bytes"] == 0x40 and idx.entries[1].data_address == 0


def test_region_roundtrip():
    region = build_region()
    idx = tagindex.TagIndex.parse_region(region, tag_data_offset=0x1000)
    check(idx, 0x1000)
    # entries and header serialise back to the same bytes
    rebuilt = idx.header.pack() + b"".join(e.pack() for e in idx.entries)
    assert rebuilt == region[:len(rebuilt)]


def test_file_roundtrip_and_report():
    region = build_region()
    tag_off = 0x3000
    body = bytes(tag_off - mapinfo.HEADER_SIZE) + region
    hdr = make_header(mapinfo.HEADER_SIZE + len(body), tag_off, len(region))
    with tempfile.TemporaryDirectory() as td:
        p = Path(td) / "test.map"
        p.write_bytes(hdr + body)
        idx = tagindex.TagIndex.read(p)
        check(idx, tag_off)
        out = Path(td) / "inv.md"
        assert tagindex.main(["report", str(p), "-o", str(out)]) == 0
        text = out.read_text()
        assert "| test | 2 |" in text and "levels/test/test_bsp0" in text


def test_detects_bad_base():
    region = build_region()
    try:
        tagindex.TagIndex.parse_region(region, 0x1000, base=BASE + 0x100)
    except tagindex.TagIndexError:
        pass
    else:
        raise AssertionError("expected a TagIndexError for a wrong base")
    bad = bytearray(region)
    struct.pack_into("<4s", bad, tagindex.INDEX_HEADER_SIZE, b"xxxx")  # scenario group
    idx = tagindex.TagIndex.parse_region(bytes(bad), 0x1000, strict=False)
    assert any("not 'scnr'" in m for m in idx.problems)


if __name__ == "__main__":
    test_region_roundtrip()
    test_file_roundtrip_and_report()
    test_detects_bad_base()
    print("ok")
