/* Script machine: section validation, threads, the explicit-stack tree-walking evaluator and the special forms.
 * See hs.h for the model and docs/24-script-section.md for the section layout and the semantics.
 *
 * The section's node table is used in place (hs_open demands a 4-byte aligned buffer and a little-endian host,
 * which the EE and every host we build on are): nodes are 16-byte records read through a struct overlay. hs_open
 * validates every index once, so the evaluator never range-checks a node, script, global or built-in reference.
 * Nodes are stored in pre-order (first / next always point to a higher index): a section cannot contain a cycle,
 * and an expression can only recurse as deep as the frame stack, which is bounded. */
#include "script/hs.h"
#include <string.h>

#define MACHINE_MAGIC 0x4D4D5348u   /* "HSMM" */
#define HEADER_SIZE 128u
#define SCRP_VERSION 1u

_Static_assert(sizeof(hs_node) == 16, "hs_node");
_Static_assert(sizeof(hs_script_rec) == 32, "hs_script_rec");
_Static_assert(sizeof(hs_global_rec) == 16, "hs_global_rec");
_Static_assert(sizeof(hs_builtin_rec) == 16, "hs_builtin_rec");
_Static_assert(sizeof(hs_value) == 8, "hs_value");
_Static_assert(sizeof(hs_frame) == 16, "hs_frame");
_Static_assert(sizeof(hs_thread) == 12 + 16 * HS_MAX_FRAMES + 8 * HS_MAX_VALUES, "hs_thread has padding");
_Static_assert(sizeof(hs_state) <= HS_STATE_BYTES_MAX, "hs_state too large");
_Static_assert(sizeof(hs_state) == 23 * 4 + 2 * HS_MAX_SCRIPTS + 4 * HS_MAX_BUILTINS + 8 * HS_MAX_GLOBALS +
               sizeof(hs_thread) * HS_MAX_THREADS, "hs_state has padding");

enum { ERR_FRAMES = 1, ERR_VALUES, ERR_DEPTH, ERR_PHASE };
enum { R_DONE = 0, R_YIELD, R_ERROR };

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

/* ---- section ---- */
static int table_ok(uint32_t off, uint64_t count, uint32_t rec, size_t size)
{
	if (off % 16u || off < HEADER_SIZE || off > size) return 0;
	return count * rec <= (uint64_t)size - off;
}

