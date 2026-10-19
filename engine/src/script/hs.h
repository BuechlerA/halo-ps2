/* Script machine (docs/24-script-section.md): a cooperative tree-walking virtual machine for the compiled scenario
 * scripts (HSC) held in the `SCRP` section written by tools/halo2ps2/scriptconv.py. Pure C11, no heap, no double, no
 * recursion in the evaluator, no ps2sdk: host-tested (engine/tests/test_hs.c), the same code runs on the EE.
 *
 * Shape
 *   hs_open()       validate a section (every index, chain and offset; corrupt input is rejected, never crashes)
 *   hs_setup()      clear a machine and register the library built-ins (hs_core.c); hs_register() adds the host's
 *   hs_init()       bind, evaluate the global initialisers, create the threads of the startup and continuous scripts
 *   hs_tick()       one 30 Hz tick: every thread that is due runs until it sleeps (with node budgets)
 *   hs_state_ptr()  the whole runtime state as one pointer-free struct for checkpoints / saves, hs_rebind() to resume
 *
 * Execution model
 *   * One script = at most one thread (startup / dormant / continuous scripts). Static and stub scripts are called
 *     inline from the calling thread (their frames sit on the caller's stack, so a static script may sleep: the
 *     caller sleeps), with a call depth limit. Up to HS_MAX_THREADS threads exist at once.
 *   * A thread owns an explicit frame stack and value stack inside hs_state. `sleep`, `sleep_until`, `sleep_forever`,
 *     `wake` and host built-ins that block just leave the stacks as they are; the next tick continues from there.
 *   * A tick visits the thread pool in index order; a thread runs when `wake <= tick`. Threads woken during the pass
 *     run in the same tick when they sit later in the pool, otherwise in the next one.
 *   * Values are 8 bytes. Real arithmetic is float. Short and long are int32 (a short wraps to 16 bits when it is
 *     produced by arithmetic). Everything the machine does not know (objects, lists, tag references, trigger
 *     volumes, enumerations) is an HS_HANDLE: a u32 the host defines and the machine only copies and compares.
 *
 * Built-ins
 *   Library built-ins (arithmetic, comparison, logic, random, print/inspect, sleep, wake) are registered by
 *   hs_setup(); special forms (begin, begin_random, if, cond, set, and, or, sleep_until) are evaluated by the machine
 *   itself because they decide which arguments run. Every other built-in comes from the host through hs_register()
 *   (by NAME, the names the SCRP section carries). A call of a built-in nobody registered is a counted no-op that
 *   returns the neutral value of the call's type (see hs_neutral), so a missing built-in never stops a mission.
 *   A host built-in gets `ret` pre-set to the neutral value of the type the call site expects; it fills ret->v and
 *   returns 0, or returns 1 = "would block": the call is retried with the same arguments next tick. */
#ifndef SCRIPT_HS_H
#define SCRIPT_HS_H
#include <stddef.h>
#include <stdint.h>

/* ---- limits (the runtime state is sized by them: see HS_STATE_BYTES_MAX) ---- */
#define HS_MAX_THREADS    128u      /* concurrent threads */
#define HS_MAX_FRAMES     24u       /* expression frames per thread (call nesting incl. static script calls) */
#define HS_MAX_VALUES     16u       /* values per thread: arguments of the calls being evaluated */
#define HS_MAX_CALL_DEPTH 12u       /* nested static script calls per thread */
#define HS_MAX_PARAMS     12u
#define HS_MAX_SCRIPTS    512u
#define HS_MAX_GLOBALS    256u      /* scenario globals + engine (external) globals */
#define HS_MAX_BUILTINS   512u
#define HS_MAX_NODES      65534u
#define HS_MAX_REGISTERED 512u
#define HS_NODE_NONE      0xFFFFu
#define HS_NO_THREAD      0xFFFFu
#define HS_FOREVER        0xFFFFFFFFu
#define HS_HANDLE_NONE    0xFFFFFFFFu
#define HS_DEFAULT_NODE_BUDGET 4096u   /* node evaluations per thread and tick */
#define HS_DEFAULT_TICK_BUDGET 60000u  /* node evaluations per tick over all threads */
#define HS_DEFAULT_POLL_TICKS  30u     /* sleep_until without a period polls once a second (HEK documentation) */
#define HS_STATE_BYTES_MAX (96u * 1024u)

