#!/usr/bin/env python3
"""Convert a scenario's compiled scripts (HSC) into the PS2 ``SCRP`` section, version 1
(docs/24-script-section.md; the reader and virtual machine are engine/src/script/hs.[ch]).

Source layout, engine version 5, **verified on all ten campaign maps** (offsets inside the scenario tag's
data block, pointers are absolute tag-space addresses, a reflexive is ``u32 count, u32 address, u32 pad``,
a "data" field is ``u32 size, u32 flags, u32 file offset, u32 address, u32 pad``):

    scnr +0x474 syntax data (size at +0x474, address at +0x480)   +0x488 string data (size, address at +0x494)
         +0x49C scripts (92 B)   +0x4A8 globals (92 B)   (+0x4B4 references, tag references: not used here)
    syntax data = 0x38-byte datum array header (``char[32] "script node"``, ``u16 max elements`` = 19001 on every
    map, ``u16 element size`` = 20, ...) then ``max elements`` nodes of 20 bytes:
        +0x00 u16 salt        datum identifier: a datum index is ``salt << 16 | index``; every reference checked
                              below carries the salt of the node it names (all of them do, on all ten maps)
        +0x02 u16 index       function index (call nodes, and the function-name node of a call), script index
                              (script calls); meaningless (stale) on other nodes
        +0x04 u16 type        value type of the expression (see VALUE TYPES); 2 = function name, 0xCACA = free slot
        +0x06 u16 flags       bit 0 primitive, bit 1 script call, bit 2 global reference, bit 3 "do not
                              garbage collect" (set on every live node). Live nodes have exactly the values
                              9 (constant, function name), 8 (call), 10 (script call), 13 (global reference);
                              nodes with salt 0 (the first nodes of the table) are the unparsed source tokens of
                              the script definitions and are never referenced
        +0x08 u32 next        datum index of the next sibling (0xFFFFFFFF none)
        +0x0C u32 string      offset into the string data (the token text; for numbers and call nodes it is a
                              source position, not a usable string)
        +0x10 u32 data        value / child reference, see below
    expression forms (verified by walking every script root and global initialiser of every map; no node is
    shared between two parents, every list ends, every referenced node has the referenced salt):
      * call: flags 8, ``index`` = function index, ``type`` = result type, ``data`` = datum of a function-name
        node (flags 9, type 2, same ``index``, ``string`` = built-in name); that node's ``next`` chain are the
        arguments. A function index maps to exactly one built-in name on all ten maps and back (291 distinct
        names, indices 0..415).
      * script call: flags 10, ``index`` = script index, ``data`` = a function-name node (type 2) naming the script
        (name equals the script block's name on 999 of 999 calls); ``next`` chain of that node = arguments (none:
        no script in the ten maps has parameters). The call's ``type`` is the type the call site expects and can
        differ from the script's return type (a unit used as object or object list, 142 of 999).
      * global reference: flags 13, ``data`` = index into the scenario globals (3996 of 4000, name and the global's
        type agree except numeric promotion, see below) or, for the 4 others, ``0xFFFF8000 | n`` = an engine
        (external) global such as a cheat or rasteriser variable; ``string`` is its name.
      * constant: flags 9, type != 2. data by type: bool low byte; real IEEE float; short low 16 bits (high 16
        bits are stale: 0xFFFF on every map); long 32 bits; string = a runtime pointer, the text is at ``string``
        (data - string is constant per map); script = script index (low 16 bits); every other type is a handle:
        16-bit indices (trigger volumes, flags, names...) either plain or with the stale high half 0xFFFF
        (0xFFFFFFFF = none), 32-bit tag ids (high half 0xE000..0xEFFF) or AI references (bit 31 set).
      * Arithmetic and comparison functions take REAL arguments (the compiler turns integer literals into real
        constants); the call node's type (short, long, real) is the type the result is converted to.
        A node read of a global may be typed real while the global is short, or short while it is long
        (numeric promotion: 397 of 3996): the evaluator converts to the node's type.
    script (92 B): ``char[32] name, u16 type, u16 return type, u32 root expression`` (datum of a ``begin`` call
        on every one of 1938 scripts) and 52 zero bytes; type 0 startup, 1 dormant, 2 continuous, 3 static, 4 stub
        (counts per map in docs/24). global (92 B): ``char[32] name, u16 type, u16 0, u32 0, u32 initialiser
        expression`` (datum of a constant, 2 globals: of a global reference, 63: of a call), 48 zero bytes.

VALUE TYPES (u16, as the engine numbers them). The class column is what the virtual machine needs; the names
are inferred from consistency (the type of the arguments a built-in receives at every call site, the result type
of well-known built-ins, the token class of constants) and only the numbered ones below were checked that way:
    4 void, 5 bool, 6 real, 7 short, 8 long, 9 string, 10 script, 11 trigger volume, 12 cutscene flag,
    13 cutscene camera point, 14 cutscene title, 15 cutscene recording, 16 device group, 17 ai, 18 ai command
    list, 19 starting profile, 20 conversation, 21 navpoint, 22 hud message, 23 object list, 24 sound,
    25 effect, 26 damage, 27 looping sound, 28 animation graph, 29 actor variant, 31 object definition,
    32 game difficulty, 33 team, 37 object, 38 unit, 39..43 object name families (see docs/24).
    Classes: void, bool, real, short, long, string, script, handle (everything else).

SCRP layout (all little endian, tables 16-byte aligned, offsets relative to the section start):
    header 128 B: "SCRP", u16 version 1, u16 0, u32 total size, u32 node count, u32 script count, u32 global
        count (scenario globals, then external ones), u32 builtin count, u32 external global count, u32 string
        bytes, u32 offsets: nodes 0x24, scripts 0x28, globals 0x2C, builtins 0x30, strings 0x34, classes 0x38;
        u32 call sites 0x3C; u16 script counts per type (5) at 0x40.
    node 16 B: u8 kind (0 constant, 1 call, 2 script call, 3 global, 4 parameter), u8 class, u8 raw value type, u8 0,
        u16 next sibling, u16 first argument (0xFFFF none), u16 argument count, u16 0, u32 data.
        data: constant value (bool 0/1, real float bits, short sign-extended, long, string pool offset, script
        index, handle) | built-in index | script index | global index | parameter slot.
        Nodes are in pre-order: first and next always point to a higher index, so a section has no cycles.
    script 32 B: u32 name offset, u16 type, u16 raw return type, u16 root node (0xFFFF none), u8 return class,
        u8 parameter count (0 on all maps), u8 parameter classes[12], 8 pad.
    global 16 B: u32 name offset, u16 raw type, u8 class, u8 flags (bit 0 external), u16 initialiser node, 2 pad.
    builtin 16 B: u32 name offset, u32 call sites in this map, u8 domain, u8 min args, u8 max args, 5 pad.
    classes: 64 bytes, class of each raw value type 0..63 (the table the converter used).
    strings: NUL separated, offset 0 is the empty string.

Usage:
    python3 -I tools/halo2ps2/scriptconv.py assets/decompressed/a10.map -o assets/hp2/a10.script.hp2
    python3 -I tools/halo2ps2/scriptconv.py --info assets/decompressed/a10.map
    python3 -I tools/halo2ps2/scriptconv.py --report docs/reports/script-usage.md assets/decompressed/*.map
"""
from __future__ import annotations