hs_status hs_open(hs_section *s, const uint8_t *data, size_t size)
{
	uint32_t nn, ns, ng, nb, nx, nstr, o_nodes, o_sc, o_gl, o_bi, o_str, o_cls, i, h;
	const hs_node *nodes;
	const hs_script_rec *sc;
	const hs_global_rec *gl;
	const hs_builtin_rec *bi;
	const char *str;
	size_t k;

	memset(s, 0, sizeof *s);
	if (!data || size < HEADER_SIZE) return HS_ERR_TRUNCATED;
	if (memcmp(data, "SCRP", 4) != 0) return HS_ERR_MAGIC;
	if (rd16(data + 4) != SCRP_VERSION) return HS_ERR_VERSION;
	if (rd32(data + 8) > size) return HS_ERR_TRUNCATED;
	if (rd32(data + 8) != size) return HS_ERR_LAYOUT;
	if ((uintptr_t)data & 3u) return HS_ERR_ALIGN;
	nn = rd32(data + 0x0C); ns = rd32(data + 0x10); ng = rd32(data + 0x14);
	nb = rd32(data + 0x18); nx = rd32(data + 0x1C); nstr = rd32(data + 0x20);
	if (nn > HS_MAX_NODES || ns > HS_MAX_SCRIPTS || ng > HS_MAX_GLOBALS || nb > HS_MAX_BUILTINS) return HS_ERR_LIMIT;
	if (nx > ng || nstr < 1) return HS_ERR_LAYOUT;
	o_nodes = rd32(data + 0x24); o_sc = rd32(data + 0x28); o_gl = rd32(data + 0x2C);
	o_bi = rd32(data + 0x30); o_str = rd32(data + 0x34); o_cls = rd32(data + 0x38);
	if (!table_ok(o_nodes, nn, 16, size) || !table_ok(o_sc, ns, 32, size) || !table_ok(o_gl, ng, 16, size) ||
	    !table_ok(o_bi, nb, 16, size) || !table_ok(o_str, nstr, 1, size) || !table_ok(o_cls, 64, 1, size))
		return HS_ERR_LAYOUT;
	str = (const char *)data + o_str;
	if (str[0] != 0 || str[nstr - 1] != 0) return HS_ERR_LAYOUT;
	for (k = 0; k < 64; k++)
		if (data[o_cls + k] > HS_HANDLE) return HS_ERR_LAYOUT;
	nodes = (const hs_node *)(data + o_nodes);
	sc = (const hs_script_rec *)(data + o_sc);
	gl = (const hs_global_rec *)(data + o_gl);
	bi = (const hs_builtin_rec *)(data + o_bi);

	for (i = 0; i < ns; i++) {
		if (sc[i].name >= nstr || sc[i].type > HS_S_STUB || sc[i].rcls > HS_HANDLE || sc[i].nparams > HS_MAX_PARAMS ||
		    (sc[i].root != HS_NODE_NONE && sc[i].root >= nn))
			return HS_ERR_LAYOUT;
		for (k = 0; k < sc[i].nparams; k++)
			if (sc[i].pcls[k] > HS_HANDLE) return HS_ERR_LAYOUT;
	}
	for (i = 0; i < ng; i++) {
		if (gl[i].name >= nstr || gl[i].cls > HS_HANDLE || (gl[i].init != HS_NODE_NONE && gl[i].init >= nn) ||
		    ((gl[i].flags & HS_G_EXTERNAL) != 0) != (i >= ng - nx))
			return HS_ERR_LAYOUT;
	}
	for (i = 0; i < nb; i++)
		if (bi[i].name >= nstr) return HS_ERR_LAYOUT;
	for (i = 0; i < nn; i++) {
		const hs_node *n = &nodes[i];
		if (n->kind > HS_N_PARAM || n->cls > HS_HANDLE) return HS_ERR_LAYOUT;
		if (n->next != HS_NODE_NONE && (n->next >= nn || n->next <= i)) return HS_ERR_LAYOUT;
		if (n->kind == HS_N_CALL || n->kind == HS_N_SCRIPT_CALL) {
			unsigned cnt = 0, c = n->first;
			while (c != HS_NODE_NONE) {
				if (c >= nn || c <= i || ++cnt > n->nargs) return HS_ERR_LAYOUT;
				c = nodes[c].next;
			}
			if (cnt != n->nargs) return HS_ERR_LAYOUT;
			if (n->kind == HS_N_CALL && n->data >= nb) return HS_ERR_LAYOUT;
			if (n->kind == HS_N_SCRIPT_CALL && (n->data >= ns || sc[n->data].nparams != n->nargs)) return HS_ERR_LAYOUT;
		} else {
			if (n->first != HS_NODE_NONE || n->nargs != 0) return HS_ERR_LAYOUT;
			if (n->kind == HS_N_GLOBAL && n->data >= ng) return HS_ERR_LAYOUT;
			if (n->kind == HS_N_PARAM && n->data >= HS_MAX_PARAMS) return HS_ERR_LAYOUT;
			if (n->kind == HS_N_CONST && n->cls == HS_STRING && n->data >= nstr) return HS_ERR_LAYOUT;
			if (n->kind == HS_N_CONST && n->cls == HS_SCRIPT && n->data >= ns) return HS_ERR_LAYOUT;
		}
	}
	h = 2166136261u;
	for (k = 0; k < size; k++) h = (h ^ data[k]) * 16777619u;
	s->data = data; s->size = size;
	s->node_count = nn; s->script_count = ns; s->global_count = ng; s->builtin_count = nb;
	s->external_count = nx; s->string_bytes = nstr; s->hash = h;
	s->nodes = nodes; s->scripts = sc; s->globals = gl; s->builtins = bi; s->strings = str;
	s->classes = data + o_cls;
	return HS_OK;
}

const char *hs_string(const hs_section *s, int32_t off)
{
	if (!s || !s->strings || off < 0 || (uint32_t)off >= s->string_bytes) return "";
	return s->strings + off;
}
const char *hs_script_name(const hs_section *s, unsigned i) { return s && i < s->script_count ? hs_string(s, (int32_t)s->scripts[i].name) : ""; }
const char *hs_global_name(const hs_section *s, unsigned i) { return s && i < s->global_count ? hs_string(s, (int32_t)s->globals[i].name) : ""; }
const char *hs_builtin_name(const hs_section *s, unsigned i) { return s && i < s->builtin_count ? hs_string(s, (int32_t)s->builtins[i].name) : ""; }

int hs_find_script(const hs_section *s, const char *name)
{
	unsigned i;
	for (i = 0; s && i < s->script_count; i++)
		if (strcmp(hs_script_name(s, i), name) == 0) return (int)i;
	return -1;
}
int hs_find_global(const hs_section *s, const char *name)
{
	unsigned i;
	for (i = 0; s && i < s->global_count; i++)
		if (strcmp(hs_global_name(s, i), name) == 0) return (int)i;
	return -1;
}
int hs_find_builtin(const hs_section *s, const char *name)
{
	unsigned i;
	for (i = 0; s && i < s->builtin_count; i++)
		if (strcmp(hs_builtin_name(s, i), name) == 0) return (int)i;
	return -1;
}

/* ---- values ---- */
hs_value hs_neutral(unsigned cls, unsigned vtype)
{
	hs_value v;
	memset(&v, 0, sizeof v);
	v.type = (uint8_t)(cls > HS_HANDLE ? HS_VOID : cls);
	v.vtype = (uint8_t)vtype;
	if (cls == HS_HANDLE) v.v.h = HS_HANDLE_NONE;
	return v;
}
hs_value hs_make_bool(int b) { hs_value v = hs_neutral(HS_BOOL, 5); v.v.i = b != 0; return v; }
hs_value hs_make_int(int32_t i) { hs_value v = hs_neutral(HS_LONG, 8); v.v.i = i; return v; }
hs_value hs_make_real(float f) { hs_value v = hs_neutral(HS_REAL, 6); v.v.f = f; return v; }
hs_value hs_make_handle(uint32_t h) { hs_value v = hs_neutral(HS_HANDLE, 0); v.v.h = h; return v; }

