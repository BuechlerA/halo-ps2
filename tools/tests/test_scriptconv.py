"""SCRP section on a synthetic scenario (no game data needed): the Xbox syntax-node layout is written by a small builder
(function-name nodes, salts, datum references, stale fields), converted, serialised, re-parsed and checked: constant
normalisation, pre-order node numbering, built-in table, external globals, script calls, unreachable and shared
nodes, defects the converter must report, the section writer and its reference parser, the usage report. Set
HS_FIXTURE=<path> to also write the .hp2 the C test (engine/tests/test_hs.c, which hard-codes the numbers of this world)
opens. A skip-if-missing check converts all ten real maps when assets/decompressed exists."""
import os
import struct
import sys
from collections import Counter, defaultdict
from pathlib import Path

sys.path[:0] = [str(Path(__file__).resolve().parents[1])]
from halo2ps2 import hp2, inventory, scriptconv as sc  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
MAPS = "a10 a30 a50 b30 b40 c10 c20 c40 d20 d40".split()


# ---- a synthetic scenario in the Xbox layout ---------------------------------------------------------
class Builder:
    """Writes syntax nodes (20 B), the string data and the script / global blocks exactly as the Xbox tag has them."""

    def __init__(self, dead: int = 3, free: int = 4) -> None:
        self.nodes: list[list[int]] = []              # salt, index, type, flags, next, string, data
        self.strings = bytearray(b"\0")
        self.scripts: list[tuple[str, int, int, int]] = []
        self.globals: list[tuple[str, int, int]] = []
        for _ in range(dead):                         # the unparsed source tokens at the start of the table
            self.nodes.append([0, 0xFFFF, 0, 1, 0xFFFFFFFF, 0, 0])
        self.free = free

    def s(self, text: str) -> int:
        off = len(self.strings)
        self.strings += text.encode() + b"\0"
        return off

    def ref(self, i: int) -> int:
        return (self.nodes[i][0] << 16) | i

    def add(self, index: int, typ: int, flags: int, nxt: int, string: str, data: int) -> int:
        i = len(self.nodes)
        self.nodes.append([0x7000 + i, index, typ, flags, nxt, self.s(string), data])
        return i

    def const(self, typ: int, data: int, text: str = "x") -> int:
        return self.add(typ, typ, 9, 0xFFFFFFFF, text, data)

    def chain(self, kids: list[int]) -> int:
        """Link siblings, return the datum reference of the first (or none)."""
        for a, b in zip(kids, kids[1:]):
            self.nodes[a][4] = self.ref(b)
        return self.ref(kids[0]) if kids else 0xFFFFFFFF

    def call(self, name: str, fid: int, ret: int, args: list[int], stale: int | None = None) -> int:
        head = self.add(fid if stale is None else stale, 2, 9, self.chain(args), name, 0)
        return self.add(fid, ret, 8, 0xFFFFFFFF, "(", self.ref(head))

    def scall(self, script: int, name: str, ret: int, args: list[int]) -> int:
        head = self.add(script, 2, 9, self.chain(args), name, 0)
        return self.add(script, ret, 10, 0xFFFFFFFF, "(", self.ref(head))

    def gref(self, data: int, typ: int, name: str) -> int:
        return self.add(0xFFFF, typ, 13, 0xFFFFFFFF, name, data)

    def script(self, name: str, typ: int, rtype: int, root: int | None) -> int:
        self.scripts.append((name, typ, rtype, 0xFFFFFFFF if root is None else self.ref(root)))
        return len(self.scripts) - 1

    def glob(self, name: str, typ: int, init: int | None) -> int:
        self.globals.append((name, typ, 0xFFFFFFFF if init is None else self.ref(init)))
        return len(self.globals) - 1

    def raw(self) -> sc.RawScripts:
        syn = bytearray(b"script node".ljust(32, b"\0") + struct.pack("<HHIHHHH", 0x4A39, 20, 1, 0, 0, 0, 0)[:24])
        syn = bytearray(syn.ljust(sc.SYNTAX_HEADER, b"\0"))
        for n in self.nodes:
            syn += struct.pack("<HHHHIII", *n)
        for _ in range(self.free):
            syn += struct.pack("<HHHHIII", 0xCACA, 0xCACA, 0xCACA, 0xCACA, 0xCACACACA, 0xCACACACA, 0xCACACACA)
        sblob = b"".join(struct.pack("<32sHHI52x", n.encode(), t, r, root) for n, t, r, root in self.scripts)
        gblob = b"".join(struct.pack("<32sHHII48x", n.encode(), t, 0, 0, init) for n, t, init in self.globals)
        return sc.parse_blocks(bytes(syn), bytes(self.strings), sblob, len(self.scripts), gblob, len(self.globals))