typedef enum { HS_VOID = 0, HS_BOOL, HS_REAL, HS_SHORT, HS_LONG, HS_STRING, HS_SCRIPT, HS_HANDLE } hs_type;

typedef enum {
	HS_OK = 0,
	HS_ERR_TRUNCATED,   /* shorter than the header or a table */
	HS_ERR_MAGIC,
	HS_ERR_VERSION,
	HS_ERR_LAYOUT,      /* a count, offset, index, chain or string is inconsistent */
	HS_ERR_ALIGN,       /* the section must start on a 4-byte boundary (the tables are used in place) */
	HS_ERR_LIMIT,       /* more scripts / globals / built-ins / nodes than the machine supports */
	HS_ERR_STATE        /* hs_rebind: the saved state does not belong to this section or is inconsistent */
} hs_status;

/* A value. `type` is the class the machine computes with, `vtype` the engine's raw value type of whoever produced
 * it (informational: lets a host tell an object list from a unit, a constant of type 38 from 37...). */
typedef struct {
	uint8_t  type;     /* hs_type */
	uint8_t  vtype;
	uint16_t aux;      /* always 0 (keeps the struct free of padding bytes: snapshots compare equal) */
	union { int32_t i; float f; uint32_t h; } v;   /* bool 0/1, short/long, real, string pool offset, script index, handle */
} hs_value;

/* ---- section (immutable, shared) ---- */
typedef struct {            /* expression node, 16 bytes, in the section as is */
	uint8_t  kind;          /* HS_N_* */
	uint8_t  cls;           /* hs_type of the value the node yields */
	uint8_t  vtype;         /* raw engine value type */
	uint8_t  zero;
	uint16_t next;          /* next sibling, HS_NODE_NONE at the end; always > own index */
	uint16_t first;         /* first argument (calls), HS_NODE_NONE otherwise */
	uint16_t nargs;
	uint16_t zero2;
	uint32_t data;          /* constant bits | builtin index | script index | global index | parameter slot */
} hs_node;
enum { HS_N_CONST = 0, HS_N_CALL, HS_N_SCRIPT_CALL, HS_N_GLOBAL, HS_N_PARAM };

typedef struct {            /* script, 32 bytes */
	uint32_t name;          /* string pool offset */
	uint16_t type;          /* HS_S_* */
	uint16_t rtype;         /* raw return type */
	uint16_t root;          /* root expression node, HS_NODE_NONE = empty */
	uint8_t  rcls;
	uint8_t  nparams;
	uint8_t  pcls[HS_MAX_PARAMS];
	uint8_t  pad[8];
} hs_script_rec;
enum { HS_S_STARTUP = 0, HS_S_DORMANT, HS_S_CONTINUOUS, HS_S_STATIC, HS_S_STUB };

typedef struct {            /* global, 16 bytes */
	uint32_t name;
	uint16_t vtype;
	uint8_t  cls;
	uint8_t  flags;         /* HS_G_EXTERNAL: an engine variable, not declared by the scenario */
	uint16_t init;          /* initialiser expression node or HS_NODE_NONE */
	uint16_t pad;
	uint32_t pad2;
} hs_global_rec;
#define HS_G_EXTERNAL 1u

typedef struct {            /* built-in, 16 bytes */
	uint32_t name;
	uint32_t calls;         /* call sites in this map (statistics) */
	uint8_t  domain, min_args, max_args, pad[5];
} hs_builtin_rec;

typedef struct {
	const uint8_t        *data;
	size_t                size;
	uint32_t              node_count, script_count, global_count, builtin_count, external_count, string_bytes;
	uint32_t              hash;                 /* FNV-1a of the section: ties a saved state to its section */
	const hs_node        *nodes;
	const hs_script_rec  *scripts;
	const hs_global_rec  *globals;
	const hs_builtin_rec *builtins;
	const char           *strings;
	const uint8_t        *classes;              /* 64 bytes: class of each raw value type */
} hs_section;

