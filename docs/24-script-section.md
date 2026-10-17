# `SCRP` section, version 1 (compiled scripts) and the script machine

Output of `tools/halo2ps2/scriptconv.py`, read and executed by `engine/src/script/hs.[ch]` (+ `hs_core.c`), host-tested by
`engine/tests/test_hs.c`. Stored as the `SCRP` section of `<map>.script.hp2` ([`.hp2` pack](07-hp2-format.md)). Little endian,
tables 16-byte aligned, offsets relative to the section start. No pointers to Xbox memory; every built-in is referred to by
**name**. Usage statistics of all ten maps: [reports/script-usage.md](reports/script-usage.md). Phase 4 workstream S
([plan](23-phase4-plan.md)). Legal posture as in the plan: this document describes formats and aggregate numbers; no script text, no
script / global / level / encounter names.

## Source data (Xbox, engine version 5; verified on all ten campaign maps unless marked)
The scenario tag holds the syntax data (`+0x474` size, `+0x480` address), the string data (`+0x488`, `+0x494`), the scripts block
(`+0x49C`, 92 B) and the globals block (`+0x4A8`, 92 B). The layouts are in the docstring of `scriptconv.py`; the essentials:

| Fact | How verified |
|---|---|
| Syntax data = 0x38-byte datum-array header (`"script node"`, max elements 19001 on every map) + 19001 nodes of 20 B: `u16 salt, u16 index, u16 type, u16 flags, u32 next, u32 string offset, u32 data` | every map; live nodes 4557 (b30) .. 17516 (a10); the rest of the table is `0xCACA` fill or the unparsed source tokens of the script definitions (salt 0), which nothing references |
| A datum reference is `salt << 16 \| index` and every reference the converter follows carries the salt of the node it names | checked on every edge of every tree of all ten maps (0 defects) |
| Live node flags are exactly 9 (constant / function name), 8 (call), 10 (script call), 13 (global reference); there is no parameter node | histogram over all maps |
| A call node: `index` = built-in function index, `type` = result type, `data` = a function-name node (type 2, `string` = the built-in's name); the `next` chain of that node = the arguments | 5302 calls on a10; all maps |
| A function index names exactly one built-in on all ten maps and back (291 distinct names, indices 0..415). The function-name node's own `index` is stale on a30 (172 nodes) and b30 (99): **the call node's index is authoritative** | all maps |
| A script call: flags 10, `index` = script index, `data` = a function-name node carrying the script's name (equal to the script block's name on 999 of 999 calls). All 999 target a static script, none passes arguments | all maps |
| A script call's `type` is the type expected at the call site, not the script's return type: 142 of 999 differ (a unit used as an object, or as an object list) | all maps |
| Global reference: `data` = index into the scenario globals (3996 of 4000; name equal on all of them) or `0xFFFF8000 \| n` = an **engine (external) global** (4 uses of 3 names: a cheat flag, a rasteriser variable, a weather variable; `string` = its name) | all maps |
| A global read may be typed real while the global is short, or short while it is long (397 of 3996): numeric promotion | all maps |
| Constants: bool low byte; real float; short low 16 bits (the high half is stale, `0xFFFF` everywhere, `-1` = `0xFFFFFFFF`); long 32 bits; string = a runtime pointer (the text is at the node's string offset; `data - offset` is constant per map: 289 string constants on a10); script = script index (low 16 bits); everything else is a handle (16-bit indices plain or with a stale `0xFFFF` high half, `0xFFFFFFFF` = none, tag ids with a `0xE...` high half, AI references with bit 31 set) | value histograms per type |
| Arithmetic and comparison built-ins take REAL arguments (integer literals are compiled to real constants: `(* 30 300)` has two real constants and a *long* result type); the call node's type says what the result is converted to | all maps |
| Every built-in has the same argument value types at every call site, position by position, except the polymorphic ones (comparison, `set`, `and` / `or` / `begin`); 399 of 413 positions are constant across the ten maps, the 14 others are the comparison and `set` operands | `test_scriptconv.py` (this is what pins down the type numbers) |
| Scripts: `char[32] name, u16 type, u16 return type, u32 root expression`, 52 zero bytes. Types: 0 startup, 1 dormant, 2 continuous, 3 static, 4 stub. The root is a `begin` call on 1938 of 1938 scripts | all maps |
| Globals: `char[32] name, u16 type, u16 0, u32 0, u32 initialiser`, 48 zero bytes. Initialisers: 658 constants, 63 calls, 2 global references | all maps |
| **No script has parameters** (the 52 bytes after the root expression are zero on all 1938 scripts) and the nodes have no parameter form | all maps |

Value type numbers (the engine's, `u16`). Names are inferred from consistency, never from text: argument types at call sites of
well-known built-ins, result types, token classes of constants. **Verified**: 4 void, 5 bool, 6 real, 7 short, 8 long, 9 string,
10 script, 11 trigger volume, 12 cutscene flag, 13 cutscene camera point, 14 cutscene title, 15 cutscene recording, 16 device group,
17 ai, 18 ai command list, 19 starting profile, 20 conversation, 21 navpoint, 22 hud message, 23 object list, 24 sound, 25 effect,
27 looping sound, 28 animation graph, 29 actor variant, 31 object definition, 32 game difficulty, 33 team, 37 object, 38 unit.
Seen but not named (the roles are clear, the numbers are not pinned): 26 (a tag reference, damage), 30 / 34 / 35 / 36 (enumerations
and tag references that occur a handful of times), 39..43 (object-name families: 39, 41, 42, 43 constants are object-name indices).

## `SCRP` layout
Header, 128 bytes: `char[4] "SCRP"`, `u16 version (1)`, `u16 0`, `u32 total size`, `u32 node count` (<= 65534), `u32 script
count` (<= 512), `u32 global count` (<= 256, scenario globals then external ones), `u32 builtin count` (<= 512), `u32 external
global count`, `u32 string bytes`, then table offsets: nodes `0x24`, scripts `0x28`, globals `0x2C`, builtins `0x30`,
strings `0x34`, classes `0x38`; `u32 call sites` at `0x3C`; five `u16` script counts per type at `0x40`.

| Table | Element | Contents |
|---|---|---|
| nodes | 16 B | `u8 kind` (0 constant, 1 call, 2 script call, 3 global reference, 4 parameter reference), `u8 class`, `u8 raw value type`, `u8 0`, `u16 next sibling`, `u16 first argument`, `u16 argument count`, `u16 0`, `u32 data` (`0xFFFF` = none) |
| scripts | 32 B | `u32 name offset`, `u16 type`, `u16 raw return type`, `u16 root node`, `u8 return class`, `u8 parameter count` (0 on all maps), `u8 parameter classes[12]`, 8 pad |
| globals | 16 B | `u32 name offset`, `u16 raw type`, `u8 class`, `u8 flags` (bit 0 = external), `u16 initialiser node`, 6 pad |
| builtins | 16 B | `u32 name offset`, `u32 call sites in this map`, `u8 domain`, `u8 min args`, `u8 max args`, 5 pad; sorted by the engine's function index |
| classes | 64 B | class of each raw value type 0..63 |
| strings | bytes | NUL separated, offset 0 is the empty string; built-in, script and global names, string constants |

Classes (`hs_type`): 0 void, 1 bool, 2 real, 3 short, 4 long, 5 string, 6 script, 7 handle. Constant `data` is normalised by class:
bool 0/1; real = float bits; short sign-extended; long; string = pool offset; script = script index; handle = the 16-bit index with the
stale `0xFFFF` high half removed, `0xFFFFFFFF` kept, tag ids and AI references as they are. **Nodes are stored in pre-order**: `first`
and `next` always point to a higher index, so a section cannot contain a cycle and the reader validates that cheaply. Only nodes
reachable from a script root or a global initialiser are written (a10: 12064 of 17516 live nodes; a node reachable twice would be
duplicated, none is). The function-name nodes are folded into the call (the call's `data` is the built-in index).
External globals are appended to the scenario globals (flag bit 0), with the type of the references that name them; their reference
nodes point at the appended slot. Sizes: a10 208 KiB raw / 94 KiB packed (the largest), b30 60 KiB raw; converting all ten maps takes
0.35 s.

`parse_section()` in the converter is the reference reader; `hs_open()` is the C one. It validates: size field, signature, version,
4-byte alignment of the buffer (the node table is used in place through a struct overlay: the tables are little endian like the EE and
every host we build on), counts against the limits, every table inside the section, string pool NUL-terminated, class table, every
node (kind, class, `next` / `first` forward and in range, argument count equal to the chain length, built-in / script / global index in
range, string offsets, script constants, script calls pass exactly the callee's parameter count), scripts and globals (types, root and
initialiser nodes, external flag only on the trailing `external count` globals). Errors: `HS_ERR_TRUNCATED / MAGIC / VERSION / LAYOUT /
ALIGN / LIMIT`. A failed open leaves the `hs_section` zeroed. `test_hs.c` truncates at every length and flips every byte three ways
(3104 bytes x 3 on the test world: 4513 of 9312 flips still open, because most land in constants, and all of them run safely under
ASan / UBSan).

## The machine
```c
hs_status hs_open(hs_section *s, const uint8_t *data, size_t size);
void      hs_setup(hs_machine *m);                                  /* clear + register the library built-ins */
void      hs_register(hs_machine *m, const char *name, hs_builtin fn);   /* before or after hs_init; by NAME */
hs_status hs_init(hs_machine *m, const hs_section *s, void *host);  /* bind, run global initialisers, create threads */
void      hs_tick(hs_machine *m);                                   /* one 30 Hz tick */
hs_status hs_rebind(hs_machine *m, const hs_section *s, void *host);    /* after restoring a saved state */
hs_state *hs_state_ptr(hs_machine *m);  size_t hs_state_size(void);
int hs_wake_script(m, name);  hs_wake_script_index;  hs_script_running;  hs_thread_count;
hs_value hs_global_get(m, i);  int hs_global_set(m, i, v);  hs_find_script / hs_find_global / hs_find_builtin / hs_string
hs_value hs_run_expression(m, node);  hs_value hs_call_script(m, script);     /* synchronous: tests, initialisers, tools */
unsigned hs_unregistered(m, k, &name, &calls);                       /* what the host has not implemented yet */
void hs_set_log(m, fn);  hs_set_budgets(m, per_thread, per_tick);  hs_set_poll_ticks(m, ticks);  hs_seed(m, seed);
typedef int (*hs_builtin)(hs_machine *m, void *host, const hs_value *args, unsigned nargs, hs_value *ret);  /* 0 ok, 1 = would block */
```
Call `hs_setup`, then `hs_register` every host built-in, then `hs_init` (global initialisers may call host built-ins; registering after
`hs_init` also works and re-binds). `hs_value` is 8 bytes: `type` (the class), `vtype` (the engine's raw value type of whoever produced
it: lets a host tell an object list from a unit), `aux` (0), and a union `i / f / h`. A host built-in receives its arguments already
evaluated (`args[i].type` = class, `.vtype` = raw type), and `ret` **pre-set to the neutral value of the type the call site expects**: it fills
`ret->v` and returns 0; returning 1 means "would block": the call is retried with the same arguments next tick (the thread keeps its
stacks). Handles are opaque 32-bit values (`HS_HANDLE_NONE = 0xFFFFFFFF`); an argument typed object list may carry a single object
(a unit used as a list: 36 call sites on the ten maps; a unit used as an object: 106), so a host that takes lists must accept an object handle there, which it can
see through `vtype`. A call of a built-in nobody registered is a **counted no-op returning the neutral value of the call's class**
(bool false, 0, 0.0, `HS_HANDLE_NONE`, the empty string, void), its arguments are still evaluated; `hs_unregistered()` lists the
built-ins, in section order, with their call counts.

### Evaluation
* A thread owns an explicit frame stack (16-byte frames: node, next argument, value base, special-form scratch, phase) and a value
  stack. The evaluator is one loop over the top frame; arguments that are constants, global reads and parameter reads are evaluated inline,
  call arguments push a frame. No C recursion; an expression can only be as deep as `HS_MAX_FRAMES` (24; the real maps need at most 17
  frames, 6 values, 4 nested static calls), and a thread that overflows its stacks or the static-call depth (`HS_MAX_CALL_DEPTH` 12) is
  killed and counted (`errors`, `last_error`, `last_error_script`); it never affects other threads.
* Special forms the evaluator handles itself, because they decide which arguments run: `begin` (value of the last expression),
  `begin_random` (every expression exactly once in random order, void), `if` (with or without else; value of the taken branch), `cond`
  (arguments are `(test, result)` pairs), `set` (target is the first argument, a global; the value is converted to the global's class),
  `and` / `or` (n-ary, **short-circuit**), `sleep_until`.
* Library built-ins in `hs_core.c`, registered by `hs_setup` and overridable: `+ - * / min max` (REAL arguments, result converted to the
  call node's type, truncating toward zero; short results wrap to 16 bits; `/` by zero gives 0), `= != < > <= >=` (integers exactly,
  otherwise floats; handles, strings and scripts compare their 32 bits), `not`, `random_range` (integer in `[lo, hi)`),
  `real_random_range`, `print` / `inspect` (to the host log callback), `sleep`, `sleep_forever`, `wake`.
* Values are converted to the type of the node that yields them when both are numeric (`short`/`long` <-> `real`, `long` -> `short`
  wraps); a void-typed node drops its value; other mismatches (a unit where a list is expected) are left to the host.
* Script calls run inline: the callee's root expression is evaluated on the caller's stacks, arguments are bound to parameter slots
  (`parameter reference` nodes read `vals[pbase + slot]`; no map uses them, the converter never emits them, the machine supports them
  and the tests cover them), the callee's result replaces the arguments and is converted to the call site's class. A static script may
  sleep: the calling thread sleeps with it.

### Threads, time and scheduling
* One thread per startup / dormant / continuous script (`script_thread[script]`), at most `HS_MAX_THREADS` = 128 at once; a start
  or wake with no free slot is dropped and counted (`thread_overflow`). `hs_init` creates the startup and continuous threads (due at tick 0);
  dormant scripts have none until `wake`.
* `hs_tick` visits slots in index order; a thread runs when `wake <= tick` (`HS_FOREVER` never). **A thread woken or started during
  the pass runs in the same tick when its slot lies after the running one, otherwise in the next tick** (the lowest free slot is
  reused). A thread runs until it sleeps, blocks, finishes, or exhausts its node budget (`HS_DEFAULT_NODE_BUDGET` 4096
  evaluations per thread and tick; `HS_DEFAULT_TICK_BUDGET` 60000 over all threads: a pass that hits it stops and the next tick starts
  with the thread it stopped at, so nobody starves; both counted).
* `sleep n` resumes after `n` ticks (`n < 1` yields until next tick, `n < 0` = forever); `sleep n script` / `sleep_forever script` act on
  another script's thread (no-op when it has none); `wake script` makes a sleeping thread due now (cutting `sleep`, `sleep_forever`,
  a `sleep_until` poll short), starts a dormant or finished one from its beginning, does nothing for static / stub scripts.
* `sleep_until cond [period [timeout]]`: evaluates `cond` immediately; while false it sleeps `period` ticks (default
  `poll_ticks` = **30, once a second**, configurable) and evaluates the *condition expression again from scratch*; `timeout` (ticks, long)
  ends the wait even if the condition stays false. A finished startup / dormant thread frees its slot (a later `wake` runs the script
  again); a continuous thread restarts one tick after it finishes.
* Global initialisers run at `hs_init` in declaration order, synchronously (sleeps skipped).

### Runtime state, snapshots
All mutable state is **one pointer-free POD struct, `hs_state`**, inside the machine; the section, the host pointer, the registered
built-ins and the configuration (budgets, poll period, log callback) live outside it. Size is fixed: **72,284 bytes**
(limit 96 KiB): 128 threads x 524 B (12 header + 24 frames x 16 + 16 values x 8) = 67 KB, plus 256 globals x 8, 512 script -> thread
indices, 512 unregistered-call counters and about 100 B of header / counters / high-water marks. The whole machine (`hs_machine`: the
state, the 512-entry registry, the binding table, a scratch thread for synchronous evaluation) is 126,120 bytes.

Checkpoint / save: `memcpy(out, hs_state_ptr(m), hs_state_size())`. Restore (also in a later run of the same ELF): `hs_setup(m)`,
`hs_register` the same host built-ins, copy the bytes into `hs_state_ptr(m)`, `hs_rebind(m, section, host)`. `hs_rebind` refuses with
`HS_ERR_STATE` (and leaves the machine unbound; ticking it is a no-op) a state whose tag, version or section hash (FNV-1a of the section,
computed by `hs_open`) does not match, or that is inconsistent: thread states, frame and value counts, every frame's node index and
kind, its cached special-form op against the binding, value bases in order, `script_thread` and the threads agreeing both ways, value
and global classes. The evaluator re-checks what it indexes with (stack underflow, global target of `set`, random-order mask), so even a
state that passes can only misbehave, not corrupt memory. Tests: a machine run for 14 ticks (9 threads asleep at different points:
inside `sleep_until`, inside a static script, at plain statements) is snapshotted, the original runs 150 more ticks, a fresh machine
is restored from the snapshot, rebound and run 150 ticks: global values, thread states and the **whole state bytes** are identical, as is
the sequence of host calls; 6000 bit flips in the used part of a saved state are each refused or run safely.

## Tests and numbers
`make -C engine/tests hs` (part of `make -C engine/tests`): `test_scriptconv.py` (Xbox-layout synthetic scenario -> converter ->
section -> reference parser; constants, defects, domains, report; all ten real maps: no problems, node / script / global counts of a10,
291 distinct built-ins, one name per function index, constant argument types per position, built-in call counts equal to the
inventory's) then `test_hs.c`: arithmetic and every conversion, logic with short circuit, `if` / `cond` / `begin` values,
`begin_random` (every branch once, 300 seeds, orders vary, same seed same order), `set` with conversions, global initialisers, script
calls with parameters and nested calls, recursion (frame and depth limits kill only the offending thread; legal 5-deep recursion works),
sleeping (`sleep`, `sleep_until` with period 1 / 5 / default / timeout and a changed default), waking (same tick versus next tick, dormant
scripts re-run, `sleep_forever` cut short, sleeping another script, continuous scripts), static scripts that sleep, blocking host
built-ins, the 128 thread cap (130 wakes: 2 dropped), per-thread and per-tick budgets, unregistered built-ins (neutral values per class,
counts, binding after the fact), random ranges, `print` / `inspect`, snapshot and restore, corrupt sections and states, the converter's
fixture, and every real pack in `assets/hp2/*.script.hp2` for 3000 ticks (below).

Real maps, 3000 ticks, `-O2` host build, three hosts: *neutral* (only the library built-ins: every unimplemented built-in returns its
neutral value, so conditions that wait for the game never fire), *chaos* (every built-in answers with pseudo-random bools / numbers /
handles and blocks 5 % of the time: far more scripts run), *stress* (chaos, and every dormant script is woken at ticks 0, 300, 900 and
1800). No stack or depth error and no budget stop on any map, any host.

| map | neutral: nodes/tick avg (max) | chaos | stress | threads peak: chaos (stress; high water) | frames hw | values hw |
|---|---|---|---|---|---|---|
| a10 | 15 (177) | 101 (325) | 111 (2001) | 18 (79; 105) | 16 | 6 |
| a30 | 14 (132) | 45 (224) | 62 (2031) | 18 (70; 110) | 10 | 5 |
| a50 | 5 (565) | 226 (454) | 231 (2076) | 19 (73; 128) | 12 | 5 |
| b30 | 24 (131) | 11 (219) | 18 (730) | 11 (47; 68) | 10 | 5 |
| b40 | 1 (209) | 175 (334) | 179 (2080) | 18 (70; 128) | 14 | 4 |
| c10 | 20 (204) | 77 (349) | 79 (864) | 15 (48; 128) | 17 | 4 |
| c20 | 13 (481) | 279 (484) | 285 (1905) | 29 (100; 128) | 13 | 4 |
| c40 | 0.1 (147) | 49 (100) | 55 (1302) | 5 (68; 96) | 10 | 6 |
| d20 | 1 (494) | 135 (351) | 138 (1555) | 20 (73; 87) | 13 | 6 |
| d40 | 2 (388) | 100 (341) | 160 (2013) | 16 (93; 128) | 14 | 6 |

Reading: a node is one expression evaluation (a constant, a global read, a call); 5302 built-in calls (plus 150 script calls) and 12064 reachable nodes on a10 are
*static* sizes, per tick a10 evaluates 15..111 nodes on average (worst tick 2001, with every dormant script awake): scripts mostly
sleep, and a polling `sleep_until` (default once a second) is cheap. Host cost is 0.1..2.2 us per tick on average (neutral 0.1..0.2) and 3..22 us for the
worst tick over all runs (this includes the clock read), i.e. **about 7 ns per node** on the host. The EE is 30-100x slower: expect 0.3..0.7 us per node,
so up to about 220 us per tick on average and under 2 ms for the worst pass, against a 33 ms frame; the 128-slot pool scan costs a few
microseconds. The *real* load depends on how often host built-ins block or answer true (the chaos and stress columns bracket it);
everything the machine does per tick is bounded by the budgets. In a stress run 5 of the 10 maps want more than 128 threads at once
(a50 at most 141 non-static scripts exist); in play the dormant scripts wake one mission phase at a time (peak 5..29 threads in the chaos runs).

## Built-in usage (summary; the full table with every built-in, domain, argument types and per-map call counts is in
[reports/script-usage.md](reports/script-usage.md))
13 map-independent domains plus `player` (14): control flow (`begin begin_random if cond set sleep sleep_until sleep_forever wake`), math
(arithmetic, comparison, random), logic (`and or not`), object (`object_*`, `objects_*`, `list_*`, `volume_*`, `players`), unit
(`unit_*`, `units_*`, `vehicle_*`, `unit`), player (`player_action_test_*`, `player0_*`, `player_enable_input` ...), ai (`ai_*`), device
(`device_*`), camera/cinematic (`camera_*`, `cinematic_*`, `recording_*`, `custom_animation*`, `fade_*`, `scenery_animation_*`), sound,
hud/ui (`hud_*`, `show_hud*`, nav points, timers), game/flow (`game_*`, `switch_bsp`, difficulty), effects (`effect_new*`, `damage_*`,
`player_effect_*`), debug/no-op (`print`, `inspect`, `rasterizer_*`, prediction and PVS hints). Calls and distinct built-ins:

| domain | a10 distinct | a10 calls | union distinct | union calls (10 maps) |
|---|---:|---:|---:|---:|
| control | 6 | 1409 | 7 | 11758 |
| math | 9 | 221 | 13 | 2352 |
| logic | 3 | 162 | 3 | 1009 |
| object | 21 | 1184 | 27 | 5446 |
| unit | 18 | 184 | 28 | 1022 |
| player | 17 | 64 | 18 | 88 |
| ai | 37 | 564 | 73 | 4912 |
| device | 6 | 109 | 12 | 554 |
| camera/cinematic | 23 | 526 | 31 | 2022 |
| sound | 7 | 378 | 7 | 1652 |
| hud/ui | 15 | 123 | 27 | 481 |
| game/flow | 15 | 95 | 20 | 583 |
| effects | 8 | 181 | 9 | 638 |
| debug/no-op | 10 | 102 | 16 | 1220 |

a10 uses 195 distinct built-ins in 5302 calls; the machine provides 19 of them (3,487 calls go to the 176 the host must implement). In
call order (a10): `players 203, sound_impulse_start 197, volume_test_objects 188, camera_set 184, object_destroy 163, ai_actors 159,
object_teleport 138, sound_impulse_time 137, object_create 123, object_create_anew 119, custom_animation 103, effect_new 99, list_get 75,
ai_place 71, ai_command_list 69, recording_play 59, objects_can_see_object 46, device_set_position 41, show_hud_help_text 39,
object_beautify 37, recording_kill 35, objects_attach 32, vehicle_load_magic 28, enable_hud_help_flash 28, ai_link_activation 28,
unit_set_seat 27, hud_set_help_text 27, ai_magically_see_encounter 27, sound_looping_start 26, device_set_power 25,
player_effect_set_max_rotation 22, fade_out 22, ai_conversation 22, object_destroy_containing 21, ai_living_count 21, device_get_position 20,
ai_conversation_stop 20, ai_command_list_advance 19, object_cannot_take_damage 18, game_save 18, structure_bsp_index 17, recording_time 17,
player_action_test_reset 17, device_set_position_immediate 17, unit_stop_custom_animation 16, unit_set_emotion 16, player_effect_start 16,
player_effect_set_max_rumble 16, object_set_scale 16, object_create_anew_containing 16, game_is_cooperative 16, fade_in 16,
units_set_current_vitality 15, unit_doesnt_drop_items 15, player_effect_set_max_translation 15, ai_playfight 15, recording_play_and_delete 14,
object_pvs_activate 14, unit_suspended 12, switch_bsp 12, object_type_predict 12, camera_time 12, ai_migrate 12, player_effect_stop 11,
camera_control 11, unit_set_current_vitality 10, unit_get_custom_animation_time 10, unit 10, objects_predict 10, objects_detach 10,
camera_set_relative 10, ai_command_list_by_unit 10, ai_command_list_status 9, sound_looping_stop 8, game_difficulty_get_real 8, ai_erase 8,
ai_defend 8, unit_close 7, ai_kill 7, ai_dialogue_triggers 7, ai_braindead 7, sound_looping_set_alternate 6, show_hud 6,
player_enable_input 6, object_can_take_damage 6, game_difficulty_get 6, cinematic_start 6, cinematic_show_letterbox 6,
ai_conversation_status 6, unit_set_maximum_vitality 5, ...` then 5 or fewer calls each (the long tail: `hud_*`,
`player_action_test_*`, `cinematic_screen_effect_*`, `game_*`, `ai_*`). 9 of the 176 (79 calls) are debug / hint built-ins that may
stay no-ops (`object_beautify`, `object_pvs_*`, `*_predict`, `render_lights`, `rasterizer_*`).

## Unverified or approximated
* `sleep_until` without a period polls **once a second** (`HS_DEFAULT_POLL_TICKS` 30): taken from the public documentation of the
  built-in, not from the data. The data supports it indirectly (period 1 is the most common *explicit* value, 525 of 1813 calls; 763
  calls pass no period). Change with `hs_set_poll_ticks`. The behaviour on `timeout` and the result (void) are as documented.
* `cond` argument structure (no map uses it) and the names `cond`, `min`, `sleep_forever` (no map uses them either) are from memory of the
  engine's vocabulary.
* `and` / `or` short-circuit (the engine may evaluate every argument; the arguments of all 618 uses are side-effect free but for
  built-in calls that are queries); `begin_random` uses our own generator, so orders are not reproducible against the original; `/` by zero
  returns 0; `random_range` is `[lo, hi)`.
* Timing details of the original scheduler: same-tick versus next-tick wakeups (we follow thread slot order), a continuous script that
  ends in a `sleep` restarts one tick after the sleep ends, a `wake` of a script that is currently running does nothing, a finished
  dormant script runs again when woken again.
* Parameters: no retail script has them and the Xbox data has no node form for them, so the converter never emits `parameter` nodes
  or non-zero parameter counts; the machine's support (and its tests) is an extension.
* The names of value types 26, 30, 34, 35, 36 and 39..43; the class table treats them as handles, which is all the machine needs.
* External (engine) globals: only 3 names occur; the machine stores them like scenario globals (initial value neutral) and the host can
  read / write them with `hs_find_global` + `hs_global_get/set`; what the engine does with them (cheats, render settings) is the host's.

## ADR proposal
**ADR-04x: Scripts run on a cooperative tree-walking machine over a pre-order node table, state as one pointer-free struct.**
*Decision*: `SCRP` holds the reachable expression trees of a scenario in pre-order 16-byte nodes with built-ins resolved to names;
`engine/src/script/hs.[ch]` evaluates them with explicit frame / value stacks (no C recursion, no heap), one thread per script, `sleep` and
`sleep_until` by suspending the thread, special forms in the evaluator, library built-ins in `hs_core.c`, every other built-in registered
by name by the host and a counted neutral no-op when missing. The machine's mutable state is a fixed-size (72 KB), pointer-free `hs_state`
that checkpoints and memory-card saves copy as bytes. *Why*: the original is itself a node-tree interpreter with 17.5 k nodes on a10 and
dozens of sleeping threads, a tree walk keeps the converter trivial and the semantics checkable against the data, an explicit stack makes
suspension free and bounded, a flat state makes saves and tests (bit-exact restore) trivial. *Consequences*: per tick cost is ~100 node
evaluations on average (about 50 us on the EE), bounded by budgets; the host owns every game built-in and the integrator reads
`hs_unregistered()` to see what is still missing; the 128-thread pool is sufficient for play but not for waking every dormant script
at once; sleeping is tick-quantised exactly like the original.