T_BOOL, T_REAL, T_SHORT, T_LONG, T_STRING, T_SCRIPT, T_VOID = 5, 6, 7, 8, 9, 10, 4
FID = dict(begin=0, begin_random=1, if_=2, set=4, plus=7, times=9, sleep=19, sleep_until=20, wake=21, not_=27,
           greater=15, fx_host=300, ai_place=161, camera_set=246)


def f32(x: float) -> int:
    return struct.unpack("<I", struct.pack("<f", x))[0]


EXPECT_NODES = (32, 11, 14, 1, 6)   # nodes, constants, calls, script calls, global references (counted by hand)


def fixture_world() -> Builder:
    """The world test_hs.c expects (see its test_fixture): 4 globals (1 external), 4 scripts, 7 built-ins."""
    b = Builder()
    # globals
    a_init = b.call("+", FID["plus"], T_LONG, [b.const(T_REAL, f32(1.0)), b.const(T_REAL, f32(2.0))])
    g_a = b.glob("fx_a", T_LONG, a_init)
    g_b = b.glob("fx_b", T_SHORT, b.const(T_SHORT, 0xFFFF0007))
    g_flag = b.glob("fx_flag", T_BOOL, b.const(T_BOOL, 0xFFFFFF00))
    # script 2: static, returns fx_b * 2 as a short
    static_root = b.call("begin", FID["begin"], T_SHORT, [
        b.call("*", FID["times"], T_SHORT, [b.gref(g_b, T_SHORT, "fx_b"), b.const(T_REAL, f32(2.0))])])
    # script 0: startup
    start_root = b.call("begin", FID["begin"], T_VOID, [
        b.call("set", FID["set"], T_VOID, [b.gref(g_flag, T_BOOL, "fx_flag"), b.const(T_BOOL, 0xFFFFFF01)]),
        b.call("sleep", FID["sleep"], T_VOID, [b.const(T_SHORT, 0xFFFF0003)]),
        b.call("set", FID["set"], T_VOID, [b.gref(g_a, T_LONG, "fx_a"), b.call("+", FID["plus"], T_LONG, [
            b.gref(g_a, T_REAL, "fx_a"), b.const(T_REAL, f32(10.0))])]),
        b.call("set", FID["set"], T_VOID, [b.gref(0xFFFF8003, T_REAL, "fx_engine"), b.const(T_REAL, f32(2.5))]),
        b.call("wake", FID["wake"], T_VOID, [b.const(T_SCRIPT, 0xFFFF0001, "fx_dorm")])])
    dorm_root = b.call("begin", FID["begin"], T_VOID, [
        b.call("set", FID["set"], T_VOID, [b.gref(g_b, T_SHORT, "fx_b"), b.scall(2, "fx_static", T_SHORT, [])])])
    cont_root = b.call("begin", FID["begin"], T_VOID, [b.call("fx_host", FID["fx_host"], T_VOID, [b.const(T_SHORT, 0xFFFF0001)])])
    assert b.script("fx_start", 0, T_VOID, start_root) == 0
    assert b.script("fx_dorm", 1, T_VOID, dorm_root) == 1
    assert b.script("fx_static", 3, T_SHORT, static_root) == 2
    assert b.script("fx_cont", 2, T_VOID, cont_root) == 3
    # an unreachable (garbage) expression the converter must drop
    b.call("ai_place", FID["ai_place"], T_VOID, [b.const(17, 0x80000000)])
    return b