/* ---- runtime state: plain old data, NO pointers, fixed size (hs_state_size()), see hs_rebind ---- */
typedef struct {            /* one expression frame, 16 bytes */
	uint16_t node;          /* call / script call node */
	uint16_t cur;           /* next argument node to evaluate */
	uint16_t vbase;         /* first value-stack slot of this frame */
	uint16_t a16;           /* special forms: saved parameter base / sleep period / count */
	uint32_t aux;           /* special forms: random mask / deadline tick */
	uint8_t  phase;
	uint8_t  have;          /* a child's result was pushed since the frame last ran */
	uint8_t  op;            /* HS_OP_* of the call (cached from the binding) */
	uint8_t  pad;
} hs_frame;

enum { HS_T_FREE = 0, HS_T_ACTIVE = 1 };
typedef struct {            /* one thread */
	uint32_t wake;          /* runs when wake <= tick; HS_FOREVER = asleep until woken */
	uint16_t script;
	uint8_t  state;         /* HS_T_* */
	uint8_t  fsp;
	uint8_t  vsp;
	uint8_t  calls;         /* static script calls in progress */
	uint16_t pbase;         /* value-stack slot of the current script call's parameters */
	hs_frame f[HS_MAX_FRAMES];
	hs_value v[HS_MAX_VALUES];
} hs_thread;

#define HS_STATE_MAGIC 0x54535348u   /* "HSST" */
#define HS_STATE_VERSION 1u

typedef struct {
	uint32_t magic, version, section_hash;
	uint32_t tick;                  /* ticks run so far */
	uint32_t rng;                   /* xorshift32 state */
	uint32_t scan_start;            /* thread to start the next pass with (fairness after a tick budget stop) */
	/* counters (never read by the machine) */
	uint32_t nodes_tick, nodes_max_tick, nodes_total;
	uint32_t budget_hits, tick_budget_hits, blocked_calls, unreg_total, threads_started, wakes;
	uint32_t thread_overflow;       /* a wake / start found no free thread */
	uint32_t errors, last_error, last_error_script;   /* threads killed for a stack / depth error */
	uint32_t hw_threads, hw_frames, hw_values, hw_calls;   /* high-water marks */
	uint16_t script_thread[HS_MAX_SCRIPTS];     /* thread index running the script, HS_NO_THREAD = dormant / finished */
	uint32_t unreg[HS_MAX_BUILTINS];            /* calls per built-in nobody registered */
	hs_value globals[HS_MAX_GLOBALS];
	hs_thread threads[HS_MAX_THREADS];
} hs_state;

/* ---- machine ---- */
typedef struct hs_machine hs_machine;
typedef int  (*hs_builtin)(hs_machine *m, void *host, const hs_value *args, unsigned nargs, hs_value *ret);
typedef void (*hs_log_fn)(void *host, const char *msg);

typedef struct { char name[80]; hs_builtin fn; } hs_reg;
typedef struct { hs_builtin fn; uint8_t op; } hs_bind;

struct hs_machine {
	uint32_t         magic;
	const hs_section *sec;          /* bound by hs_init / hs_rebind: NULL until then */
	void            *host;
	hs_log_fn        log;           /* print / inspect output, may be NULL */
	uint32_t         node_budget, tick_budget, poll_ticks;   /* configuration, not part of the saved state */
	unsigned         nreg;
	hs_reg           reg[HS_MAX_REGISTERED];
	hs_bind          bind[HS_MAX_BUILTINS];
	hs_state         st;
	/* transient (never live across hs_tick) */
	hs_thread        scratch;       /* synchronous evaluation: initialisers, hs_run_expression, hs_call_script */
	hs_thread       *cur;
	int              yield, sync;
};

/* ---- API ---- */
hs_status hs_open(hs_section *s, const uint8_t *data, size_t size);