import argparse
import struct
import sys
from collections import Counter, defaultdict
from dataclasses import dataclass, field
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from halo2ps2 import hp2, mapinfo, tagindex  # noqa: E402

BASE = mapinfo.XBOX_TAG_BASE
MAGIC = b"SCRP"
VERSION = 1
HEADER_SIZE = 128
NODE_REC, SCRIPT_REC, GLOBAL_REC, BUILTIN_REC = 16, 32, 16, 16
NONE16 = 0xFFFF
MAX_NODES = 0xFFFE
MAX_SCRIPTS = 512
MAX_GLOBALS = 256
MAX_BUILTINS = 512
SYNTAX_HEADER = 0x38
SRC_NODE = 20
FREE_TYPE = 0xCACA

# scenario offsets
SC_SYNTAX_SIZE, SC_SYNTAX_ADDR, SC_STRING_SIZE, SC_STRING_ADDR = 0x474, 0x480, 0x488, 0x494
SC_SCRIPTS, SC_GLOBALS = 0x49C, 0x4A8
SCRIPT_BLOCK, GLOBAL_BLOCK = 92, 92

# source node flags
F_PRIMITIVE, F_SCRIPT_CALL, F_GLOBAL, F_KEEP = 1, 2, 4, 8
# SCRP node kinds
K_CONST, K_CALL, K_SCALL, K_GLOBAL, K_PARAM = range(5)
# value classes (mirror hs_type in engine/src/script/hs.h)
C_VOID, C_BOOL, C_REAL, C_SHORT, C_LONG, C_STRING, C_SCRIPT, C_HANDLE = range(8)
CLASS_NAMES = ["void", "bool", "real", "short", "long", "string", "script", "handle"]
SCRIPT_TYPES = ["startup", "dormant", "continuous", "static", "stub"]

T_FUNCTION_NAME = 2
VALUE_CLASS = {4: C_VOID, 5: C_BOOL, 6: C_REAL, 7: C_SHORT, 8: C_LONG, 9: C_STRING, 10: C_SCRIPT}
# type names that were verified by consistency (see the docstring); the rest print as numbers
VALUE_NAMES = {4: "void", 5: "bool", 6: "real", 7: "short", 8: "long", 9: "string", 10: "script",
               11: "trigger_volume", 12: "cutscene_flag", 13: "camera_point", 14: "cutscene_title",
               15: "recording", 16: "device_group", 17: "ai", 18: "ai_command_list", 19: "starting_profile",
               20: "conversation", 21: "navpoint", 22: "hud_message", 23: "object_list", 24: "sound",
               25: "effect", 26: "damage", 27: "looping_sound", 28: "animation_graph", 29: "actor_variant",
               31: "object_definition", 32: "game_difficulty", 33: "team", 37: "object", 38: "unit"}