static int32_t f2i(float f)
{
	if (!(f == f)) return 0;
	if (f >= 2147483520.0f) return 2147483647;
	if (f <= -2147483648.0f) return (-2147483647 - 1);
	return (int32_t)f;
}
static int32_t wrap16(int32_t i) { return (int32_t)(int16_t)(uint16_t)(uint32_t)i; }
static int is_int(unsigned t) { return t == HS_SHORT || t == HS_LONG; }

/* Numeric promotion between short / long / real; void forces a void value; everything else stays as it is
 * (a unit used where an object list is expected is still the host's handle). */
static void coerce(hs_value *v, unsigned cls)
{
	if (v->type == cls) return;
	if (cls == HS_VOID) { v->type = HS_VOID; v->v.i = 0; return; }
	if (v->type == HS_REAL && is_int(cls)) {
		v->v.i = f2i(v->v.f);
		if (cls == HS_SHORT) v->v.i = wrap16(v->v.i);
		v->type = (uint8_t)cls;
	} else if (is_int(v->type) && cls == HS_REAL) {
		v->v.f = (float)v->v.i;
		v->type = HS_REAL;
	} else if (is_int(v->type) && is_int(cls)) {
		if (cls == HS_SHORT) v->v.i = wrap16(v->v.i);
		v->type = (uint8_t)cls;
	}
}

/* ---- binding ---- */
static const hs_reg *find_reg(const hs_machine *m, const char *name)
{
	unsigned i;
	for (i = m->nreg; i > 0; i--)
		if (strcmp(m->reg[i - 1].name, name) == 0) return &m->reg[i - 1];
	return NULL;
}

static void bind_one(hs_machine *m, unsigned b)
{
	const char *name = hs_builtin_name(m->sec, b);
	unsigned op = hs_core_op(name);
	const hs_reg *r = op ? NULL : find_reg(m, name);
	m->bind[b].op = (uint8_t)op;
	m->bind[b].fn = r ? r->fn : NULL;
}
static void bind_all(hs_machine *m)
{
	unsigned b;
	memset(m->bind, 0, sizeof m->bind);
	for (b = 0; m->sec && b < m->sec->builtin_count; b++) bind_one(m, b);
}

void hs_setup(hs_machine *m)
{
	memset(m, 0, sizeof *m);
	m->magic = MACHINE_MAGIC;
	m->node_budget = HS_DEFAULT_NODE_BUDGET;
	m->tick_budget = HS_DEFAULT_TICK_BUDGET;
	m->poll_ticks = HS_DEFAULT_POLL_TICKS;
	hs_core_register(m);
}

void hs_register(hs_machine *m, const char *name, hs_builtin fn)
{
	unsigned i, b;
	size_t n;
	if (m->magic != MACHINE_MAGIC) hs_setup(m);
	if (!name || !fn) return;
	n = strlen(name);
	if (n >= sizeof m->reg[0].name) return;
	for (i = 0; i < m->nreg; i++)
		if (strcmp(m->reg[i].name, name) == 0) break;
	if (i == m->nreg) {
		if (m->nreg >= HS_MAX_REGISTERED) return;
		m->nreg++;
		memcpy(m->reg[i].name, name, n + 1);
	}
	m->reg[i].fn = fn;
	for (b = 0; m->sec && b < m->sec->builtin_count; b++)
		if (strcmp(hs_builtin_name(m->sec, b), name) == 0) bind_one(m, b);
}

void hs_set_log(hs_machine *m, hs_log_fn fn) { m->log = fn; }
void hs_set_budgets(hs_machine *m, uint32_t per_thread, uint32_t per_tick)
{
	m->node_budget = per_thread ? per_thread : 1u;
	m->tick_budget = per_tick ? per_tick : 1u;
}
void hs_set_poll_ticks(hs_machine *m, uint32_t ticks) { m->poll_ticks = ticks ? (ticks > 65535u ? 65535u : ticks) : 1u; }
void hs_seed(hs_machine *m, uint32_t seed) { m->st.rng = seed ? seed : 0x2545F491u; }
hs_state *hs_state_ptr(hs_machine *m) { return &m->st; }
size_t hs_state_size(void) { return sizeof(hs_state); }

uint32_t hs_random(hs_machine *m)
{
	uint32_t x = m->st.rng;
	if (!x) x = 0x2545F491u;
	x ^= x << 13; x ^= x >> 17; x ^= x << 5;
	m->st.rng = x;
	return x;
}

/* ---- threads ---- */
static int is_leaf(const hs_node *n) { return n->kind == HS_N_CONST || n->kind >= HS_N_GLOBAL; }

static int push_frame(hs_machine *m, hs_thread *t, unsigned node)
{
	const hs_node *n = &m->sec->nodes[node];
	hs_frame *f;
	if (t->fsp >= HS_MAX_FRAMES) return -1;
	f = &t->f[t->fsp++];
	f->node = (uint16_t)node;
	f->cur = n->first;
	f->vbase = t->vsp;
	f->a16 = 0;
	f->aux = 0;
	f->phase = 0;
	f->have = 0;
	f->op = n->kind == HS_N_CALL ? m->bind[n->data].op : HS_OP_SCRIPT_CALL;
	f->pad = 0;
	if (t->fsp > m->st.hw_frames) m->st.hw_frames = t->fsp;
	return 0;
}