# ---- checks ------------------------------------------------------------------------------------------
def check_fixture_world() -> bytes:
    b = fixture_world()
    c = sc.convert(b.raw())
    assert not c.problems, c.problems
    assert len(c.scripts) == 4 and len(c.globals) == 4 and sum(g.external for g in c.globals) == 1
    assert [g.name for g in c.globals] == ["fx_a", "fx_b", "fx_flag", "fx_engine"]
    assert c.globals[3].external and c.globals[3].cls == sc.C_REAL and c.globals[3].init == sc.NONE16
    names = [x.name for x in c.builtins]
    assert names == ["begin", "set", "+", "*", "sleep", "wake", "fx_host"], names
    assert [x.fid for x in c.builtins] == [0, 4, 7, 9, 19, 21, 300]                    # sorted by function id
    assert c.stale_heads == 0
    # the garbage call (ai_place) is not emitted
    n_const = sum(1 for n in c.nodes if n.kind == sc.K_CONST)
    n_call = sum(1 for n in c.nodes if n.kind == sc.K_CALL)
    n_sc = sum(1 for n in c.nodes if n.kind == sc.K_SCALL)
    n_gl = sum(1 for n in c.nodes if n.kind == sc.K_GLOBAL)
    assert "ai_place" not in names
    assert (len(c.nodes), n_const, n_call, n_sc, n_gl) == EXPECT_NODES, (len(c.nodes), n_const, n_call, n_sc, n_gl)
    # pre-order: first / next always point forward, argument counts match the chains
    for i, n in enumerate(c.nodes):
        for link in (n.next, n.first):
            assert link == sc.NONE16 or link > i
        k, a = 0, n.first
        while a != sc.NONE16:
            k, a = k + 1, c.nodes[a].next
        assert k == n.nargs
    # constants are normalised
    shorts = [n for n in c.nodes if n.kind == sc.K_CONST and n.cls == sc.C_SHORT]
    assert sorted(n.data for n in shorts) == sorted([7, 1, 3]), [hex(n.data) for n in shorts]
    bools = sorted(n.data for n in c.nodes if n.kind == sc.K_CONST and n.cls == sc.C_BOOL)
    assert bools == [0, 1]
    assert [n.data for n in c.nodes if n.kind == sc.K_CONST and n.cls == sc.C_SCRIPT] == [1]
    # the external global's reference was remapped to its slot after the scenario globals
    assert sum(1 for n in c.nodes if n.kind == sc.K_GLOBAL and n.data == 3) == 1
    # script call
    scall = [n for n in c.nodes if n.kind == sc.K_SCALL]
    assert len(scall) == 1 and scall[0].data == 2 and scall[0].nargs == 0
    blob = sc.build_section(c)
    sec = sc.parse_section(blob)
    assert sec.size == len(blob) and len(blob) % 16 == 0
    assert [s["name"] for s in sec.scripts] == ["fx_start", "fx_dorm", "fx_static", "fx_cont"]
    assert [s["type"] for s in sec.scripts] == [0, 1, 3, 2] and sec.type_counts == [1, 1, 1, 1, 0]
    assert [g["name"] for g in sec.globals] == ["fx_a", "fx_b", "fx_flag", "fx_engine"]
    assert sec.globals[3]["external"] and not sec.globals[0]["external"]
    assert [x["name"] for x in sec.builtins] == names and sec.builtins[0]["calls"] == c.builtins[0].calls
    assert sec.call_sites == sum(x.calls for x in c.builtins)
    assert len(sec.nodes) == len(c.nodes) and sec.classes == sc.CLASS_TABLE
    assert sec.classes[5] == sc.C_BOOL and sec.classes[38] == sc.C_HANDLE and sec.classes[4] == sc.C_VOID
    assert sec.nodes[sec.scripts[0]["root"]]["kind"] == sc.K_CALL
    # corrupt sections are rejected, never crash
    for cut in range(0, len(blob), 7):
        try:
            sc.parse_section(blob[:cut])
            raise AssertionError(f"truncation at {cut} accepted")
        except sc.ScriptError:
            pass
    for off in (0, 4, 8, 0x24, 0x38):
        bad = bytearray(blob)
        bad[off] ^= 0x55
        try:
            sc.parse_section(bytes(bad))
            raise AssertionError(f"corruption at {off:#x} accepted")
        except sc.ScriptError:
            pass
    if os.environ.get("HS_FIXTURE"):
        Path(os.environ["HS_FIXTURE"]).write_bytes(hp2.build("fx", [("SCRP", blob)]))
    pack = hp2.parse(hp2.build("fx", [("SCRP", blob)]))
    assert sc.parse_section(pack.read("SCRP")).size == len(blob)
    print(f"fixture world: {len(c.nodes)} nodes ({n_const} constants, {n_call} calls, {n_sc} script calls, {n_gl} global refs), "
          f"{len(blob)} bytes")
    return blob