def vname(t: int) -> str:
    return VALUE_NAMES.get(t, f"t{t}")


def value_class(t: int) -> int:
    if t in VALUE_CLASS:
        return VALUE_CLASS[t]
    return C_VOID if t < 4 else C_HANDLE


CLASS_TABLE = bytes(value_class(t) for t in range(64))

# ---- domains of the usage report ----------------------------------------------------------------
DOMAINS = ["control", "math", "logic", "object", "unit", "player", "ai", "device", "camera/cinematic",
           "sound", "hud/ui", "game/flow", "effects", "debug/no-op"]
D = {n: i for i, n in enumerate(DOMAINS)}

_EXACT = {
    **{n: "control" for n in ("begin", "begin_random", "if", "cond", "set", "sleep", "sleep_until",
                              "sleep_forever", "wake")},
    **{n: "logic" for n in ("and", "or", "not")},
    **{n: "math" for n in ("+", "-", "*", "/", "min", "max", "=", "!=", "<", ">", "<=", ">=",
                           "random_range", "real_random_range")},
    **{n: "debug/no-op" for n in ("print", "inspect", "cls", "breakable_surfaces_enable",
                                  "breakable_surfaces_reset", "objects_predict", "object_type_predict",
                                  "object_pvs_activate", "object_beautify")},
    "object_pvs_clear": "debug/no-op", "object_pvs_set_camera": "debug/no-op",
    "object_pvs_set_object": "debug/no-op", "scenery_animation_start": "camera/cinematic",
    "scenery_animation_start_at_frame": "camera/cinematic", "garbage_collect_now": "debug/no-op",
    "players": "object", "unit": "unit", "list_get": "object", "list_count": "object",
    "damage_new": "effects", "damage_object": "effects", "structure_bsp_index": "game/flow",
    "switch_bsp": "game/flow", "volume_teleport_players_not_inside": "object",
    "custom_animation": "camera/cinematic", "custom_animation_list": "camera/cinematic",
    "fade_in": "camera/cinematic", "fade_out": "camera/cinematic",
    "time_code_show": "hud/ui", "time_code_start": "hud/ui", "time_code_reset": "hud/ui",
    "enable_hud_help_flash": "hud/ui", "display_scenario_help": "hud/ui", "show_hud": "hud/ui",
    "show_hud_help_text": "hud/ui", "show_hud_timer": "hud/ui", "pause_hud_timer": "hud/ui",
    "numeric_countdown_timer_set": "hud/ui", "numeric_countdown_timer_stop": "hud/ui",
    "activate_team_nav_point_flag": "hud/ui", "deactivate_team_nav_point_flag": "hud/ui",
    "activate_team_nav_point_object": "hud/ui", "deactivate_team_nav_point_object": "hud/ui",
    "vehicle_riders": "unit", "recording_play": "camera/cinematic",
    "device_group_set": "device", "device_group_get": "device",
    "game_difficulty_get": "game/flow", "game_difficulty_get_real": "game/flow",
    "player_effect_start": "effects", "player_effect_stop": "effects",
    "player_effect_set_max_rotation": "effects", "player_effect_set_max_rumble": "effects",
    "player_effect_set_max_translation": "effects",
}
_PREFIX = (
    ("ai_", "ai"), ("unit_", "unit"), ("units_", "unit"), ("vehicle_", "unit"),
    ("player_effect_", "effects"), ("player", "player"), ("players_", "player"),
    ("object_", "object"), ("objects_", "object"), ("list_", "object"), ("volume_", "object"),
    ("device_", "device"), ("camera_", "camera/cinematic"), ("cinematic_", "camera/cinematic"),
    ("recording_", "camera/cinematic"), ("sound_", "sound"), ("hud_", "hud/ui"), ("show_hud", "hud/ui"),
    ("game_", "game/flow"), ("effect_", "effects"), ("rasterizer_", "debug/no-op"),
    ("cheat_", "debug/no-op"), ("debug_", "debug/no-op"),
)


def domain_of(name: str) -> int:
    d = _EXACT.get(name)
    if d:
        return D[d]
    for p, dom in _PREFIX:
        if name.startswith(p):
            return D[dom]
    return D["ai"] if name == "ai" else D["debug/no-op"]


class ScriptError(ValueError):
    pass


# ---- raw (Xbox) blocks ----------------------------------------------------------------------------
@dataclass
class RawNode:
    salt: int
    index: int
    type: int
    flags: int
    next: int
    string: int
    data: int