extern int g_hs_trace, g_hs_trace_all;
void      hs_setup(hs_machine *m);                                   /* clear + register the library built-ins */
void      hs_register(hs_machine *m, const char *name, hs_builtin fn);   /* may be called before or after hs_init */
hs_status hs_init(hs_machine *m, const hs_section *s, void *host);
hs_status hs_rebind(hs_machine *m, const hs_section *s, void *host);  /* adopt a restored state, see below */
void      hs_tick(hs_machine *m);

void      hs_set_log(hs_machine *m, hs_log_fn fn);
void      hs_set_budgets(hs_machine *m, uint32_t per_thread, uint32_t per_tick);
void      hs_set_poll_ticks(hs_machine *m, uint32_t ticks);          /* sleep_until default period */
void      hs_seed(hs_machine *m, uint32_t seed);

/* Snapshot: memcpy hs_state_size() bytes out of hs_state_ptr(m) at any time between ticks; to restore, hs_setup() a
 * machine, hs_register() the same built-ins, copy the bytes into hs_state_ptr(), then hs_rebind(m, section, host).
 * hs_rebind refuses (HS_ERR_STATE) a state that belongs to another section or is internally inconsistent. */
hs_state *hs_state_ptr(hs_machine *m);
size_t    hs_state_size(void);

/* lookups (-1 = not found) */
int       hs_find_script(const hs_section *s, const char *name);
int       hs_find_global(const hs_section *s, const char *name);
int       hs_find_builtin(const hs_section *s, const char *name);
const char *hs_string(const hs_section *s, int32_t offset);        /* "" when out of range */
const char *hs_script_name(const hs_section *s, unsigned i);
const char *hs_global_name(const hs_section *s, unsigned i);
const char *hs_builtin_name(const hs_section *s, unsigned i);

/* scripts and globals */
int       hs_wake_script(hs_machine *m, const char *name);          /* 0 = done, -1 unknown / no free thread */
int       hs_wake_script_index(hs_machine *m, unsigned script);
int       hs_script_running(const hs_machine *m, unsigned script);   /* has a thread (running or sleeping) */
unsigned  hs_thread_count(const hs_machine *m);
hs_value  hs_global_get(const hs_machine *m, unsigned index);
int       hs_global_set(hs_machine *m, unsigned index, hs_value v);  /* coerced to the global's class, 0 = ok */
hs_value  hs_make_bool(int b);
hs_value  hs_make_int(int32_t i);
hs_value  hs_make_real(float f);
hs_value  hs_make_handle(uint32_t h);
hs_value  hs_neutral(unsigned cls, unsigned vtype);

/* Run an expression / a script synchronously to its end (sleeps are skipped, blocking built-ins count as done).
 * For tests, global initialisers and host tools; never from inside a host built-in of a running tick. */
hs_value  hs_run_expression(hs_machine *m, unsigned node);
hs_value  hs_call_script(hs_machine *m, unsigned script);

/* unregistered built-ins that were called (counted, in built-in index order); returns the number of distinct ones */
unsigned  hs_unregistered(const hs_machine *m, unsigned k, const char **name, uint32_t *calls);

/* ---- for hs_core.c (library built-ins) ---- */
enum { HS_OP_GENERIC = 0, HS_OP_BEGIN, HS_OP_BEGIN_RANDOM, HS_OP_IF, HS_OP_COND, HS_OP_AND, HS_OP_OR, HS_OP_SET,
       HS_OP_SLEEP_UNTIL, HS_OP_SCRIPT_CALL, HS_OP_COUNT };
unsigned  hs_core_op(const char *name);                  /* special form of that name or HS_OP_GENERIC */
void      hs_core_register(hs_machine *m);
void      hs_sleep_current(hs_machine *m, int32_t ticks);           /* <0: forever; yields the running thread */
int       hs_sleep_script(hs_machine *m, unsigned script, int32_t ticks);   /* another script's thread */
uint32_t  hs_random(hs_machine *m);                      /* xorshift32 */

#endif