static void reset_thread(hs_machine *m, hs_thread *t, unsigned script)
{
	const hs_script_rec *sr = &m->sec->scripts[script];
	memset(t, 0, sizeof *t);
	t->script = (uint16_t)script;
	t->state = HS_T_ACTIVE;
	t->wake = m->st.tick;
	if (sr->root != HS_NODE_NONE && !is_leaf(&m->sec->nodes[sr->root])) push_frame(m, t, sr->root);
}

static int start_thread(hs_machine *m, unsigned script)
{
	unsigned i, slot = HS_MAX_THREADS, active = 0;
	hs_state *st = &m->st;
	for (i = 0; i < HS_MAX_THREADS; i++) {
		if (st->threads[i].state != HS_T_FREE) active++;
		else if (slot == HS_MAX_THREADS) slot = i;
	}
	if (slot == HS_MAX_THREADS) { st->thread_overflow++; return -1; }
	reset_thread(m, &st->threads[slot], script);
	st->script_thread[script] = (uint16_t)slot;
	st->threads_started++;
	if (active + 1 > st->hw_threads) st->hw_threads = active + 1;
	return 0;
}

static void free_thread(hs_machine *m, unsigned ti)
{
	hs_thread *t = &m->st.threads[ti];
	if (t->script < HS_MAX_SCRIPTS && m->st.script_thread[t->script] == ti) m->st.script_thread[t->script] = HS_NO_THREAD;
	memset(t, 0, sizeof *t);
}

int hs_wake_script_index(hs_machine *m, unsigned script)
{
	hs_state *st = &m->st;
	unsigned ti;
	if (!m->sec || script >= m->sec->script_count) return -1;
	ti = st->script_thread[script];
	if (ti != HS_NO_THREAD) {
		hs_thread *t = &st->threads[ti];
		if (t->wake > st->tick) t->wake = st->tick;
		st->wakes++;
		return 0;
	}
	if (m->sec->scripts[script].type >= HS_S_STATIC) return -1;
	st->wakes++;
	return start_thread(m, script);
}

int hs_wake_script(hs_machine *m, const char *name)
{
	int i = hs_find_script(m->sec, name);
	return i < 0 ? -1 : hs_wake_script_index(m, (unsigned)i);
}

int hs_script_running(const hs_machine *m, unsigned script)
{
	return m->sec && script < m->sec->script_count && m->st.script_thread[script] != HS_NO_THREAD;
}

unsigned hs_thread_count(const hs_machine *m)
{
	unsigned i, n = 0;
	for (i = 0; i < HS_MAX_THREADS; i++) n += m->st.threads[i].state != HS_T_FREE;
	return n;
}

void hs_sleep_current(hs_machine *m, int32_t ticks)
{
	hs_thread *t = m->cur;
	if (m->sync || !t) return;
	t->wake = ticks < 0 ? HS_FOREVER : m->st.tick + (uint32_t)(ticks < 1 ? 1 : ticks);
	m->yield = 1;
}

int hs_sleep_script(hs_machine *m, unsigned script, int32_t ticks)
{
	unsigned ti;
	hs_thread *t;
	if (!m->sec || script >= m->sec->script_count) return -1;
	ti = m->st.script_thread[script];
	if (ti == HS_NO_THREAD) return -1;
	t = &m->st.threads[ti];
	if (t == m->cur) { hs_sleep_current(m, ticks); return 0; }
	t->wake = ticks < 0 ? HS_FOREVER : m->st.tick + (uint32_t)(ticks < 1 ? 1 : ticks);
	return 0;
}

/* ---- globals ---- */
hs_value hs_global_get(const hs_machine *m, unsigned index)
{
	if (!m->sec || index >= m->sec->global_count) return hs_neutral(HS_VOID, 0);
	return m->st.globals[index];
}
int hs_global_set(hs_machine *m, unsigned index, hs_value v)
{
	const hs_global_rec *g;
	if (!m->sec || index >= m->sec->global_count) return -1;
	g = &m->sec->globals[index];
	coerce(&v, g->cls);
	v.vtype = (uint8_t)g->vtype;
	v.aux = 0;
	m->st.globals[index] = v;
	return 0;
}

/* ---- evaluator ---- */
static hs_value leaf_value(hs_machine *m, const hs_thread *t, const hs_node *n)
{
	hs_value v;
	switch (n->kind) {
	case HS_N_GLOBAL:
		v = m->st.globals[n->data];
		coerce(&v, n->cls);
		return v;
	case HS_N_PARAM: {
		unsigned idx = (unsigned)t->pbase + n->data;
		if (idx < t->vsp) { v = t->v[idx]; coerce(&v, n->cls); return v; }
		return hs_neutral(n->cls, n->vtype);
	}
	default:
		memset(&v, 0, sizeof v);
		v.type = n->cls;
		v.vtype = n->vtype;
		v.v.h = n->data;
		return v;
	}
}