@dataclass
class RawScripts:
    nodes: list[RawNode]
    strings: bytes
    scripts: list[dict]            # name, type, rtype, root (datum), extra (bytes after the root)
    globals: list[dict]            # name, type, init (datum), extra
    problems: list[str] = field(default_factory=list)

    def string(self, off: int) -> str:
        if off >= len(self.strings):
            return ""
        end = self.strings.find(b"\0", off)
        return self.strings[off:end if end >= 0 else len(self.strings)].decode("latin1")


def parse_blocks(syntax: bytes, strings: bytes, scripts: bytes, nscripts: int, globals_: bytes,
                 nglobals: int) -> RawScripts:
    if len(syntax) < SYNTAX_HEADER or not syntax.startswith((b"script node", b"static script node")):
        raise ScriptError("script syntax data lacks a script node table header")
    nn = (len(syntax) - SYNTAX_HEADER) // SRC_NODE
    nodes = [RawNode(*struct.unpack_from("<HHHHIII", syntax, SYNTAX_HEADER + i * SRC_NODE)) for i in range(nn)]
    if len(scripts) < nscripts * SCRIPT_BLOCK or len(globals_) < nglobals * GLOBAL_BLOCK:
        raise ScriptError("script or global block shorter than its element count")
    raw = RawScripts(nodes, strings, [], [])
    for i in range(nscripts):
        b = scripts[i * SCRIPT_BLOCK:(i + 1) * SCRIPT_BLOCK]
        stype, rtype, root = struct.unpack_from("<HHI", b, 32)
        raw.scripts.append(dict(name=b[:32].split(b"\0", 1)[0].decode("latin1"), type=stype, rtype=rtype,
                                root=root, extra=b[40:]))
    for i in range(nglobals):
        b = globals_[i * GLOBAL_BLOCK:(i + 1) * GLOBAL_BLOCK]
        gtype, _p, _p2, init = struct.unpack_from("<HHII", b, 32)
        raw.globals.append(dict(name=b[:32].split(b"\0", 1)[0].decode("latin1"), type=gtype, init=init,
                                extra=b[44:], pad=(_p, _p2)))
    return raw


def read_map(path: Path) -> RawScripts:
    hdr = mapinfo.read_header(path)
    idx = tagindex.TagIndex.read(path)
    with path.open("rb") as f:
        f.seek(hdr.tag_data_offset)
        data = f.read(hdr.tag_data_size)

    def u32(off: int) -> int:
        return struct.unpack_from("<I", data, off)[0]

    def at(addr: int, size: int, what: str) -> bytes:
        off = addr - BASE
        if size == 0:
            return b""
        if off < 0 or off + size > len(data):
            raise ScriptError(f"{what}: {size} bytes at {addr:#x} outside the tag region")
        return data[off:off + size]

    sc = idx.scenario.data_address - BASE
    syntax = at(u32(sc + SC_SYNTAX_ADDR), u32(sc + SC_SYNTAX_SIZE), "syntax data")
    strings = at(u32(sc + SC_STRING_ADDR), u32(sc + SC_STRING_SIZE), "string data")
    ns, na = u32(sc + SC_SCRIPTS), u32(sc + SC_SCRIPTS + 4)
    ng, ga = u32(sc + SC_GLOBALS), u32(sc + SC_GLOBALS + 4)
    return parse_blocks(syntax, strings, at(na, ns * SCRIPT_BLOCK, "scripts"), ns,
                        at(ga, ng * GLOBAL_BLOCK, "globals"), ng)


# ---- conversion ------------------------------------------------------------------------------------
@dataclass
class Node:
    kind: int
    cls: int
    vtype: int
    data: int
    next: int = NONE16
    first: int = NONE16
    nargs: int = 0


@dataclass
class Script:
    name: str
    type: int
    rtype: int
    root: int
    rcls: int


@dataclass
class Global:
    name: str
    vtype: int
    cls: int
    external: bool
    init: int


@dataclass
class Builtin:
    name: str
    calls: int
    domain: int
    min_args: int
    max_args: int
    arg_types: set = field(default_factory=set)      # raw value types seen as arguments
    ret_types: set = field(default_factory=set)      # raw value types seen as results
    fid: int = 0


@dataclass
class Converted:
    nodes: list[Node]
    scripts: list[Script]
    globals: list[Global]
    builtins: list[Builtin]
    strings: bytes
    problems: list[str]
    live_nodes: int = 0          # nodes of the source table the map uses (salt != 0, not free)
    stale_heads: int = 0         # function-name nodes whose index differs from their call's


class _Strings:
    def __init__(self, blob: bytes = b"\0") -> None:
        self.blob = bytearray(blob)
        self.index: dict[str, int] = {}
        off = 0
        for part in blob.split(b"\0")[:-1]:
            self.index.setdefault(part.decode("latin1"), off)
            off += len(part) + 1

    def add(self, s: str) -> int:
        if s not in self.index:
            self.index[s] = len(self.blob)
            self.blob += s.encode("latin1", "replace") + b"\0"
        return self.index[s]


