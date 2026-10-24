"""menuconv.py: the sprite table of the converter and the enum of engine/src/ui/menudef.c agree, and the real ui.map (when present) converts to a pack the sections of which parse."""
import re
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
from halo2ps2 import hp2, menuconv, texconv  # noqa: E402


def c_names() -> list[str]:
    src = (ROOT / "engine/src/ui/menudef.c").read_text()
    body = src[src.index("k_names[MS_COUNT] = {"):]
    body = body[:body.index("};")]
    return re.findall(r'"([a-z0-9_]+)"', body)


def test_names_match_engine() -> None:
    py = [n for n, _p, _i, _c in menuconv.SPRITES]
    assert py == c_names(), "menuconv.SPRITES and menudef.c k_names differ"
    assert len(set(py)) == len(py) and all(len(n) < 12 for n in py)


def test_real_map() -> None:
    path = ROOT / "assets/decompressed/ui.map"
    if not path.exists():
        print("  (no ui.map: the conversion is not run)")
        return
    pack, hs, _shown = menuconv.build(path)
    pk = hp2.parse(pack)
    menu = pk.read("MENU")
    magic, ver, _z, total, ns, nl, nf, so, lo, fo = struct.unpack_from("<4sHHIIIIIII", menu, 0)
    assert magic == b"MENU" and ver == 1 and total == len(menu) and ns == len(menuconv.SPRITES) and nl == len(menuconv.LISTS) and nf == len(menuconv.FONTS)
    view = texconv.parse_texs(pk.read("HUDT"))
    for i in range(ns):
        rec = menu[so + i * menuconv.SPRITE_REC:so + (i + 1) * menuconv.SPRITE_REC]
        tex, u0, v0, u1, v1, w, h, _fl = struct.unpack_from("<8H", rec, 0)
        name = rec[16:28].split(b"\0")[0].decode()
        assert name == menuconv.SPRITES[i][0] and tex < len(view.textures)
        t = view.textures[tex]
        assert 0 <= u0 < u1 <= t["width"] and 0 <= v0 < v1 <= t["height"] and w > 0 and h > 0, name
    assert len(hs.lists) == nl and all(len(s) for _t, s in hs.lists)


if __name__ == "__main__":
    test_names_match_engine()
    test_real_map()
    print("ok")