static int as_int(const hs_value *v) { return v->type == HS_REAL ? f2i(v->v.f) : v->v.i; }

/* Runs `t` until it finishes, sleeps / blocks, runs out of budget or fails. */
static int run_thread(hs_machine *m, hs_thread *t, uint32_t *budget)
{
	const hs_node *nodes = m->sec->nodes;
	hs_state *st = &m->st;
	unsigned err = 0;

#define PUSHV(val) do { if (t->vsp >= HS_MAX_VALUES) { err = ERR_VALUES; goto fail; } t->v[t->vsp++] = (val); \
		if (t->vsp > st->hw_values) st->hw_values = t->vsp; } while (0)
#define EVAL(c) do { unsigned c_ = (c); const hs_node *cn_ = &nodes[c_]; \
		if (*budget) { --*budget; } st->nodes_tick++; \
		if (is_leaf(cn_)) { hs_value lv_ = leaf_value(m, t, cn_); PUSHV(lv_); f->have = 1; } \
		else if (push_frame(m, t, c_)) { err = ERR_FRAMES; goto fail; } } while (0)
#define POPV(dst) do { if (t->vsp <= f->vbase) { err = ERR_PHASE; goto fail; } (dst) = t->v[--t->vsp]; } while (0)
#define FINISH() do { \
		if (t->vsp <= f->vbase) { err = ERR_PHASE; goto fail; } \
		if (t->vsp != f->vbase + 1u) { t->v[f->vbase] = t->v[t->vsp - 1u]; t->vsp = (uint8_t)(f->vbase + 1u); } \
		coerce(&t->v[f->vbase], nd->cls); \
		t->fsp--; if (t->fsp) t->f[t->fsp - 1u].have = 1; \
		goto top; } while (0)
#define PUSH_FINISH(val) do { hs_value pv_ = (val); PUSHV(pv_); FINISH(); } while (0)
/* push the values of the remaining arguments; a call argument suspends the frame until its result arrives */
#define COLLECT_ARGS() do { while (f->cur != HS_NODE_NONE) { unsigned c_ = f->cur; f->cur = nodes[c_].next; \
		if (is_leaf(&nodes[c_])) { hs_value lv_; if (*budget) { --*budget; } st->nodes_tick++; \
			lv_ = leaf_value(m, t, &nodes[c_]); PUSHV(lv_); } \
		else { if (*budget) { --*budget; } st->nodes_tick++; \
			if (push_frame(m, t, c_)) { err = ERR_FRAMES; goto fail; } goto top; } } } while (0)