def _const_value(raw: RawScripts, n: RawNode, cls: int, pool: _Strings) -> int:
    d = n.data
    if cls == C_VOID:
        return 0
    if cls == C_BOOL:
        return d & 0xFF and 1
    if cls == C_REAL:
        return d
    if cls == C_SHORT:
        v = d & 0xFFFF
        return (v - 0x10000 if v & 0x8000 else v) & 0xFFFFFFFF
    if cls == C_LONG:
        return d
    if cls == C_STRING:
        return pool.add(raw.string(n.string))
    if cls == C_SCRIPT:
        return d & 0xFFFF
    if (d >> 16) == 0xFFFF and d != 0xFFFFFFFF:      # stale high half of a 16-bit index
        return d & 0xFFFF
    return d


def convert(raw: RawScripts) -> Converted:
    """Walk every script root and global initialiser, emit the reachable expression trees in pre-order."""
    N = raw.nodes
    problems = list(raw.problems)
    pool = _Strings()
    out: list[Node] = []
    builtin_by_fid: dict[int, Builtin] = {}
    ext: dict[int, tuple[str, int]] = {}
    ngl = len(raw.globals)
    stale = [0]

    def prob(msg: str) -> None:
        if len(problems) < 50:
            problems.append(msg)

    def node_at(ref: int, what: str) -> RawNode | None:
        """Resolve a datum index, checking range and salt."""
        if ref == 0xFFFFFFFF:
            return None
        i = ref & 0xFFFF
        if i >= len(N):
            prob(f"{what}: node {i} outside the table")
            return None
        if N[i].salt != ref >> 16:
            prob(f"{what}: node {i} has salt {N[i].salt:#x}, referenced as {ref >> 16:#x}")
        return N[i]

    def children(first_ref: int, what: str) -> list[int]:
        """Datum indices of a sibling chain starting at ``first_ref`` (bounded, loop-safe)."""
        refs, seen, ref = [], set(), first_ref
        while ref != 0xFFFFFFFF:
            i = ref & 0xFFFF
            if i in seen or i >= len(N) or len(refs) > 4096:
                prob(f"{what}: bad or cyclic sibling chain")
                break
            seen.add(i)
            refs.append(ref)
            ref = N[i].next
        return refs

    def emit(ref: int, depth_guard: set[int], what: str) -> int:
        """Emit the expression rooted at datum ``ref``; returns its node index in ``out`` (iterative pre-order)."""
        first_index = len(out)
        last_child: dict[int, int] = {}
        pending: list[tuple[int, int | None]] = [(ref, None)]
        guard = 0
        while pending:
            r, parent = pending.pop()
            guard += 1
            if guard > 200000:
                raise ScriptError(f"{what}: expression too large or cyclic")
            n = node_at(r, what)
            idx = len(out)
            if idx > MAX_NODES:
                raise ScriptError("more than 65534 expression nodes")
            if n is None:
                out.append(Node(K_CONST, C_VOID, 4, 0))
            else:
                fl, ty = n.flags, n.type
                cls = value_class(ty)
                if fl == (F_PRIMITIVE | F_KEEP) and ty != T_FUNCTION_NAME:
                    out.append(Node(K_CONST, cls, ty, _const_value(raw, n, cls, pool)))
                elif fl == F_KEEP or fl == (F_SCRIPT_CALL | F_KEEP):
                    is_script = fl != F_KEEP
                    head = node_at(n.data, what + " call head")
                    if head is not None and head.type == T_FUNCTION_NAME and head.index != n.index:
                        stale[0] += 1          # the call's own index is the authoritative one (docstring)
                    if head is None or head.type != T_FUNCTION_NAME:
                        prob(f"{what}: call node {r & 0xFFFF} without a function-name node")
                        out.append(Node(K_CONST, cls, ty, 0))
                        head = None
                    else:
                        kids = children(head.next, what)
                        if is_script:
                            if n.index >= len(raw.scripts):
                                prob(f"{what}: script call to script {n.index} of {len(raw.scripts)}")
                                out.append(Node(K_CONST, cls, ty, 0))
                                kids = []
                            else:
                                if raw.scripts[n.index]["name"] != raw.string(head.string):
                                    prob(f"{what}: script call {n.index} name mismatch")
                                out.append(Node(K_SCALL, cls, ty, n.index, nargs=len(kids)))
                        else:
                            b = builtin_by_fid.get(n.index)
                            name = raw.string(head.string)
                            if b is None:
                                b = builtin_by_fid[n.index] = Builtin(name, 0, domain_of(name), 255, 0, fid=n.index)
                            elif b.name != name:
                                prob(f"{what}: function {n.index} named both {b.name!r} and {name!r}")
                            b.calls += 1
                            b.min_args, b.max_args = min(b.min_args, len(kids)), max(b.max_args, len(kids))
                            b.ret_types.add(ty)
                            for k in kids:
                                b.arg_types.add(N[k & 0xFFFF].type)
                            out.append(Node(K_CALL, cls, ty, n.index, nargs=len(kids)))
                        for k in reversed(kids):
                            pending.append((k, idx))
                elif fl == (F_PRIMITIVE | F_GLOBAL | F_KEEP):
                    d = n.data
                    if d < ngl:
                        g = raw.globals[d]
                        if g["name"] != raw.string(n.string):
                            prob(f"{what}: global {d} name mismatch")
                        out.append(Node(K_GLOBAL, cls, ty, d))
                    elif d & 0x8000:
                        key = d & 0x7FFF
                        ext.setdefault(key, (raw.string(n.string), ty))
                        out.append(Node(K_GLOBAL, cls, ty, 0x80000000 | key))     # patched below
                    else:
                        prob(f"{what}: global reference {d:#x} out of range")
                        out.append(Node(K_CONST, cls, ty, 0))
                else:
                    prob(f"{what}: node {r & 0xFFFF} has unsupported flags {fl:#x} type {ty}")
                    out.append(Node(K_CONST, cls, ty, 0))
            if parent is not None:
                prev = last_child.get(parent)
                if prev is None:
                    out[parent].first = idx
                else:
                    out[prev].next = idx
                last_child[parent] = idx
        return first_index

    scripts: list[Script] = []
    for si, s in enumerate(raw.scripts):
        if any(s["extra"]):
            prob(f"script {si}: non-zero bytes after the root expression (parameters?)")
        if s["type"] > 4:
            prob(f"script {si}: type {s['type']}")
        root = emit(s["root"], set(), f"script {si}") if s["root"] != 0xFFFFFFFF else NONE16
        scripts.append(Script(s["name"], s["type"], s["rtype"], root, value_class(s["rtype"])))
    globs: list[Global] = []
    for gi, g in enumerate(raw.globals):
        if any(g["extra"]) or any(g["pad"]):
            prob(f"global {gi}: unexpected non-zero padding")
        init = emit(g["init"], set(), f"global {gi}") if g["init"] != 0xFFFFFFFF else NONE16
        globs.append(Global(g["name"], g["type"], value_class(g["type"]), False, init))
    ext_order = sorted(ext)
    ext_index = {k: ngl + i for i, k in enumerate(ext_order)}
    for k in ext_order:
        name, ty = ext[k]
        globs.append(Global(name, ty, value_class(ty), True, NONE16))
    for n in out:
        if n.kind == K_GLOBAL and n.data & 0x80000000:
            n.data = ext_index[n.data & 0x7FFF]
    fids = sorted(builtin_by_fid)
    bindex = {f: i for i, f in enumerate(fids)}
    for n in out:
        if n.kind == K_CALL:
            n.data = bindex[n.data]
    builtins = [builtin_by_fid[f] for f in fids]
    if len(out) > MAX_NODES or len(scripts) > MAX_SCRIPTS or len(globs) > MAX_GLOBALS or len(builtins) > MAX_BUILTINS:
        raise ScriptError("script tables exceed the engine limits")
    live = sum(1 for n in N if n.salt != 0 and n.type != FREE_TYPE)
    return Converted(out, scripts, globs, builtins, bytes(pool.blob), problems, live, stale[0])


