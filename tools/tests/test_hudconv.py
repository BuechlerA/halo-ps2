"""HUD conversion on a synthetic map (no game data needed): bitmap sequences and sprites, anchors and scales, colours,
unit / weapon / grenade HUD layouts, hud_globals, fonts, strings and messages, links, the section writers and their
reference parsers, corrupt-tag handling, the PNG renderer. Set HUD_FIXTURE=<path> to also write the .hp2 the C reader
test (engine/tests/test_huddef.c, which hard-codes the numbers of this world) opens. A skip-if-missing check converts
every real campaign map found in assets/decompressed and verifies it."""
import os
import struct
import sys
import tempfile
from pathlib import Path

sys.path[:0] = [str(Path(__file__).resolve().parents[1])]
from halo2ps2 import hp2, hudconv as hc, tagindex, texconv  # noqa: E402

BASE = tagindex.XBOX_TAG_BASE
ROOT = Path(__file__).resolve().parents[2]
NONE = hc.NONE16


# ---- a synthetic map region ------------------------------------------------------------------------
class Region:
    """Tag region builder: tags with zero-filled data blocks, then pokes (offsets are region offsets)."""

    def __init__(self, tags):
        self.region = bytearray(tagindex.TagIndex.build(tags, scenario_index=0))
        self.index = tagindex.TagIndex.parse_region(bytes(self.region), 0x1000)
        self.by_path = {(e.group, e.path): e for e in self.index.entries}
        self.cursor = {}

    def e(self, group, path):
        return self.by_path[(group, path)]

    def block(self, group, path):
        return self.e(group, path).data_address - BASE

    def tid(self, group, path):
        return self.e(group, path).tag_id

    def alloc(self, group, path, data: bytes) -> int:
        """Append data to the tail (from +0x1800) of a tag's block, 4-aligned; returns its tag-space address."""
        e = self.e(group, path)
        cur = (self.cursor.get(e.tag_id, 0x1800) + 3) & ~3
        off = e.data_address - BASE + cur
        self.region[off:off + len(data)] = data
        self.cursor[e.tag_id] = cur + len(data)
        return BASE + off

    def poke(self, off: int, fmt: str, *vals) -> None:
        struct.pack_into(fmt, self.region, off, *vals)

    def refl(self, off: int, count: int, addr: int) -> None:
        struct.pack_into("<III", self.region, off, count, addr, 0)

    def ref(self, off: int, group: str, path: str | None) -> None:
        tid = 0xFFFFFFFF if path is None else self.tid(group, path)
        self.region[off:off + 16] = struct.pack("<4sIII", group[::-1].encode(), 0x80000000, 0, tid)


def argb(a: int, r: int, g: int, b: int) -> bytes:
    return bytes((b, g, r, a))        # ColorARGBInt: bytes B, G, R, A


TAGS = [
    ("scnr", "levels\\t\\t", 0x3000), ("matg", "globals\\globals", 0x3000), ("hudg", "ui\\hud\\default", 0x3000),
    ("hud#", "ui\\hud\\counter", 0x3000), ("unhi", "ui\\hud\\unit", 0x3000), ("unhi", "ui\\hud\\vehicle", 0x3000),
    ("wphi", "weapons\\rifle\\rifle", 0x3000), ("wphi", "ui\\hud\\master", 0x3000), ("wphi", "weapons\\junk\\junk", 0x3000),
    ("grhi", "ui\\hud\\grenade", 0x3000), ("grhi", "ui\\hud\\plasma", 0x3000),
    ("font", "ui\\large_ui", 0x3000), ("font", "ui\\small_ui", 0x3000),
    ("ustr", "ui\\hud\\items", 0x3000), ("ustr", "ui\\hud\\icons", 0x3000),
    ("hmt ", "ui\\hud\\hud messages", 0x3000), ("hmt ", "levels\\t\\hud messages", 0x3000),
    ("bitm", "ui\\hud\\bitmaps\\sheet", 0x3000), ("bitm", "ui\\hud\\bitmaps\\back", 0x3000),
    ("bitm", "ui\\hud\\bitmaps\\digits", 0x3000), ("bitm", "ui\\hud\\bitmaps\\icons", 0x3000),
    ("weap", "weapons\\rifle\\rifle", 0x3000), ("weap", "weapons\\nohud\\nohud", 0x3000),
    ("bipd", "characters\\cyborg\\cyborg", 0x3000), ("vehi", "vehicles\\hog\\hog", 0x3000),
    ("snd!", "sound\\hit", 0x100), ("lsnd", "sound\\charge", 0x100), ("scen", "scenery\\rock", 0x100),
    ("bitm", "ui\\hud\\bitmaps\\hud_sensor_blip", 0x3000),       # the motion sensor blip, found by path (last: keeps the sprite indices)
]