def check_constants() -> None:
    b = Builder()
    consts = {
        "bool_true": b.const(T_BOOL, 0xFFFFFF01), "short_neg": b.const(T_SHORT, 0xFFFFFFFF), "short_hi": b.const(T_SHORT, 0xFFFF8000),
        "short_pos": b.const(T_SHORT, 0xFFFF001E), "long": b.const(T_LONG, 0x00B71B00), "real": b.const(T_REAL, f32(0.25)),
        "none": b.const(37, 0xFFFFFFFF), "obj16": b.const(38, 0xFFFF0031), "plain16": b.const(11, 0x7D),
        "tag": b.const(24, 0xECE30B6F), "ai": b.const(17, 0x80000000), "script": b.const(T_SCRIPT, 0xFFFF0010),
        "string": b.const(T_STRING, 0x117EE4E8, "some text"),
    }
    root = b.call("begin", FID["begin"], T_VOID, list(consts.values()))
    b.script("s", 0, T_VOID, root)
    c = sc.convert(b.raw())
    assert not c.problems
    kids = {}
    a = c.nodes[0].first
    for name in consts:
        kids[name] = c.nodes[a]
        a = c.nodes[a].next
    assert kids["bool_true"].data == 1 and kids["short_neg"].data == 0xFFFFFFFF and kids["short_hi"].data == 0xFFFF8000
    assert kids["short_pos"].data == 30 and kids["long"].data == 0xB71B00 and kids["real"].data == f32(0.25)
    assert kids["none"].data == 0xFFFFFFFF and kids["none"].cls == sc.C_HANDLE
    assert kids["obj16"].data == 0x31 and kids["plain16"].data == 0x7D and kids["tag"].data == 0xECE30B6F
    assert kids["ai"].data == 0x80000000 and kids["script"].data == 0x10 and kids["script"].cls == sc.C_SCRIPT
    sec = sc.parse_section(sc.build_section(c))
    assert sec.string(kids["string"].data) == "some text" and kids["string"].cls == sc.C_STRING
    assert kids["short_neg"].vtype == T_SHORT and kids["obj16"].vtype == 38


def check_shared_and_unreachable() -> None:
    b = Builder()
    shared = b.const(T_SHORT, 0xFFFF0002)
    first = b.call("sleep", FID["sleep"], T_VOID, [shared])
    root = b.call("begin", FID["begin"], T_VOID, [first])
    b.script("a", 0, T_VOID, root)
    b.script("b", 3, T_VOID, root)                  # two scripts with one root: both get their own copy
    b.call("wake", FID["wake"], T_VOID, [b.const(T_SCRIPT, 0xFFFF0000)])      # unreachable
    c = sc.convert(b.raw())
    assert not c.problems and len(c.nodes) == 2 * 3 and len(c.builtins) == 2
    assert c.scripts[0].root == 0 and c.scripts[1].root == 3
    assert c.live_nodes > len(c.nodes)