# ---- writer ----------------------------------------------------------------------------------------
def _pad16(b: bytearray) -> None:
    b += bytes(-len(b) % 16)


def build_section(c: Converted) -> bytes:
    pool = _Strings(c.strings)      # constants' strings sit at fixed offsets; names are appended
    out = bytearray(HEADER_SIZE)
    offs = {}
    offs["nodes"] = len(out)
    for n in c.nodes:
        out += struct.pack("<BBBBHHHHI", n.kind, n.cls, n.vtype, 0, n.next, n.first, n.nargs, 0, n.data)
    _pad16(out)
    offs["scripts"] = len(out)
    for s in c.scripts:
        out += struct.pack("<IHHHBB12s8x", pool.add(s.name), s.type, s.rtype, s.root, s.rcls, 0, bytes(12))
    _pad16(out)
    offs["globals"] = len(out)
    for g in c.globals:
        out += struct.pack("<IHBBH6x", pool.add(g.name), g.vtype, g.cls, 1 if g.external else 0, g.init)
    _pad16(out)
    offs["builtins"] = len(out)
    for b in c.builtins:
        out += struct.pack("<IIBBB5x", pool.add(b.name), b.calls, b.domain, min(b.min_args, 255), b.max_args)
    _pad16(out)
    offs["classes"] = len(out)
    out += CLASS_TABLE
    _pad16(out)
    offs["strings"] = len(out)
    strings = bytes(pool.blob)
    out += strings
    _pad16(out)
    counts = Counter(s.type for s in c.scripts)
    call_sites = sum(b.calls for b in c.builtins)
    struct.pack_into("<4sHHIIIIIII", out, 0, MAGIC, VERSION, 0, len(out), len(c.nodes), len(c.scripts),
                     len(c.globals), len(c.builtins), sum(1 for g in c.globals if g.external), len(strings))
    struct.pack_into("<IIIIII", out, 0x24, offs["nodes"], offs["scripts"], offs["globals"], offs["builtins"],
                     offs["strings"], offs["classes"])
    struct.pack_into("<I5H", out, 0x3C, call_sites, *(counts.get(i, 0) for i in range(5)))
    return bytes(out)