# bitmap images: name -> list of (w, h, format, fill(x, y) -> int) ; pixel data are placed in the temp map file
FMT_AY8, FMT_A8Y8 = bitmaps_fmt = (2, 3)
IMAGES = {
    "sheet": [(64, 64, FMT_AY8, lambda x, y: x * 4)],
    "back": [(128, 32, FMT_AY8, lambda x, y: 200), (100, 20, FMT_A8Y8, lambda x, y: 0x80 << 8 | 100),
             (512, 16, FMT_AY8, lambda x, y: x // 2)],
    "digits": [(128, 64, FMT_AY8, lambda x, y: 255)],
    "icons": [(16, 16, FMT_AY8, lambda x, y: 128 + x)] * 4,
    "hud_sensor_blip": [(32, 32, FMT_AY8, lambda x, y: 255 if (x - 16) ** 2 + (y - 16) ** 2 < 196 else 0)],
}
# sequences: name -> list of (first image, image count, [(image, left, right, top, bottom, regx, regy)])
SEQS = {
    "sheet": [(0, 0, [(0, 0.0, 0.5, 0.0, 0.25, 0.25, 0.125)]),
              (0, 0, [(0, 0.5, 1.0, 0.0, 0.5, 0.75, 0.25), (0, 0.0, 0.5, 0.5, 1.0, 0.25, 0.75), (0, 0.5, 1.0, 0.5, 1.0, 0.75, 0.75)])],
    "back": [(0, 1, []), (1, 1, []), (2, 1, [])],
    "digits": [(0, 0, [(0, (k % 8) * 16 / 128, ((k % 8) * 16 + 12) / 128, (k // 8) * 32 / 64, ((k // 8) * 32 + 20) / 64, 0.0, 0.0)
                       for k in range(15)])],
    "icons": [(i, 1, []) for i in range(4)],
    "hud_sensor_blip": [(0, 1, [])],
}
BITMAP_TYPE = {"sheet": 3, "back": 0, "digits": 3, "icons": 0, "hud_sensor_blip": 0}


def write_bitmaps(r: Region, mapfile_pixels: bytearray) -> None:
    for name, images in IMAGES.items():
        path = f"ui\\hud\\bitmaps\\{name}"
        b = r.block("bitm", path)
        data = bytearray()
        for w, h, fmt, fill in images:
            off = 0x1000 + len(mapfile_pixels)
            if fmt == FMT_AY8:
                px = bytes(fill(x, y) & 255 for y in range(h) for x in range(w))
            else:
                px = b"".join(struct.pack("<H", fill(x, y)) for y in range(h) for x in range(w))
            data += struct.pack("<4sHHHHHHhhHHII", b"mtib", w, h, 1, 0, fmt, 0x01, 0, 0, 0, 0, off, len(px)).ljust(48, b"\0")
            mapfile_pixels += px
        r.refl(b + 0x60, len(images), r.alloc("bitm", path, bytes(data)))
        seqs = bytearray()
        for first, count, sprites in SEQS[name]:
            sp = b"".join(struct.pack("<hhIffffff", bi, 0, 0, *rest) for bi, *rest in sprites)
            sa = r.alloc("bitm", path, sp) if sprites else 0
            seqs += struct.pack("<32shh", b"", first, count).ljust(0x34, b"\0") + struct.pack("<III", len(sprites), sa, 0) + bytes(64 - 0x40)
        r.refl(b + 0x54, len(SEQS[name]), r.alloc("bitm", path, bytes(seqs)))
        struct.pack_into("<H", r.region, b, BITMAP_TYPE[name])


def static_body(r: Region, o: int, x, y, sx, sy, scaling, bitmap, seq, colour, flash, period, delay, count, fflags, length, disabled) -> None:
    """The common 0x24 header and the static element body at region offset o."""
    r.poke(o, "<hhffH", x, y, sx, sy, scaling)
    r.ref(o + 0x24, "bitm", bitmap)
    r.region[o + 0x34:o + 0x38] = colour
    r.region[o + 0x38:o + 0x3C] = flash
    r.poke(o + 0x3C, "<ffHHf", period, delay, count, fflags, length)
    r.region[o + 0x4C:o + 0x50] = disabled
    r.poke(o + 0x54, "<H", seq)


def meter_body(r: Region, o: int, x, y, bitmap, seq, cmin, cmax, cflash, cempty, flags, minv, amult, abias, vscale, opacity, transl, cdis) -> None:
    r.poke(o, "<hhffH", x, y, 1.0, 1.0, 0)
    r.ref(o + 0x24, "bitm", bitmap)
    for k, c in zip((0x34, 0x38, 0x3C, 0x40), (cmin, cmax, cflash, cempty)):
        r.region[o + k:o + k + 4] = c
    r.poke(o + 0x44, "<BBHBBH", flags, minv, seq, amult, abias, vscale)
    r.poke(o + 0x4C, "<ff", opacity, transl)
    r.region[o + 0x54:o + 0x58] = cdis


def number_body(r: Region, o: int, x, y, sx, sy, colour, flash, digits, flags, frac, period=1.0) -> None:
    r.poke(o, "<hhffH", x, y, sx, sy, 0)
    r.region[o + 0x24:o + 0x28] = colour
    r.region[o + 0x28:o + 0x2C] = flash
    r.poke(o + 0x2C, "<ffHHf", period, 0.0, 1, 0, 1.0)
    r.region[o + 0x3C:o + 0x40] = colour
    r.poke(o + 0x44, "<BBB", digits, flags, frac)


def overlay_common(r: Region, o: int, x, y, colour, flash, seq_at: int, seq: int, flags_at: int, flags: int, size: int,
                   typ_at: int | None = None, typ: int = 0) -> None:
    r.poke(o, "<hhffH", x, y, 1.0, 1.0, 3)
    r.region[o + 0x24:o + 0x28] = colour
    r.region[o + 0x28:o + 0x2C] = flash
    r.poke(o + 0x2C, "<ffHHf", 1.0, 0.0, 1, 0, 1.0)
    r.poke(o + seq_at, "<H", seq)
    r.poke(o + flags_at, "<I", flags)
    if typ_at is not None:
        r.poke(o + typ_at, "<H", typ)


def build_world() -> tuple[Region, bytearray]:
    r = Region(TAGS)
    pixels = bytearray()
    write_bitmaps(r, pixels)
    P = lambda path: f"ui\\hud\\bitmaps\\{path}"  # noqa: E731
    G = "ui\\hud\\default"
    # -- globals, hud_globals, hud_number
    gb = r.block("matg", "globals\\globals")
    r.ref(gb + 0x78C, "hudg", G)
    r.ref(gb + 0x7DC, "hud#", "ui\\hud\\counter")
    grens = bytearray(2 * 68)
    for i, path in enumerate(("ui\\hud\\grenade", "ui\\hud\\plasma")):
        grens[i * 68 + 0x14:i * 68 + 0x24] = struct.pack("<4sIII", b"ihrg", 0, 0, r.tid("grhi", path))
    r.refl(gb + 0x128, 2, r.alloc("matg", "globals\\globals", bytes(grens)))
    hb = r.block("hudg", G)
    r.poke(hb, "<H", 0)
    r.poke(hb + 0x24, "<hh", 0, 60)
    r.ref(hb + 0x48, "font", "ui\\large_ui")
    r.ref(hb + 0x58, "font", "ui\\small_ui")
    r.poke(hb + 0x68, "<ff", 2.0, 2.0)
    r.poke(hb + 0x70, "<4f", 0.5, 0.25, 0.75, 1.0)
    r.poke(hb + 0x80, "<4f", 1.0, 1.0, 1.0, 1.0)
    r.ref(hb + 0x94, "ustr", "ui\\hud\\items")
    r.ref(hb + 0xA4, "bitm", P("icons"))
    r.ref(hb + 0xB4, "ustr", "ui\\hud\\icons")
    r.ref(hb + 0xF0, "hmt ", "ui\\hud\\hud messages")
    r.ref(hb + 0x2C0, "wphi", "ui\\hud\\master")
    r.poke(hb + 0x2D0, "<fff", 20.0, 0.0002, 32.0)
    r.ref(hb + 0x338, "bitm", P("icons"))
    r.poke(hb + 0x348, "<HH", 3, 1)
    r.region[hb + 0x34C:hb + 0x350] = argb(200, 255, 0, 0)
    icons = b"".join(struct.pack("<Hhhh", seq, -6, -2, -5) + argb(255, 75, 239, 80) + struct.pack("<BBH", 0, 2, i)
                     for i, seq in enumerate((0, 1, 3)))
    r.refl(hb + 0xC4, 3, r.alloc("hudg", G, icons))
    nb = r.block("hud#", "ui\\hud\\counter")
    r.ref(nb, "bitm", P("digits"))
    r.poke(nb + 0x10, "<6b", 12, 9, 1, 0, 6, 6)
    # scenario: the level's hud messages
    r.ref(r.block("scnr", "levels\\t\\t") + 0x594, "hmt ", "levels\\t\\hud messages")

    # -- unit HUD A: anchor top right
    U = "ui\\hud\\unit"
    ub = r.block("unhi", U)
    r.poke(ub, "<H", hc.A_TR)
    z = argb(0, 0, 0, 0)
    static_body(r, ub + 0x24, 0, 0, 1.0, 1.0, 0, None, 0, z, z, 0, 0, 0, 0, 0, z)                               # hud background: none
    static_body(r, ub + 0x8C, 0, 1, 1.0, 1.0, 0, P("back"), 0, argb(220, 170, 213, 255), argb(255, 255, 85, 43), 3.0, 0.225, 3, 0, 0.225, z)
    meter_body(r, ub + 0xF4, 7, 0, P("sheet"), 0, argb(0, 0, 80, 159), argb(0, 40, 150, 255), argb(0, 255, 255, 255), z, 0, 0, 1, 55, 198, 0.0, 0.5, argb(0, 0, 37, 74))
    static_body(r, ub + 0x17C, -5, 18, 1.0, 1.0, 0, P("back"), 1, argb(200, 1, 2, 3), z, 1.0, 0.0, 1, 0, 1.0, z)
    meter_body(r, ub + 0x1E4, 36, 17, P("sheet"), 0, argb(0, 255, 0, 0), argb(0, 40, 150, 255), z, z, 1, 31, 30, 15, 0, 0.0, 0.75, z)
    static_body(r, ub + 0x26C, 0, 0, 1.0, 1.0, 0, P("back"), 2, argb(40, 6, 20, 40), z, 0, 0, 0, 0, 0, z)       # motion background: 512 x 16
    static_body(r, ub + 0x2D4, 0, 0, 1.0, 1.0, 0, None, 0, z, z, 0, 0, 0, 0, 0, z)
    r.poke(ub + 0x35C, "<hhffH", 41, 41, 1.0, 1.0, 0)                                                         # motion centre
    snd = bytearray(2 * 56)
    snd[0:16] = struct.pack("<4sIII", b"!dns", 0, 0, r.tid("snd!", "sound\\hit"))
    struct.pack_into("<If", snd, 0x10, 2, 0.5)
    snd[56:72] = struct.pack("<4sIII", b"dnsl", 0, 0, r.tid("lsnd", "sound\\charge"))
    struct.pack_into("<If", snd, 56 + 0x10, 1, 0.0)
    r.refl(ub + 0x3C0, 2, r.alloc("unhi", U, bytes(snd)))
    aux = bytearray(324)
    struct.pack_into("<H", aux, 0, 0)
    r.region[0:0] = b""
    auxb = r.alloc("unhi", U, bytes(aux)) - BASE
    r.refl(ub + 0x3CC, 1, BASE + auxb)
    static_body(r, auxb + 0x14, -7, 34, 1.0, 1.0, 0, P("sheet"), 1, argb(255, 170, 213, 255), z, 3.0, 0.225, 3, 0, 0.225, z)
    meter_body(r, auxb + 0x14 + 104, -7, 33, P("sheet"), 0, argb(0, 0, 80, 159), argb(0, 40, 150, 255), z, z, 0, 0, 1, 0, 255, 0.0, 0.75, z)
    r.poke(auxb + 0x14 + 208, "<fI", 0.25, 1)

    # -- unit HUD B: anchor centre, width scale 2, height scale unset
    V = "ui\\hud\\vehicle"
    vb = r.block("unhi", V)
    r.poke(vb, "<H", hc.A_C)
    static_body(r, vb + 0x24, 0, -40, 2.0, 0.0, 0, P("sheet"), 0, argb(255, 255, 255, 255), z, 0, 0, 0, 0, 0, z)

    # -- weapon HUDs: child ("master"), rifle, junk
    M, W, J = "ui\\hud\\master", "weapons\\rifle\\rifle", "weapons\\junk\\junk"
    mb = r.block("wphi", M)
    r.poke(mb + 0x10, "<I", 1)
    r.poke(mb + 0x3C, "<H", hc.A_TL)
    r.poke(mb + 0x9C, "<I", 1 << 3 | 1 << 8)
    crosses = bytearray()
    cross_addrs = []
    for typ, seq, flags in ((3, 1, 0), (8, 0, 1)):
        ov = bytearray(108)
        ca = r.alloc("wphi", M, bytes(ov)) - BASE
        overlay_common(r, ca, 0, 0, argb(0, 255, 0, 0), z, 0x46, seq, 0x48, flags, 108)
        cross_addrs.append(BASE + ca)
    for (typ, _s, _f), ca in zip(((3, 1, 0), (8, 0, 1)), cross_addrs):
        el = bytearray(104)
        struct.pack_into("<H", el, 0, typ)
        el[0x24:0x34] = struct.pack("<4sIII", b"mtib", 0, 0, r.tid("bitm", P("sheet")))
        struct.pack_into("<III", el, 0x34, 1, ca, 0)
        crosses += el
    r.refl(mb + 0x84, 2, r.alloc("wphi", M, bytes(crosses)))
    # rifle
    rb = r.block("wphi", W)
    r.ref(rb, "wphi", M)
    r.poke(rb + 0x10, "<IHHHH", 0, 61, 10, 0, 0)
    r.poke(rb + 0x3C, "<H", hc.A_BL)
    r.poke(rb + 0x9C, "<I", 3)
    r.poke(rb + 0x13C, "<Hhhh", 2, 2, 0, 0)
    st = bytearray(180)
    r.region[0:0] = b""
    sa = r.alloc("wphi", W, bytes(st)) - BASE
    r.poke(sa, "<H2xH", 1, 1)
    static_body(r, sa + 0x24, 12, -40, 1.0, 1.0, 0, P("back"), 0, argb(110, 128, 75, 20), argb(110, 22, 43, 128), 3.0, 0.225, 3, 0, 0.225, z)
    r.refl(rb + 0x60, 1, BASE + sa)
    ma = r.alloc("wphi", W, bytes(180)) - BASE
    r.poke(ma, "<H2xH", 1, 0)
    meter_body(r, ma + 0x24, 14, 15, P("sheet"), 0, argb(0, 20, 75, 128), argb(0, 20, 75, 128), argb(0, 128, 43, 22), argb(0, 0, 19, 37), 0, 0, 4, 2, 0, 0.0, 0.75, argb(128, 128, 43, 22))
    r.refl(rb + 0x6C, 1, BASE + ma)
    nums = bytearray(2 * 160)
    na = r.alloc("wphi", W, bytes(nums)) - BASE
    r.poke(na, "<H2xH", 0, 1)
    number_body(r, na + 0x24, 29, 3, 0.0, 0.0, argb(255, 255, 240, 230), argb(255, 230, 230, 255), 3, 1, 0)
    r.poke(na + 160, "<H2xH", 1, 2)
    number_body(r, na + 160 + 0x24, 25, 3, 1.5, 1.5, argb(255, 255, 240, 230), argb(255, 230, 230, 255), 2, 3, 1)
    r.refl(rb + 0x78, 2, BASE + na)
    # overlay element with one sub overlay (136 B)
    sub = bytearray(136)
    suba = r.alloc("wphi", W, bytes(sub)) - BASE
    overlay_common(r, suba, 53, 4, argb(255, 255, 240, 230), argb(255, 230, 230, 255), 0x48, 2, 0x4C, 1, 136, 0x4A, 16)
    oel = bytearray(104)
    oa = r.alloc("wphi", W, bytes(oel)) - BASE
    r.poke(oa, "<H2xH", 0, 1)
    r.ref(oa + 0x24, "bitm", P("icons"))
    r.refl(oa + 0x34, 1, BASE + suba)
    r.refl(rb + 0x90, 1, BASE + oa)
    # crosshairs of the rifle: aim (sheet seq 0, flags 1, alpha-less colour) and zoom (sheet seq 1, three frames, only zoomed)
    cr = bytearray()
    for typ, seq, flags, colour in ((0, 0, 1, argb(0, 40, 150, 255)), (1, 1, 4, argb(60, 40, 150, 255))):
        ca = r.alloc("wphi", W, bytes(108)) - BASE
        overlay_common(r, ca, 1, -2, colour, argb(0, 255, 0, 0), 0x46, seq, 0x48, flags, 108)
        el = bytearray(104)
        struct.pack_into("<H", el, 0, typ)
        el[0x24:0x34] = struct.pack("<4sIII", b"mtib", 0, 0, r.tid("bitm", P("sheet")))
        struct.pack_into("<III", el, 0x34, 1, BASE + ca, 0)
        cr += el
    r.refl(rb + 0x84, 2, r.alloc("wphi", W, bytes(cr)))
    # junk: pickup icon fields hold a text fragment
    jb = r.block("wphi", J)
    r.poke(jb + 0x13C, "<Hhhh", 25193, 29805, 18480, 7936)

    # -- grenade HUDs
    for path, seqs in (("ui\\hud\\grenade", (2, 3)), ("ui\\hud\\plasma", (3, 2))):
        gb2 = r.block("grhi", path)
        static_body(r, gb2 + 0x24, 82, 1, 1.0, 1.0, 0, P("back"), 0, argb(200, 255, 150, 40), argb(200, 255, 85, 43), 3.0, 0.225, 3, 0, 0.225, z)
        static_body(r, gb2 + 0x8C, 82, 1, 1.0, 1.0, 0, P("back"), 1, argb(220, 255, 213, 170), z, 3.0, 0.225, 3, 0, 0.225, z)
        number_body(r, gb2 + 0xF4, 107, 3, 1.0, 1.0, argb(255, 255, 240, 230), argb(255, 230, 230, 255), 1, 1, 0)
        r.ref(gb2 + 0x14C, "bitm", P("icons"))
        subs = bytearray(2 * 136)
        sa2 = r.alloc("grhi", path, bytes(subs)) - BASE
        for k, seq in enumerate(seqs):
            overlay_common(r, sa2 + k * 136, 90, 4, argb(255, 255, 240, 230), argb(255, 230, 230, 255), 0x48, seq, 0x4C, 0, 136, 0x4A, 1 if k == 0 else 4)
        r.refl(gb2 + 0x15C, 2, BASE + sa2)

    # -- fonts: ' ', '?', 'A', 'B', 'j'
    for fpath, scale in (("ui\\large_ui", 1), ("ui\\small_ui", 2)):
        fb = r.block("font", fpath)
        r.poke(fb, "<Ihhhh", 0, 17 // scale, 4 // scale, 4 // scale, 0)
        chars = [(32, 6, -2, 0, 2, -5), (63, 7, 5, 8, 1, 8), (65, 10, 6, 8, 1, 8), (66, 9, 6 * scale, 8, 1, 8), (106, 5, 4, 10, 0, 8)]
        px = bytearray()
        recs = bytearray()
        for code, adv, bw, bh, ox, oy in chars:
            off = len(px)
            if bw > 0 and bh > 0:
                px += bytes(((code + x + y) & 0xFF) | 1 for y in range(bh) for x in range(bw))
            recs += struct.pack("<Hhhhhh2Hi", code, adv, bw, bh, ox, oy, 0xFFFF, 0, off if bw > 0 and bh > 0 else 0)
        r.refl(fb + 0x7C, len(chars), r.alloc("font", fpath, bytes(recs)))
        pa = r.alloc("font", fpath, bytes(px))
        r.poke(fb + 0x88, "<IIII", len(px), 0, 0, pa)

    # -- strings
    def ustr(path, strings):
        b = r.block("ustr", path)
        els = bytearray()
        for s in strings:
            raw = s.encode("utf-16le") + b"\0\0"
            els += struct.pack("<IIII", len(raw), 0, 0, r.alloc("ustr", path, raw)) + bytes(4)
        r.refl(b, len(strings), r.alloc("ustr", path, bytes(els)))
    ustr("ui\\hud\\items", ["Picked up an item", "Picked up %d items", ""])
    ustr("ui\\hud\\icons", ["black", "white"])

    def hmt(path, messages):
        """messages: (name, [segments]) with segments ('t', text) or ('i', value)."""
        b = r.block("hmt ", path)
        text = bytearray()
        elements = bytearray()
        recs = bytearray()
        for name, segs in messages:
            tstart, estart = len(text) // 2, len(elements) // 2
            for kind, v in segs:
                if kind == "t":
                    raw = (v + "\0").encode("utf-16le")
                    text += raw
                    elements += bytes((0, len(raw) // 2))
                else:
                    elements += bytes((1, v))
            recs += struct.pack("<32sHHH", name.encode(), tstart, estart, len(segs)).ljust(64, b"\0")
        ta = r.alloc("hmt ", path, bytes(text))
        r.poke(b, "<IIII", len(text), 0, 0, ta)
        r.refl(b + 0x14, len(elements) // 2, r.alloc("hmt ", path, bytes(elements)))
        r.refl(b + 0x20, len(messages), r.alloc("hmt ", path, bytes(recs)))
    hmt("ui\\hud\\hud messages", [("pickup", [("t", "Hold "), ("i", 18), ("t", " to pick up "), ("i", 32), ("t", "")]),
                                  ("exit", [("t", "Press "), ("i", 18), ("t", " to exit")])])
    hmt("levels\\t\\hud messages", [("tip", [("t", "Look up")])])

    # -- links: weapons, units
    r.ref(r.block("weap", W) + 0x480, "wphi", W)
    wb = r.block("bipd", "characters\\cyborg\\cyborg")
    r.ref(wb + 0x100, "unhi", U)
    r.ref(wb + 0x130, "unhi", V)
    r.ref(r.block("vehi", "vehicles\\hog\\hog") + 0x200, "unhi", V)
    return r, pixels


def synthetic_map(tmp: Path) -> texconv.MapData:
    r, pixels = build_world()
    mapfile = tmp / "t.map"
    mapfile.write_bytes(bytes(0x1000) + bytes(pixels))
    idx = tagindex.TagIndex.parse_region(bytes(r.region), 0x1000)
    return texconv.MapData(mapfile, None, idx, bytes(r.region), {e.tag_id: e for e in idx.entries})  # type: ignore[arg-type]


def collect_synthetic(tmp: Path, mutate=None) -> tuple[hc.HudSet, dict]:
    r, pixels = build_world()
    if mutate:
        mutate(r)
    mapfile = tmp / "t.map"
    mapfile.write_bytes(bytes(0x1000) + bytes(pixels))
    idx = tagindex.TagIndex.parse_region(bytes(r.region), 0x1000)
    m = texconv.MapData(mapfile, None, idx, bytes(r.region), {e.tag_id: e for e in idx.entries})  # type: ignore[arg-type]
    return hc.collect_map(m, "t"), {f"{e.group}:{e.path}": e.tag_id for e in idx.entries}


def gs(r, g, b, a):
    return hc.gs_colour(r, g, b, a)


def hud(hs, ids, key):
    return next(h for h in hs.huds if h.tag_id == ids[key])


# ---- tests --------------------------------------------------------------------------------------------
def test_colours_layout() -> None:
    assert hc.gs_colour(255, 255, 255, 255) == 0x80808080 and hc.gs_colour(0, 0, 0, 0) == 0
    assert hc.gs_colour(255, 0, 128, 255) == 0x80 << 24 | 64 << 16 | 0x80
    assert hc.argb_int(bytes((0xFF, 0xBA, 0x75, 0x80))) == hc.gs_colour(0x75, 0xBA, 0xFF, 0x80)       # B, G, R, A byte order
    assert hc.argb_float(0.5, 0.458, 0.727, 1.0) == hc.gs_colour(117, 185, 255, 128)
    assert hc.opaque_if_unset(hc.gs_colour(10, 20, 30, 0)) >> 24 == 0x80 and hc.opaque_if_unset(0) == 0
    assert hc.opaque_if_unset(hc.gs_colour(10, 20, 30, 50)) == hc.gs_colour(10, 20, 30, 50)
    assert [hc.ticks(s) for s in (0.0, 0.225, 1.0, 3.0, -1.0, float("nan"), 1e9)] == [0, 7, 30, 90, 0, 0, 0xFFFF]
    # anchors: the matching corner (centre) goes to anchor point + offset
    # anchors: the matching corner (centre) goes to the title-safe frame corner (48, 36 in) + offset, y down everywhere
    assert hc.resolve_position(hc.A_TL, 5, 6, 100, 20) == (53, 42)
    assert hc.resolve_position(hc.A_TR, -5, 6, 100, 20) == (487, 42)
    assert hc.resolve_position(hc.A_BL, 12, -40, 128, 32) == (60, 372)
    assert hc.resolve_position(hc.A_BR, 0, 0, 10, 10) == (582, 434)
    assert hc.resolve_position(hc.A_C, 0, -40, 32, 16) == (304, 192)
    # the motion sensor centre is counted inward from the corner; the sensor is bottom left whatever the HUD's anchor
    assert hc.corner_point(hc.A_BL, 41, 41) == (89, 403) and hc.corner_point(hc.A_TR, 10, 5) == (582, 41)
    assert hc.element_anchor(hc.R_MOTION_BG, hc.A_TR) == hc.A_BL and hc.element_anchor(hc.R_SHIELD_BG, hc.A_TR) == hc.A_TR
    assert hc._scale(0.0, -1.0) == (1.0, 1.0) and hc._scale(2.0, 1.5) == (2.0, 1.5) and hc._scale(float("nan"), 3.0) == (1.0, 3.0)
    assert hc.pow2_ceil(1) == 1 and hc.pow2_ceil(100) == 128 and hc.pow2_ceil(512) == 512


def test_collect() -> None:
    with tempfile.TemporaryDirectory() as d:
        hs, ids = collect_synthetic(Path(d))
        assert hs.problems == [], hs.problems
        assert hc.verify(hs) == []
        assert [h.kind for h in hs.huds] == [hc.K_UNIT] * 2 + [hc.K_WEAPON] * 3 + [hc.K_GRENADE] * 2
        sp = hs.sprites
        sheet = [s for s in sp if s.bitmap_tag == ids["bitm:ui\\hud\\bitmaps\\sheet"]]
        assert len(sheet) == 4                       # seq 0 (one sprite) and seq 1 (three frames), each built once
        # sheet seq 0: 32 x 16 px of the 64 x 64 image, texel rect (0,0)-(32,16), registration (16, 8)
        s0 = sheet[0]
        assert (s0.w, s0.h, s0.u0, s0.v0, s0.u1, s0.v1, s0.reg_x, s0.reg_y, s0.flags) == (32, 16, 0, 0, 32, 16, 16, 8, 0)
        s1 = sheet[1]                                # sheet seq 1 frame 0: right half, top half
        assert (s1.w, s1.h, s1.u0, s1.v0, s1.u1, s1.v1) == (32, 32, 32, 0, 64, 32) and s1.tex == s0.tex
        # interface bitmaps: 128 x 32 whole image; 100 x 20 padded to 128 x 32; 512 x 16 halved to 256 x 8
        back = [s for s in sp if s.bitmap_tag == ids["bitm:ui\\hud\\bitmaps\\back"]]
        assert [(s.w, s.h, s.u0, s.v0, s.u1, s.v1, s.flags) for s in back] == [
            (128, 32, 0, 0, 128, 32, 0), (100, 20, 0, 0, 100, 20, hc.SF_PADDED), (512, 16, 0, 0, 256, 8, hc.SF_SCALED)]
        tx = hs.textures.textures
        assert (tx[back[0].tex].width, tx[back[0].tex].height) == (128, 32) and (tx[back[1].tex].width, tx[back[1].tex].height) == (128, 32)
        assert (tx[back[2].tex].width, tx[back[2].tex].height) == (256, 8)
        assert all(len(t.mips) == 1 and t.psm in (texconv.PSMT8, texconv.PSMT4) for t in tx)
        # ay8 images become exact white alpha ramps: index = the source value, CLUT entry i = (255, 255, 255, alpha i)
        t0 = tx[s0.tex]
        pal = texconv.parse_clut(t0.clut, 256)
        assert pal[7] == (255, 255, 255, texconv.ps2_alpha(7)) and t0.mips[0][:5] == bytes((0, 4, 8, 12, 16))
        # unit HUD A, anchor top right
        ua = hud(hs, ids, "unhi:ui\\hud\\unit")
        assert ua.anchor == hc.A_TR and len(ua.elems) == 7 and len(ua.meters) == 3 and len(ua.sounds) == 2
        assert [e.role for e in ua.elems] == [hc.R_HUD_BG, hc.R_SHIELD_BG, hc.R_HEALTH_BG, hc.R_MOTION_BG, hc.R_MOTION_FG,
                                              hc.R_MOTION_CENTER, hc.R_AUX_BG]
        bg, shield_bg, health_bg, motion_bg, motion_fg, centre, aux_bg = ua.elems[0], ua.elems[1], ua.elems[2], ua.elems[3], ua.elems[4], ua.elems[5], ua.elems[6]
        assert (bg.role, bg.sprite, bg.frames, bg.w) == (hc.R_HUD_BG, NONE, 0, 0)
        assert (shield_bg.role, shield_bg.w, shield_bg.h, shield_bg.x, shield_bg.y) == (hc.R_SHIELD_BG, 128, 32, 464, 37)
        assert shield_bg.colour == gs(170, 213, 255, 220) and shield_bg.flash_colour == gs(255, 85, 43, 255)
        assert (shield_bg.flash_period, shield_bg.flash_delay, shield_bg.flash_count, shield_bg.flash_length) == (90, 7, 3, 7)
        assert (health_bg.w, health_bg.h, health_bg.x, health_bg.y, health_bg.off_x, health_bg.off_y) == (100, 20, 487, 54, -5, 18)
        assert (motion_bg.w, motion_bg.h, motion_bg.x, motion_bg.y, motion_bg.anchor) == (512, 16, 48, 444 - 16, hc.A_BL) and motion_bg.seq == 2
        assert motion_fg.sprite == NONE and motion_fg.anchor == hc.A_BL
        assert (centre.role, centre.x, centre.y, centre.off_x, centre.anchor, centre.sprite) == (hc.R_MOTION_CENTER, 89, 403, 41, hc.A_BL, NONE)
        assert aux_bg.role == hc.R_AUX_BG and aux_bg.frames == 3 and aux_bg.w == 32 and aux_bg.x == 592 - 7 - 32
        sm, hm, am = ua.meters
        assert (sm.role, sm.w, sm.h, sm.x, sm.y, sm.alpha_mult, sm.alpha_bias, sm.value_scale, sm.flags) == (hc.M_SHIELD, 32, 16, 592 + 7 - 32, 36, 1, 55, 198, 0)
        assert sm.colour_min == gs(0, 80, 159, 255) and sm.colour_max == gs(40, 150, 255, 255) and sm.colour_flash == gs(255, 255, 255, 255)
        assert sm.colour_empty == 0 and sm.colour_disabled == gs(0, 37, 74, 255) and (sm.opacity, sm.translucency) == (0.0, 0.5)
        assert (hm.flags, hm.min_value, hm.alpha_mult, hm.alpha_bias, hm.off_x, hm.off_y) == (1, 31, 30, 15, 36, 17)
        assert hm.colour_min == gs(255, 0, 0, 255)
        assert (am.role, am.min_fraction, am.aux_flags, am.state) == (hc.M_AUX, 0.25, 1, 0) and am.y == 36 + 33
        assert [(s.sound_tag, s.group, s.latched, s.scale) for s in ua.sounds] == [
            (ids["snd!:sound\\hit"], "snd!", 2, 0.5), (ids["lsnd:sound\\charge"], "lsnd", 1, 0.0)]
        # unit HUD B: centred, width scale 2, height scale unset (1.0)
        ub = hud(hs, ids, "unhi:ui\\hud\\vehicle")
        e = ub.elems[0]
        assert (e.sx, e.sy, e.w, e.h, e.x, e.y, e.anchor) == (2.0, 1.0, 64, 16, 288, 192, hc.A_C)
        # weapon HUD: child, flags, cutoffs, pickup icon
        rifle = hud(hs, ids, "wphi:weapons\\rifle\\rifle")
        master = hud(hs, ids, "wphi:ui\\hud\\master")
        assert rifle.child == ids["wphi:ui\\hud\\master"] and master.child == 0 and master.flags == 1
        assert (rifle.total_cutoff, rifle.loaded_cutoff, rifle.heat_cutoff, rifle.age_cutoff, rifle.anchor) == (61, 10, 0, 0, hc.A_BL)
        assert (rifle.icon_seq, rifle.icon_width_offset) == (2, 2) and rifle.icon_sprite != NONE
        assert hs.sprites[rifle.icon_sprite].bitmap_tag == ids["bitm:ui\\hud\\bitmaps\\icons"]
        junk = hud(hs, ids, "wphi:weapons\\junk\\junk")
        assert junk.icon_seq == NONE and junk.icon_sprite == NONE and any("junk" in n for n in hs.notes)
        st = [x for x in rifle.elems if x.role == hc.R_STATIC][0]
        assert (st.state, st.allowed, st.x, st.y, st.w, st.h) == (1, 1, 48 + 12, 444 - 40 - 32, 128, 32)
        ov = [x for x in rifle.elems if x.role == hc.R_OVERLAY][0]
        assert (ov.state, ov.allowed, ov.type, ov.ext_flags, ov.seq, ov.off_x, ov.off_y) == (0, 1, 16, 1, 2, 53, 4)
        assert (ov.w, ov.h, ov.x, ov.y) == (16, 16, 48 + 53, 444 + 4 - 16)         # bottom left anchor, 16 px sprite
        wm = rifle.meters[0]
        assert (wm.role, wm.state, wm.allowed, wm.alpha_mult, wm.alpha_bias, wm.x, wm.y) == (hc.M_WEAPON, 1, 0, 4, 2, 48 + 14, 444 + 15 - 16)
        assert wm.colour_disabled == gs(128, 43, 22, 128)                                   # alpha 128 kept
        n0, n1 = rifle.numbers
        assert (n0.role, n0.state, n0.allowed, n0.max_digits, n0.flags, n0.fraction_digits) == (hc.N_WEAPON, 0, 1, 3, 1, 0)
        assert (n0.sx, n0.sy) == (1.0, 1.0)                                                 # zero scale = unset
        digit_h = hs.sprites[hs.globals.digit_first].h
        assert (n0.w, n0.h) == (3 * 9, digit_h) and n0.x == 48 + 29 and n0.y == 444 + 3 - digit_h
        assert (n1.allowed, n1.max_digits, n1.flags, n1.fraction_digits, n1.sx) == (2, 2, 3, 1, 1.5)
        assert n1.w == round((2 * 9 + 6) * 1.5)                                              # digits + decimal point, scaled
        assert n0.colour == gs(255, 240, 230, 255) and n0.flash_period == 30 and n0.flash_length == 30
        # crosshairs: own sets, mask, alpha rule (alpha 0 + RGB -> opaque), anchored at the centre
        assert rifle.crosshair_mask == 3 and [c.type for c in rifle.crosshairs] == [0, 1] and master.crosshair_mask == 0x108
        aim, zoom = rifle.cross_elems
        assert (aim.role, aim.anchor, aim.type, aim.ext_flags, aim.seq, aim.frames) == (hc.R_CROSSHAIR, hc.A_C, 0, 1, 0, 1)
        assert aim.colour == gs(40, 150, 255, 255) and aim.flash_colour == gs(255, 0, 0, 255)   # alpha 0 -> opaque
        assert (aim.w, aim.h, aim.x, aim.y) == (32, 16, 320 + 1 - 16, 240 - 2 - 8)
        assert (zoom.type, zoom.ext_flags, zoom.frames, zoom.colour) == (1, 4, 3, gs(40, 150, 255, 60))
        assert hs.sprites[zoom.sprite + 2].u0 == 32 and zoom.sprite == hs.sprites.index(s1)
        # grenade HUDs
        g0 = hud(hs, ids, "grhi:ui\\hud\\grenade")
        assert [e.role for e in g0.elems] == [hc.R_GRENADE_BG, hc.R_GRENADE_COUNT_BG, hc.R_GRENADE_OVERLAY, hc.R_GRENADE_OVERLAY]
        assert (g0.elems[0].x, g0.elems[0].y, g0.elems[0].w) == (48 + 82, 36 + 1, 128)
        assert [(e.seq, e.type) for e in g0.elems[2:]] == [(2, 1), (3, 4)]
        assert (g0.numbers[0].role, g0.numbers[0].max_digits, g0.numbers[0].sx) == (hc.N_GRENADE, 1, 1.0)
        # hud_globals
        g = hs.globals
        assert (g.tag_id, g.anchor, g.off_y, g.item_scale) == (ids["hudg:ui\\hud\\default"], 0, 60, (2.0, 2.0))
        assert g.font_sp == ids["font:ui\\large_ui"] and g.font_mp == ids["font:ui\\small_ui"]
        assert g.default_weapon_hud == ids["wphi:ui\\hud\\master"] and g.motion == (20.0, struct.unpack("<f", struct.pack("<f", 0.0002))[0], 32.0)
        assert g.item_strings == ids["ustr:ui\\hud\\items"] and g.icon_strings == ids["ustr:ui\\hud\\icons"]
        assert g.messages_tag == ids["hmt :ui\\hud\\hud messages"] and g.level_messages_tag == ids["hmt :levels\\t\\hud messages"]
        assert (g.digit_first, g.digit_count, g.digit_widths) == (hs.sprites.index(next(s for s in hs.sprites if s.bitmap_tag == ids["bitm:ui\\hud\\bitmaps\\digits"])), 15, (12, 9, 1, 0, 6, 6))
        dsp = hs.sprites[g.digit_first + 9]
        assert (dsp.w, dsp.h) == (12, 20) and dsp.u0 == 16 + 16 * 0 + 0 and dsp.v0 == 32
        assert len(g.icons) == 3 and [i[1] for i in g.icons] == [0, 1, 3] and g.icons[0][2:5] == (-6, -2, -5)
        assert g.icons[0][5] == gs(75, 239, 80, 255) and g.damage_sprite != NONE and g.damage_mp_sprite != NONE
        # the blip: the last sprite, sequence 0 of the bitmap found by path
        assert g.blip_tag == ids["bitm:ui\\hud\\bitmaps\\hud_sensor_blip"] and g.blip_sprite == len(hs.sprites) - 1
        assert (hs.sprites[g.blip_sprite].w, hs.sprites[g.blip_sprite].h) == (32, 32)
        assert g.damage_colour == gs(255, 0, 0, 200)
        # links: weapon -> HUD, unit -> HUDs in order, grenade type -> HUD
        assert sorted(hs.links) == sorted([
            (ids["weap:weapons\\rifle\\rifle"], hc.L_WEAPON, ids["wphi:weapons\\rifle\\rifle"], 0),
            (ids["bipd:characters\\cyborg\\cyborg"], hc.L_UNIT, ids["unhi:ui\\hud\\unit"], 0),
            (ids["bipd:characters\\cyborg\\cyborg"], hc.L_UNIT, ids["unhi:ui\\hud\\vehicle"], 1),
            (ids["vehi:vehicles\\hog\\hog"], hc.L_UNIT, ids["unhi:ui\\hud\\vehicle"], 0),
            (0, hc.L_GRENADE, ids["grhi:ui\\hud\\grenade"], 0), (1, hc.L_GRENADE, ids["grhi:ui\\hud\\plasma"], 0)])
        # fonts: glyph table, atlas
        f = hs.fonts[0]
        assert (f.ascent, f.descent, f.leading_h) == (17, 4, 4) and [gl.code for gl in f.glyphs] == [32, 63, 65, 66, 106]
        a = next(gl for gl in f.glyphs if gl.code == 65)
        assert (a.advance, a.w, a.h, a.ox, a.oy, a.u1 - a.u0, a.v1 - a.v0) == (10, 6, 8, 1, 8, 6, 8) and a.tex != NONE
        sp_ = next(gl for gl in f.glyphs if gl.code == 32)
        assert (sp_.w, sp_.h, sp_.tex) == (0, 0, NONE)
        ft = hs.textures.textures[a.tex]
        assert ft.psm == texconv.PSMT8 and ft.width >= 8 and f.tex_count == 1 and f.tex_first == a.tex
        # glyph pixels land where the record says: row 0 of 'A' = (65 + x + 0) | 1
        plane = ft.mips[0]
        assert plane[a.v0 * ft.width + a.u0:a.v0 * ft.width + a.u0 + 6] == bytes(((65 + x) & 0xFF) | 1 for x in range(6))
        # strings
        assert hs.lists[0][1] == ["Picked up an item", "Picked up %d items", ""] and hs.lists[1][1] == ["black", "white"]
        msgs = dict((tid, m) for tid, m in hs.message_sets)
        ui = msgs[ids["hmt :ui\\hud\\hud messages"]]
        assert ui[0].name == "pickup" and ui[0].segments == [(0, 0, "Hold "), (1, 18, ""), (0, 0, " to pick up "), (1, 32, ""), (0, 0, "")]
        assert ui[1].segments[2] == (0, 0, " to exit") and msgs[ids["hmt :levels\\t\\hud messages"]][0].segments == [(0, 0, "Look up")]


def test_sections() -> None:
    with tempfile.TemporaryDirectory() as d:
        hs, ids = collect_synthetic(Path(d))
        pack = hc.build_pack(hs)
        pk = hp2.parse(pack)
        assert [s.tag for s in pk.sections] == ["HUD0", "FONT", "STRS", "HUDT"]
        h0 = hc.parse_hud0(pk.read("HUD0"))
        assert len(h0["huds"]) == 7 and len(h0["sprites"]) == len(hs.sprites) and len(h0["links"]) == 6 and len(h0["sounds"]) == 2
        assert len(h0["icons"]) == 3 and h0["globals"]["icon_count"] == 3 and h0["globals"]["blip_sprite"] == len(hs.sprites) - 1
        for sec in ("HUD0", "FONT", "STRS", "HUDT"):
            raw = pk.read(sec)
            assert len(raw) % 16 == 0 or sec == "HUDT"
        # every record round-trips through the reference parser
        for h, rec in zip(hs.huds, h0["huds"]):
            assert (rec["tag"], rec["kind"], rec["anchor"], rec["child"]) == (h.tag_id, h.kind, h.anchor, h.child)
            els = h0["elems"][rec["elem_first"]:rec["elem_first"] + rec["elem_count"]]
            assert [(e["role"], e["x"], e["y"], e["w"], e["h"], e["colour"]) for e in els] == \
                [(e.role, e.x, e.y, e.w, e.h, e.colour) for e in h.elems + h.cross_elems]
            ms = h0["meters"][rec["meter_first"]:rec["meter_first"] + rec["meter_count"]]
            assert [(m["role"], m["x"], m["colour_min"], m["alpha_mult"], m["min_fraction"]) for m in ms] == \
                [(m.role, m.x, m.colour_min, m.alpha_mult, m.min_fraction) for m in h.meters]
            ns = h0["numbers"][rec["number_first"]:rec["number_first"] + rec["number_count"]]
            assert [(n["max_digits"], n["flags"], n["w"], n["colour"]) for n in ns] == [(n.max_digits, n.flags, n.w, n.colour) for n in h.numbers]
            cs = h0["crosses"][rec["cross_first"]:rec["cross_first"] + rec["cross_count"]]
            assert [c["type"] for c in cs] == [c.type for c in h.crosshairs]
            for c, cr in zip(cs, h.crosshairs):
                assert [e["seq"] for e in h0["elems"][c["elem_first"]:c["elem_first"] + c["elem_count"]]] == \
                    [e.seq for e in h.cross_elems[cr.elem_first:cr.elem_first + cr.elem_count]]
        fonts = hc.parse_font(pk.read("FONT"))
        assert len(fonts["fonts"]) == 2 and [g["code"] for g in fonts["fonts"][1]["glyphs"]] == [32, 63, 65, 66, 106]
        strs = hc.parse_strs(pk.read("STRS"))
        assert strs["lists"][0]["strings"] == ["Picked up an item", "Picked up %d items", ""]
        assert strs["sets"][0]["messages"][0]["segments"][1] == (1, 18, "")
        # HUDT is a valid TEXS blob the existing reader accepts
        view = texconv.parse_texs(pk.read("HUDT"))
        assert len(view.textures) == len(hs.textures.textures) and view.bindings == []
        assert all(t["mips"] == 1 for t in view.textures)
        if os.environ.get("HUD_FIXTURE"):
            Path(os.environ["HUD_FIXTURE"]).write_bytes(pack)
        # PNG renderer: textures, font specimen, previews
        out = Path(d) / "png"
        pp = Path(d) / "t.hud.hp2"
        pp.write_bytes(pack)
        files = hc.dump_png(pp, out)
        assert files and all(f.stat().st_size > 100 and f.read_bytes()[:8] == b"\x89PNG\r\n\x1a\n" for f in files)
        assert len(files) == len(view.textures) + 3
        w, h, rgba = hc.texture_rgba(view, 0)
        assert (w, h, len(rgba)) == (view.textures[0]["width"], view.textures[0]["height"], w * h * 4)


def test_problems() -> None:
    """Corrupt tags are reported and skipped, never raised past collect()."""
    def break_sequence(r: Region) -> None:                  # a static element asks for sequence 9 of a 3 sequence bitmap
        r.poke(r.block("unhi", "ui\\hud\\unit") + 0x8C + 0x54, "<H", 9)

    def break_reflexive(r: Region) -> None:                 # statics reflexive outside the region
        r.refl(r.block("wphi", "weapons\\rifle\\rifle") + 0x60, 1, 0x80000000)

    def break_crosshair_type(r: Region) -> None:
        ca = struct.unpack_from("<I", r.region, r.block("wphi", "ui\\hud\\master") + 0x84 + 4)[0] - BASE
        struct.pack_into("<H", r.region, ca, 40)

    def break_anchor(r: Region) -> None:
        r.poke(r.block("unhi", "ui\\hud\\vehicle"), "<H", 9)

    def break_font(r: Region) -> None:                      # glyph pixel offset that is not the running sum
        fb = r.block("font", "ui\\large_ui")
        ca = struct.unpack_from("<I", r.region, fb + 0x7C + 4)[0] - BASE
        struct.pack_into("<i", r.region, ca + 20 * 2 + 16, 3)

    def break_string(r: Region) -> None:                    # odd byte length
        b = r.block("ustr", "ui\\hud\\icons")
        ea = struct.unpack_from("<I", r.region, b + 4)[0] - BASE
        struct.pack_into("<I", r.region, ea, 7)

    def break_message(r: Region) -> None:                   # text units do not add up to the next message
        b = r.block("hmt ", "ui\\hud\\hud messages")
        ma = struct.unpack_from("<I", r.region, b + 0x20 + 4)[0] - BASE
        struct.pack_into("<H", r.region, ma + 64 + 0x20, 1)

    cases = [(break_sequence, "sequence 9"), (break_reflexive, "weapons\\rifle\\rifle"), (break_crosshair_type, "crosshair type"),
             (break_anchor, "anchor"), (break_font, "ui\\large_ui"), (break_string, "ui\\hud\\icons"), (break_message, "hud messages")]
    for mut, needle in cases:
        with tempfile.TemporaryDirectory() as d:
            hs, ids = collect_synthetic(Path(d), mut)
            assert any(needle in p for p in hs.problems), (mut.__name__, hs.problems)
            assert hc.verify(hs), mut.__name__
            hc.build_pack(hs)                                       # what survived still packs
    # a map without hud_globals is a hard error
    with tempfile.TemporaryDirectory() as d:
        r, pixels = build_world()
        r.region[r.block("matg", "globals\\globals") + 0x78C:r.block("matg", "globals\\globals") + 0x78C + 16] = bytes(12) + b"\xff\xff\xff\xff"
        mapfile = Path(d) / "t.map"
        mapfile.write_bytes(bytes(0x1000) + bytes(pixels))
        idx = tagindex.TagIndex.parse_region(bytes(r.region), 0x1000)
        m = texconv.MapData(mapfile, None, idx, bytes(r.region), {e.tag_id: e for e in idx.entries})  # type: ignore[arg-type]
        hs = hc.collect_map(m, "t")
        assert any("hud_globals" in p for p in hs.problems) and hc.verify(hs)
    # reference parsers reject damaged sections
    with tempfile.TemporaryDirectory() as d:
        hs, _ = collect_synthetic(Path(d))
        pk = hp2.parse(hc.build_pack(hs))
        raw = pk.read("HUD0")
        for bad in (raw[:60], b"XXXX" + raw[4:], raw[:8] + struct.pack("<I", len(raw) + 16) + raw[12:]):
            try:
                hc.parse_hud0(bad)
            except hc.HudError:
                continue
            raise SystemExit("damaged HUD0 accepted")
        for fn, sec in ((hc.parse_font, "FONT"), (hc.parse_strs, "STRS")):
            try:
                fn(pk.read(sec)[:40])
            except hc.HudError:
                continue
            raise SystemExit("damaged section accepted")


def test_real_maps() -> None:
    maps = sorted((ROOT / "assets" / "decompressed").glob("*.map"))
    campaign = [p for p in maps if p.stem in "a10 a30 a50 b30 b40 c10 c20 c40 d20 d40".split()]
    if not campaign:
        print("skip: no real maps (assets/decompressed missing)")
        return
    for p in campaign:
        hs = hc.collect(p)
        bad = hc.verify(hs)
        assert not bad, (p.stem, bad)
        pk = hp2.parse(hc.build_pack(hs))
        h0 = hc.parse_hud0(pk.read("HUD0"))
        assert len(h0["huds"]) >= 16 and len(h0["sprites"]) > 90 and len(h0["globals"]) and h0["globals"]["digit_count"] == 15
        texconv.parse_texs(pk.read("HUDT"))
        fonts = hc.parse_font(pk.read("FONT"))
        assert len(fonts["fonts"]) == 2 and all(len(f["glyphs"]) == 224 for f in fonts["fonts"])
        strs = hc.parse_strs(pk.read("STRS"))
        assert len(strs["lists"]) == 2 and len(strs["sets"]) == 2
        print(f"{p.stem}: {len(h0['huds'])} HUDs, {len(h0['sprites'])} sprites, {len(pk.read('HUDT')) // 1024} KiB HUDT")


if __name__ == "__main__":
    for name, fn in sorted(globals().items()):
        if name.startswith("test_") and callable(fn):
            fn()
            print("ok", name)