top:
	for (;;) {
		hs_frame *f;
		const hs_node *nd;
		if (t->fsp == 0) return R_DONE;
		if (*budget == 0) {
			st->budget_hits++;
			t->wake = st->tick + 1u;
			return R_YIELD;
		}
		f = &t->f[t->fsp - 1u];
		nd = &nodes[f->node];
		switch (f->op) {
		case HS_OP_BEGIN:
			if (f->have) {
				f->have = 0;
				if (f->cur == HS_NODE_NONE) FINISH();
				if (t->vsp <= f->vbase) { err = ERR_PHASE; goto fail; }
				t->vsp--;
			}
			if (f->cur == HS_NODE_NONE) PUSH_FINISH(hs_neutral(HS_VOID, nd->vtype));
			{ unsigned c = f->cur; f->cur = nodes[c].next; EVAL(c); }
			continue;

		case HS_OP_IF:
			if (f->phase == 0) {
				unsigned c = f->cur;
				if (c == HS_NODE_NONE) PUSH_FINISH(hs_neutral(HS_VOID, nd->vtype));
				f->cur = nodes[c].next;
				f->phase = 1;
				EVAL(c);
				continue;
			}
			if (f->phase == 1) {
				hs_value cond;
				unsigned then = f->cur, pick;
				POPV(cond);
				f->have = 0;
				pick = cond.v.i ? then : (then != HS_NODE_NONE ? nodes[then].next : HS_NODE_NONE);
				f->phase = 2;
				if (pick == HS_NODE_NONE) PUSH_FINISH(hs_neutral(HS_VOID, nd->vtype));
				EVAL(pick);
				continue;
			}
			f->have = 0;
			FINISH();

		case HS_OP_COND:                 /* arguments are (test, result) pairs; unverified, no map uses it */
			if (f->phase == 0) {
				unsigned test = f->cur, res;
				if (test == HS_NODE_NONE) PUSH_FINISH(hs_neutral(HS_VOID, nd->vtype));
				res = nodes[test].next;
				f->a16 = (uint16_t)res;
				f->cur = res == HS_NODE_NONE ? HS_NODE_NONE : nodes[res].next;
				f->phase = 1;
				EVAL(test);
				continue;
			}
			if (f->phase == 1) {
				hs_value x;
				POPV(x);
				f->have = 0;
				if (!x.v.i) { f->phase = 0; continue; }
				if (f->a16 == HS_NODE_NONE) PUSH_FINISH(hs_neutral(HS_VOID, nd->vtype));
				f->phase = 2;
				EVAL(f->a16);
				continue;
			}
			f->have = 0;
			FINISH();

		case HS_OP_AND:
		case HS_OP_OR: {
			int is_and = f->op == HS_OP_AND;
			if (f->have) {
				hs_value x;
				POPV(x);
				f->have = 0;
				if (is_and ? !x.v.i : x.v.i) PUSH_FINISH(hs_make_bool(!is_and));
			}
			if (f->cur == HS_NODE_NONE) PUSH_FINISH(hs_make_bool(is_and));
			{ unsigned c = f->cur; f->cur = nodes[c].next; EVAL(c); }
			continue;
		}

		case HS_OP_SET:
			if (f->phase == 0) {
				unsigned target = nd->first, val = target == HS_NODE_NONE ? HS_NODE_NONE : nodes[target].next;
				if (val == HS_NODE_NONE || nodes[target].kind != HS_N_GLOBAL) PUSH_FINISH(hs_neutral(HS_VOID, nd->vtype));
				f->phase = 1;
				EVAL(val);
				continue;
			} else {
				hs_value x;
				const hs_global_rec *g;
				if (nd->first == HS_NODE_NONE || nodes[nd->first].kind != HS_N_GLOBAL) { err = ERR_PHASE; goto fail; }
				g = &m->sec->globals[nodes[nd->first].data];
				POPV(x);
				f->have = 0;
				coerce(&x, g->cls);
				x.vtype = (uint8_t)g->vtype;
				x.aux = 0;
				st->globals[nodes[nd->first].data] = x;
				PUSH_FINISH(hs_neutral(HS_VOID, nd->vtype));
			}

		case HS_OP_BEGIN_RANDOM: {
			unsigned n = nd->nargs, remaining = 0, k, idx = 0, c;
			if (f->have) {
				f->have = 0;
				if (t->vsp <= f->vbase) { err = ERR_PHASE; goto fail; }
				t->vsp--;
			}
			if (f->a16 >= n) PUSH_FINISH(hs_neutral(HS_VOID, nd->vtype));
			c = nd->first;
			if (n > 32u) {                 /* more than a mask holds: run in order (no map has more than 14) */
				for (k = f->a16; k > 0; k--) c = nodes[c].next;
			} else {
				remaining = n - f->a16;
				k = hs_random(m) % remaining;
				for (;; idx++, c = nodes[c].next) {
					if (c == HS_NODE_NONE || idx >= 32u) { err = ERR_PHASE; goto fail; }
					if (f->aux & (1u << idx)) continue;
					if (k == 0) break;
					k--;
				}
				f->aux |= 1u << idx;
			}
			f->a16++;
			EVAL(c);
			continue;
		}

		case HS_OP_SLEEP_UNTIL: {
			unsigned c0 = nd->first, sleep = 0;
			switch (f->phase) {
			case 0:
				if (c0 == HS_NODE_NONE) PUSH_FINISH(hs_neutral(HS_VOID, nd->vtype));
				f->cur = nodes[c0].next;
				f->phase = 1;
				EVAL(c0);
				continue;
			case 1: {                       /* first test */
				hs_value x;
				POPV(x);
				f->have = 0;
				if (x.v.i || m->sync) PUSH_FINISH(hs_neutral(HS_VOID, nd->vtype));
				f->aux = HS_FOREVER;
				f->a16 = (uint16_t)m->poll_ticks;
				if (f->cur == HS_NODE_NONE) { sleep = 1; break; }
				{ unsigned c1 = f->cur; f->cur = nodes[c1].next; f->phase = 2; EVAL(c1); }
				continue;
			}
			case 2: {                       /* period */
				hs_value x;
				int p;
				POPV(x);
				p = as_int(&x);
				f->have = 0;
				f->a16 = (uint16_t)(p < 1 ? 1 : p > 65535 ? 65535 : p);
				if (f->cur == HS_NODE_NONE) { sleep = 1; break; }
				{ unsigned c2 = f->cur; f->cur = HS_NODE_NONE; f->phase = 3; EVAL(c2); }
				continue;
			}
			case 3: {                       /* timeout in ticks */
				hs_value x;
				int tm;
				POPV(x);
				tm = as_int(&x);
				f->have = 0;
				if (tm > 0) f->aux = st->tick + (uint32_t)tm;
				sleep = 1;
				break;
			}
			case 4:                         /* woke up: test again */
				f->phase = 5;
				EVAL(c0);
				continue;
			case 5: {
				hs_value x;
				POPV(x);
				f->have = 0;
				if (x.v.i || (f->aux != HS_FOREVER && st->tick >= f->aux)) PUSH_FINISH(hs_neutral(HS_VOID, nd->vtype));
				sleep = 1;
				break;
			}
			default:
				err = ERR_PHASE;
				goto fail;
			}
			if (sleep) {
				uint32_t w = st->tick + f->a16;
				if (f->aux != HS_FOREVER && w > f->aux) w = f->aux;
				f->phase = 4;
				t->wake = w;
				return R_YIELD;
			}
			continue;
		}

		case HS_OP_SCRIPT_CALL: {
			const hs_script_rec *sr = &m->sec->scripts[nd->data];
			if (f->phase == 0) {
				unsigned i, np;
				COLLECT_ARGS();
				np = t->vsp - f->vbase;
				if (sr->root == HS_NODE_NONE) { t->vsp = f->vbase; PUSH_FINISH(hs_neutral(sr->rcls, sr->rtype)); }
				if (t->calls >= HS_MAX_CALL_DEPTH) { err = ERR_DEPTH; goto fail; }
				for (i = 0; i < np && i < sr->nparams; i++) coerce(&t->v[f->vbase + i], sr->pcls[i]);
				f->phase = 1;
				f->a16 = t->pbase;
				t->pbase = f->vbase;
				t->calls++;
				if (t->calls > st->hw_calls) st->hw_calls = t->calls;
				EVAL(sr->root);
				continue;
			} else {
				f->have = 0;
				if (t->vsp <= f->vbase) { err = ERR_PHASE; goto fail; }
				t->v[f->vbase] = t->v[t->vsp - 1u];
				t->vsp = (uint8_t)(f->vbase + 1u);
				t->v[f->vbase].vtype = (uint8_t)sr->rtype;
				t->pbase = f->a16;
				t->calls--;
				FINISH();
			}
		}

		default: {                          /* HS_OP_GENERIC: evaluate every argument, then call */
			const hs_bind *b = &m->bind[nd->data];
			hs_value ret = hs_neutral(nd->cls, nd->vtype);
			COLLECT_ARGS();
			if (b->fn) {
				int rc;
				m->cur = t;
				m->yield = 0;
				rc = b->fn(m, m->host, &t->v[f->vbase], (unsigned)(t->vsp - f->vbase), &ret);
				if (rc && !m->sync) {       /* would block: same arguments again next tick */
					st->blocked_calls++;
					t->wake = st->tick + 1u;
					return R_YIELD;
				}
			} else {
				st->unreg[nd->data]++;
				st->unreg_total++;
			}
			t->vsp = f->vbase;
			ret.aux = 0;
			{
				unsigned y = (unsigned)m->yield;
				PUSHV(ret);
				m->yield = 0;
				if (y) {                    /* sleep / sleep_forever asked to suspend the thread: finish the call first */
					t->vsp = f->vbase + 1u;
					coerce(&t->v[f->vbase], nd->cls);
					t->fsp--;
					if (t->fsp) t->f[t->fsp - 1u].have = 1;
					return R_YIELD;
				}
			}
			FINISH();
		}
		}
	}