@dataclass
class Section:
    size: int
    nodes: list[dict]
    scripts: list[dict]
    globals: list[dict]
    builtins: list[dict]
    strings: bytes
    classes: bytes
    call_sites: int
    type_counts: list[int]

    def string(self, off: int) -> str:
        end = self.strings.find(b"\0", off)
        return self.strings[off:end].decode("latin1")


def parse_section(data: bytes) -> Section:
    """Reference reader (the C reader is engine/src/script/hs.c); raises ScriptError on a bad section."""
    if len(data) < HEADER_SIZE:
        raise ScriptError("section shorter than the header")
    magic, version, _r, size, nn, ns, ng, nb, nx, nstr = struct.unpack_from("<4sHHIIIIIII", data)
    if magic != MAGIC:
        raise ScriptError("bad signature")
    if version != VERSION:
        raise ScriptError(f"unsupported version {version}")
    if size != len(data):
        raise ScriptError("inconsistent size")
    o_nodes, o_sc, o_gl, o_bi, o_str, o_cls = struct.unpack_from("<6I", data, 0x24)
    for off, cnt, rec in ((o_nodes, nn, NODE_REC), (o_sc, ns, SCRIPT_REC), (o_gl, ng, GLOBAL_REC),
                          (o_bi, nb, BUILTIN_REC), (o_str, nstr, 1), (o_cls, 64, 1)):
        if off % 16 or off < HEADER_SIZE or off + cnt * rec > len(data):
            raise ScriptError("table outside the section")
    nodes = []
    for i in range(nn):
        kind, cls, vt, _z, nxt, first, nargs, _z2, d = struct.unpack_from("<BBBBHHHHI", data, o_nodes + i * NODE_REC)
        nodes.append(dict(kind=kind, cls=cls, vtype=vt, next=nxt, first=first, nargs=nargs, data=d))
    scripts = []
    for i in range(ns):
        name, st, rt, root, rcls, npar = struct.unpack_from("<IHHHBB", data, o_sc + i * SCRIPT_REC)
        scripts.append(dict(name_off=name, type=st, rtype=rt, root=root, rcls=rcls, nparams=npar))
    globs = []
    for i in range(ng):
        name, vt, cls, fl, init = struct.unpack_from("<IHBBH", data, o_gl + i * GLOBAL_REC)
        globs.append(dict(name_off=name, vtype=vt, cls=cls, external=bool(fl & 1), init=init))
    builtins = []
    for i in range(nb):
        name, calls, dom, mn, mx = struct.unpack_from("<IIBBB", data, o_bi + i * BUILTIN_REC)
        builtins.append(dict(name_off=name, calls=calls, domain=dom, min_args=mn, max_args=mx))
    strings = data[o_str:o_str + nstr]
    sec = Section(size, nodes, scripts, globs, builtins, strings, data[o_cls:o_cls + 64],
                  struct.unpack_from("<I", data, 0x3C)[0], list(struct.unpack_from("<5H", data, 0x40)))
    for d in scripts + globs + builtins:
        d["name"] = sec.string(d["name_off"])
    return sec


def convert_map(path: Path) -> Converted:
    return convert(read_map(path))


# ---- reporting -------------------------------------------------------------------------------------
def describe(path: Path, c: Converted, blob: bytes | None = None) -> str:
    tc = Counter(s.type for s in c.scripts)
    L = [f"{path.stem}: {len(c.nodes)} expression nodes ({c.live_nodes} live in the source table), "
         f"{len(c.scripts)} scripts ({', '.join(f'{SCRIPT_TYPES[t]} {tc[t]}' for t in sorted(tc))}), "
         f"{len(c.globals)} globals ({sum(g.external for g in c.globals)} external), "
         f"{len(c.builtins)} built-ins in {sum(b.calls for b in c.builtins)} calls, "
         f"{sum(1 for n in c.nodes if n.kind == K_SCALL)} script calls"]
    if blob is not None:
        L.append(f"  SCRP {len(blob) / 1024:.1f} KiB, {c.stale_heads} function-name nodes with a stale index")
    for p in c.problems[:10]:
        L.append(f"  problem: {p}")
    return "\n".join(L)


def usage_report(maps: list[Path]) -> str:
    """Markdown usage report over a list of maps (aggregate statistics only)."""
    return render_report({p.stem: convert_map(p) for p in maps})