def check_defects() -> None:
    # call without a function-name node, name mismatch of a script call, unknown flags, bad datum salt, bad global
    b = Builder()
    bad_head = b.add(5, 5, 8, 0xFFFFFFFF, "(", 0xFFFFFFFF)                        # call with no head
    mism = b.scall(0, "wrong_name", T_VOID, [])
    flag = b.add(0, 7, 0x21, 0xFFFFFFFF, "?", 0)                                  # unsupported flags
    glob = b.gref(77, T_SHORT, "nope")                                            # global out of range, not external
    root = b.call("begin", FID["begin"], T_VOID, [bad_head, mism, flag, glob])
    b.script("only", 0, T_VOID, root)
    c = sc.convert(b.raw())
    assert len(c.problems) >= 4, c.problems
    assert any("function-name" in p for p in c.problems) and any("name mismatch" in p for p in c.problems)
    assert any("unsupported flags" in p for p in c.problems) and any("out of range" in p for p in c.problems)
    sc.build_section(c)                                 # still serialisable: defects became neutral constants
    # a salt that does not match the reference
    b = Builder()
    k = b.const(T_SHORT, 0xFFFF0001)
    root = b.call("begin", FID["begin"], T_VOID, [k])
    b.nodes[k][0] ^= 0x1234
    b.script("s", 0, T_VOID, root)
    assert any("salt" in p for p in sc.convert(b.raw()).problems)
    # a sibling chain that loops is cut, not followed forever
    b = Builder()
    k1, k2 = b.const(T_SHORT, 1), b.const(T_SHORT, 2)
    root = b.call("begin", FID["begin"], T_VOID, [k1, k2])
    b.nodes[k2][4] = b.ref(k1)
    b.script("s", 0, T_VOID, root)
    assert any("cyclic" in p for p in sc.convert(b.raw()).problems)
    # structural errors raise
    for syn in (b"", b"not a node table".ljust(80, b"\0")):
        try:
            sc.parse_blocks(syn, b"\0", b"", 0, b"", 0)
            raise AssertionError("bad table header accepted")
        except sc.ScriptError:
            pass
    try:
        sc.parse_blocks(b"script node".ljust(sc.SYNTAX_HEADER, b"\0"), b"\0", b"", 3, b"", 0)
        raise AssertionError("short script block accepted")
    except sc.ScriptError:
        pass
    # a stale function-name index is tolerated and counted (a30 / b30 have some)
    b = Builder()
    root = b.call("begin", FID["begin"], T_VOID, [b.call("sleep", FID["sleep"], T_VOID, [b.const(T_SHORT, 1)], stale=FID["sleep"] - 1)])
    b.script("s", 0, T_VOID, root)
    c = sc.convert(b.raw())
    assert not c.problems and c.stale_heads == 1 and [x.name for x in c.builtins] == ["begin", "sleep"]
    # a name given two function ids is reported
    b = Builder()
    root = b.call("begin", FID["begin"], T_VOID, [b.call("sleep", 19, T_VOID, []), b.call("sleep", 77, T_VOID, [])])
    b.script("s", 0, T_VOID, root)
    c = sc.convert(b.raw())
    assert len(c.builtins) == 3                                                    # distinct ids stay distinct


def check_domains_and_report() -> None:
    d = lambda n: sc.DOMAINS[sc.domain_of(n)]  # noqa: E731
    assert d("sleep") == d("begin") == "control" and d("and") == "logic" and d("+") == "math" and d("random_range") == "math"
    assert d("ai_place") == d("ai_living_count") == d("ai") == "ai" and d("unit_enter_vehicle") == d("vehicle_unload") == "unit"
    assert d("object_create") == d("volume_test_objects") == d("players") == "object" and d("device_set_position") == "device"
    assert d("camera_set") == d("cinematic_start") == d("recording_play") == "camera/cinematic"
    assert d("sound_impulse_start") == "sound" and d("show_hud_help_text") == d("hud_set_help_text") == "hud/ui"
    assert d("game_save") == d("switch_bsp") == "game/flow" and d("effect_new") == d("player_effect_start") == "effects"
    assert d("print") == d("rasterizer_lights_reset_for_new_map") == "debug/no-op"
    assert d("player_action_test_reset") == "player"
    assert len(sc.DOMAINS) == len(set(sc.DOMAINS)) == 14
    c = sc.convert(fixture_world().raw())
    text = sc.render_report({"fx": c, "fy": c})
    assert "| `fx_host` | game/flow" in text or "| `fx_host` | debug/no-op" in text
    assert "## Per domain" in text and "| fx | " in text and "| fy | " in text and "| `sleep` | control | 1 |" in text