fail:
	st->errors++;
	st->last_error = err;
	st->last_error_script = t->script;
	return R_ERROR;
#undef PUSHV
#undef EVAL
#undef POPV
#undef FINISH
#undef PUSH_FINISH
#undef COLLECT_ARGS
}

/* ---- synchronous evaluation ---- */
hs_value hs_run_expression(hs_machine *m, unsigned node)
{
	hs_thread *save_cur = m->cur;
	int save_sync = m->sync, save_yield = m->yield;
	hs_value r = hs_neutral(HS_VOID, 4);
	uint32_t budget = 1u << 20;
	hs_thread *t = &m->scratch;

	if (!m->sec || node >= m->sec->node_count) return r;
	if (is_leaf(&m->sec->nodes[node])) return leaf_value(m, t, &m->sec->nodes[node]);
	memset(t, 0, sizeof *t);
	t->state = HS_T_ACTIVE;
	t->script = HS_NO_THREAD;
	m->cur = t;
	m->sync = 1;
	if (push_frame(m, t, node) == 0 && run_thread(m, t, &budget) == R_DONE && t->vsp > 0) r = t->v[0];
	m->cur = save_cur;
	m->sync = save_sync;
	m->yield = save_yield;
	return r;
}

hs_value hs_call_script(hs_machine *m, unsigned script)
{
	if (!m->sec || script >= m->sec->script_count || m->sec->scripts[script].root == HS_NODE_NONE)
		return hs_neutral(HS_VOID, 4);
	return hs_run_expression(m, m->sec->scripts[script].root);
}

/* ---- lifecycle ---- */
hs_status hs_init(hs_machine *m, const hs_section *s, void *host)
{
	unsigned i;
	hs_state *st;
	if (m->magic != MACHINE_MAGIC) hs_setup(m);
	if (!s || !s->nodes) return HS_ERR_LAYOUT;
	m->sec = s;
	m->host = host;
	m->cur = NULL; m->yield = 0; m->sync = 0;
	st = &m->st;
	memset(st, 0, sizeof *st);
	st->magic = HS_STATE_MAGIC;
	st->version = HS_STATE_VERSION;
	st->section_hash = s->hash;
	st->rng = 0x2545F491u;
	for (i = 0; i < HS_MAX_SCRIPTS; i++) st->script_thread[i] = HS_NO_THREAD;
	bind_all(m);
	for (i = 0; i < s->global_count; i++) st->globals[i] = hs_neutral(s->globals[i].cls, s->globals[i].vtype);
	for (i = 0; i < s->global_count; i++) {      /* initialisers run in declaration order, sleeps skipped */
		if (s->globals[i].init != HS_NODE_NONE && !(s->globals[i].flags & HS_G_EXTERNAL))
			hs_global_set(m, i, hs_run_expression(m, s->globals[i].init));
	}
	for (i = 0; i < s->script_count; i++)
		if (s->scripts[i].type == HS_S_STARTUP || s->scripts[i].type == HS_S_CONTINUOUS) start_thread(m, i);
	st->nodes_tick = st->nodes_max_tick = st->nodes_total = 0;     /* initialisation is not a tick */
	return HS_OK;
}