def render_report(per_map: dict[str, Converted]) -> str:
    names = list(per_map)
    calls: dict[str, dict[str, int]] = defaultdict(dict)
    domain: dict[str, int] = {}
    argt: dict[str, set] = defaultdict(set)
    rett: dict[str, set] = defaultdict(set)
    amin: dict[str, int] = {}
    amax: dict[str, int] = {}
    for m, c in per_map.items():
        for b in c.builtins:
            calls[b.name][m] = b.calls
            domain[b.name] = b.domain
            argt[b.name] |= b.arg_types
            rett[b.name] |= b.ret_types
            amin[b.name] = min(amin.get(b.name, 255), b.min_args)
            amax[b.name] = max(amax.get(b.name, 0), b.max_args)
    L = ["# Script built-in usage", "",
         "Generated by `python3 -I tools/halo2ps2/scriptconv.py --report docs/reports/script-usage.md assets/decompressed/*.map`.",
         "Counts are call sites in expression trees reachable from a script or a global initialiser; only built-in",
         "names (the engine's vocabulary) and aggregate numbers appear here, no script text. Value types are the",
         "raw type codes the arguments of the call sites have (names where verified, see docs/24-script-section.md).",
         "",
         "## Per map", "",
         "| map | expression nodes | scripts | startup | dormant | continuous | static | stub | globals | external | built-ins | calls | script calls |",
         "|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|"]
    for m, c in per_map.items():
        tc = Counter(s.type for s in c.scripts)
        L.append(f"| {m} | {len(c.nodes)} | {len(c.scripts)} | {tc[0]} | {tc[1]} | {tc[2]} | {tc[3]} | {tc[4]} | "
                 f"{len(c.globals)} | {sum(g.external for g in c.globals)} | {len(c.builtins)} | "
                 f"{sum(b.calls for b in c.builtins)} | {sum(1 for n in c.nodes if n.kind == K_SCALL)} |")
    L += ["", "## Per domain", "",
          "| domain | distinct (a10) | calls (a10) | distinct (union) | calls (union) |", "|---|---:|---:|---:|---:|"]
    for di, dn in enumerate(DOMAINS):
        in_dom = [n for n in calls if domain[n] == di]
        a10 = [n for n in in_dom if "a10" in calls[n]]
        L.append(f"| {dn} | {len(a10)} | {sum(calls[n]['a10'] for n in a10)} | {len(in_dom)} | "
                 f"{sum(sum(calls[n].values()) for n in in_dom)} |")
    L += ["", "## Every built-in", "",
          "Sorted by domain then by total calls. `args` = argument count range, `arg types` = raw value types seen "
          "in argument position, `ret` = result types seen.", ""]
    hdr = "| built-in | domain | args | arg types | ret | " + " | ".join(names) + " | total |"
    L += [hdr, "|" + "---|" * 5 + "---:|" * (len(names) + 1)]
    for n in sorted(calls, key=lambda n: (domain[n], -sum(calls[n].values()), n)):
        row = " | ".join(str(calls[n].get(m, "")) for m in names)
        at = ",".join(vname(t) for t in sorted(argt[n])) or "-"
        rt = ",".join(vname(t) for t in sorted(rett[n]))
        ar = f"{amin[n]}" if amin[n] == amax[n] else f"{amin[n]}..{amax[n]}"
        L.append(f"| `{n}` | {DOMAINS[domain[n]]} | {ar} | {at} | {rt} | {row} | {sum(calls[n].values())} |")
    for m, c in per_map.items():
        for p in c.problems[:5]:
            L.append(f"\nproblem ({m}): {p}")
    return "\n".join(L) + "\n"


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("inputs", nargs="*", type=Path, help="decompressed .map files (or a .hp2 with --info)")
    ap.add_argument("-o", "--out", type=Path)
    ap.add_argument("--info", action="store_true", help="print counts instead of writing")
    ap.add_argument("--report", type=Path, help="write the usage report (markdown) over all inputs")
    args = ap.parse_args(argv)
    if not args.inputs:
        ap.error("no input")
    if args.report:
        text = usage_report(args.inputs)
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(text)
        print(f"wrote {args.report}")
        return 0
    rc = 0
    for p in args.inputs:
        if p.suffix == ".hp2":
            sec = parse_section(hp2.parse(p.read_bytes()).read("SCRP"))
            print(f"{p.name}: SCRP {sec.size} bytes, {len(sec.nodes)} nodes, {len(sec.scripts)} scripts, "
                  f"{len(sec.globals)} globals, {len(sec.builtins)} built-ins")
            continue
        c = convert_map(p)
        blob = build_section(c)
        print(describe(p, c, blob))
        rc |= 1 if c.problems else 0
        if args.info:
            continue
        out = args.out if args.out and len(args.inputs) == 1 else p.parent.parent / "hp2" / f"{p.stem}.script.hp2"
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_bytes(hp2.build(p.stem, [("SCRP", blob)]))
        print(f"wrote {out}")
    return rc


if __name__ == "__main__":
    sys.exit(main())