def check_real_maps() -> None:
    maps = [ROOT / "assets" / "decompressed" / f"{m}.map" for m in MAPS]
    if not all(p.exists() for p in maps):
        print("skip real maps: assets/decompressed missing")
        return
    per = {}
    for p in maps:
        raw = sc.read_map(p)
        c = sc.convert(raw)
        assert not c.problems, (p.stem, c.problems)
        blob = sc.build_section(c)
        sec = sc.parse_section(blob)
        per[p.stem] = (raw, c, sec)
        assert len(sec.nodes) == len(c.nodes) and sec.type_counts[0] >= 1
        assert all(s["root"] != sc.NONE16 for s in sec.scripts)
        # every script root is a begin call of the script's own type, every global initialiser a constant or call
        for s in c.scripts:
            n = c.nodes[s.root]
            assert n.kind == sc.K_CALL and c.builtins[n.data].name == "begin" and n.vtype == s.rtype
        # the converter reaches every built-in call the inventory counts (it counts script-call heads too)
        inv = inventory.inventory(p)["scripts"]
        assert sum(inv["builtins"].values()) == sum(b.calls for b in c.builtins) + sum(1 for n in c.nodes if n.kind == sc.K_SCALL), p.stem
        # every expression node of the map's live source table is accounted for: kept ones <= live ones
        assert len(c.nodes) <= c.live_nodes
    a10 = per["a10"][1]
    assert (len(a10.nodes), len(a10.scripts), len(a10.globals)) == (12064, 185, 84)
    assert Counter(s.type for s in a10.scripts) == {0: 2, 1: 86, 2: 17, 3: 79, 4: 1}
    assert sum(b.calls for b in a10.builtins) == 5302 and len(a10.builtins) == 195
    union = {b.name for _, c, _ in per.values() for b in c.builtins}
    assert len(union) == 291, len(union)
    # a function index names exactly one built-in on all maps and back
    by_id, by_name = defaultdict(set), defaultdict(set)
    for _, c, _ in per.values():
        for b in c.builtins:
            by_id[b.fid].add(b.name)
            by_name[b.name].add(b.fid)
    assert all(len(v) == 1 for v in by_id.values()) and all(len(v) == 1 for v in by_name.values())
    # argument types are the same at every call site of a built-in (position by position), apart from the
    # polymorphic operators (comparison, set); this is what pins down the value type numbers
    pos = defaultdict(lambda: defaultdict(Counter))
    free_form = {"begin", "begin_random", "and", "or", "+", "if", "cond", "=", "!=", "<", ">", "<=", ">=", "set"}
    for _, c, _ in per.values():
        for n in c.nodes:
            if n.kind != sc.K_CALL:
                continue
            name = c.builtins[n.data].name
            if name in free_form:
                continue
            a, i = n.first, 0
            while a != sc.NONE16:
                pos[name][i][c.nodes[a].vtype] += 1
                a, i = c.nodes[a].next, i + 1
    for name, positions in pos.items():
        for i, kinds in positions.items():
            if len(kinds) > 1:
                # numeric promotion and unit -> object conversions are the only differences allowed
                assert set(kinds) <= {6, 7, 8} or set(kinds) <= {23, 37, 38} or set(kinds) <= {37, 38, 39, 41, 42, 43}, (name, i, dict(kinds))
    # arithmetic arguments are REAL on every call site (the compiler turns integer literals into reals)
    for _, c, _ in per.values():
        for n in c.nodes:
            if n.kind == sc.K_CALL and c.builtins[n.data].name in ("*", "-", "/", "max"):
                a = n.first
                while a != sc.NONE16:
                    assert c.nodes[a].vtype in (6, 7, 8) or c.nodes[a].kind == sc.K_CALL, c.nodes[a]
                    a = c.nodes[a].next
    # no script has parameters, no external global is a script global, global indices stay in range
    for _, c, sec in per.values():
        assert all(s["nparams"] == 0 for s in sec.scripts)
        ng = len(c.globals)
        assert all(n.data < ng for n in c.nodes if n.kind == sc.K_GLOBAL)
        assert all(n.data < len(c.scripts) for n in c.nodes if n.kind == sc.K_SCALL)
        assert all(c.scripts[n.data].type >= 3 for n in c.nodes if n.kind == sc.K_SCALL)    # script calls target static scripts
    print("real maps: ten maps convert, no problems; a10 12064 nodes / 185 scripts / 84 globals / 195 built-ins / 5302 calls;"
          f" {len(union)} distinct built-ins overall")


def main() -> None:
    check_fixture_world()
    check_constants()
    check_shared_and_unreachable()
    check_defects()
    check_domains_and_report()
    check_real_maps()
    print("ok")


if __name__ == "__main__":
    main()