static int state_ok(const hs_machine *m)
{
	const hs_section *s = m->sec;
	const hs_state *st = &m->st;
	unsigned i, k;
	if (st->magic != HS_STATE_MAGIC || st->version != HS_STATE_VERSION || st->section_hash != s->hash) return 0;
	for (i = 0; i < s->global_count; i++)
		if (st->globals[i].type > HS_HANDLE) return 0;
	for (i = 0; i < HS_MAX_SCRIPTS; i++) {
		unsigned ti = st->script_thread[i];
		if (ti == HS_NO_THREAD) continue;
		if (i >= s->script_count || ti >= HS_MAX_THREADS || st->threads[ti].state != HS_T_ACTIVE || st->threads[ti].script != i) return 0;
	}
	for (i = 0; i < HS_MAX_THREADS; i++) {
		const hs_thread *t = &st->threads[i];
		unsigned prev = 0;
		if (t->state == HS_T_FREE) continue;
		if (t->state != HS_T_ACTIVE || t->script >= s->script_count || st->script_thread[t->script] != i) return 0;
		if (t->fsp > HS_MAX_FRAMES || t->vsp > HS_MAX_VALUES || t->calls > HS_MAX_CALL_DEPTH || t->pbase > HS_MAX_VALUES) return 0;
		for (k = 0; k < t->vsp; k++)
			if (t->v[k].type > HS_HANDLE) return 0;
		for (k = 0; k < t->fsp; k++) {
			const hs_frame *f = &t->f[k];
			const hs_node *n;
			if (f->node >= s->node_count) return 0;
			n = &s->nodes[f->node];
			if (n->kind != HS_N_CALL && n->kind != HS_N_SCRIPT_CALL) return 0;
			if (f->op != (n->kind == HS_N_CALL ? m->bind[n->data].op : HS_OP_SCRIPT_CALL)) return 0;
			if ((f->cur != HS_NODE_NONE && f->cur >= s->node_count) || f->vbase < prev || f->vbase > t->vsp || f->phase > 5) return 0;
			if (f->op == HS_OP_BEGIN_RANDOM && f->a16 > n->nargs) return 0;
			if (f->op == HS_OP_COND && f->a16 != HS_NODE_NONE && f->a16 >= s->node_count) return 0;
			prev = f->vbase;
		}
	}
	return 1;
}

hs_status hs_rebind(hs_machine *m, const hs_section *s, void *host)
{
	if (m->magic != MACHINE_MAGIC) hs_setup(m);
	if (!s || !s->nodes) return HS_ERR_LAYOUT;
	m->sec = s;
	m->host = host;
	m->cur = NULL; m->yield = 0; m->sync = 0;
	bind_all(m);
	if (!state_ok(m)) {
		memset(&m->st, 0, sizeof m->st);
		m->sec = NULL;
		return HS_ERR_STATE;
	}
	return HS_OK;
}

void hs_tick(hs_machine *m)
{
	hs_state *st = &m->st;
	uint32_t now = st->tick, spent = 0, i;
	if (!m->sec) return;
	st->nodes_tick = 0;
	m->sync = 0;
	for (i = 0; i < HS_MAX_THREADS; i++) {
		unsigned ti = (st->scan_start + i) % HS_MAX_THREADS;
		hs_thread *t = &st->threads[ti];
		uint32_t budget, before;
		int rc;
		if (t->state != HS_T_ACTIVE || t->wake > now) continue;
		if (spent >= m->tick_budget) {
			st->tick_budget_hits++;
			st->scan_start = ti;
			goto done;
		}
		budget = m->node_budget;
		if (budget > m->tick_budget - spent) budget = m->tick_budget - spent;
		before = budget;
		m->cur = t;
		m->yield = 0;
		rc = run_thread(m, t, &budget);
		spent += before - budget;
		if (rc == R_DONE) {
			if (m->sec->scripts[t->script].type == HS_S_CONTINUOUS) {
				unsigned sc = t->script;
				reset_thread(m, t, sc);
				t->wake = now + 1u;
			} else {
				free_thread(m, ti);
			}
		} else if (rc == R_ERROR) {
			free_thread(m, ti);
		}
	}
	st->scan_start = 0;
done:
	m->cur = NULL;
	st->nodes_total += st->nodes_tick;
	if (st->nodes_tick > st->nodes_max_tick) st->nodes_max_tick = st->nodes_tick;
	st->tick = now + 1u;
}

unsigned hs_unregistered(const hs_machine *m, unsigned k, const char **name, uint32_t *calls)
{
	unsigned b, n = 0;
	for (b = 0; m->sec && b < m->sec->builtin_count; b++) {
		if (!m->st.unreg[b]) continue;
		if (n == k) {
			if (name) *name = hs_builtin_name(m->sec, b);
			if (calls) *calls = m->st.unreg[b];
		}
		n++;
	}
	return n;
}
