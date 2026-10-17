/* Host test of the script machine (docs/24-script-section.md): synthetic worlds built in this file with a small SCRP
 * writer exercise every construct (arithmetic and its type conversions, logic with short circuit, if / cond / begin /
 * begin_random, set, script calls with return values and parameters, static scripts that sleep, recursion limits),
 * the thread semantics (sleep n, sleep_until period / default / timeout, wake, sleep_forever, continuous scripts,
 * blocking built-ins, thread cap, node budgets, unregistered built-ins), the snapshot / restore contract, corrupt
 * sections and corrupt states (ASan/UBSan), then runs every real SCRP pack given on the command line for 3000
 * ticks against a neutral host and against a chaotic one.
 *
 *   test_hs <fixture.hp2 written by tools/tests/test_scriptconv.py> [<map>.script.hp2 ...]
 * Set HS_BENCH=1 for the timing report (build without sanitizers). */
#define _POSIX_C_SOURCE 200809L
#include "script/hs.h"
#include "stream/hp2.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

/* ======================================================================================================
 * SCRP writer
 * ====================================================================================================== */
typedef struct tn {
	uint8_t kind, cls, vtype;
	uint32_t data;
	int nk;
	struct tn *kid[300];
} tn;

static tn pool[3000];
static int npool;
static const char *bnames[HS_MAX_BUILTINS];
static int nbn;
static struct { const char *name; int type, rtype, rcls, nparams; uint8_t pcls[HS_MAX_PARAMS]; tn *root; } bs[HS_MAX_SCRIPTS + 8];
static int nbs;
static struct { const char *name; int cls, vtype, ext; tn *init; } bg[HS_MAX_GLOBALS + 8];
static int nbg;
static char spool[16384];
static unsigned nspool;

static void reset_builder(void)
{
	npool = nbn = nbs = nbg = 0;
	spool[0] = 0;
	nspool = 1;
}
static unsigned sidx(const char *s)
{
	unsigned off = 1;
	size_t n = strlen(s);
	if (!n) return 0;
	while (off < nspool) {
		if (strcmp(spool + off, s) == 0) return off;
		off += (unsigned)strlen(spool + off) + 1;
	}
	CHECK(nspool + n + 1 < sizeof spool);
	memcpy(spool + nspool, s, n + 1);
	off = nspool;
	nspool += (unsigned)n + 1;
	return off;
}
static int vt_of(int cls) { static const int v[] = { 4, 5, 6, 7, 8, 9, 10, 37 }; return v[cls]; }
static tn *mk(int kind, int cls, uint32_t data)
{
	tn *n;
	CHECK(npool < 3000);
	n = &pool[npool++];
	memset(n, 0, sizeof *n);
	n->kind = (uint8_t)kind; n->cls = (uint8_t)cls; n->vtype = (uint8_t)vt_of(cls); n->data = data;
	return n;
}
static tn *Cb(int b) { return mk(HS_N_CONST, HS_BOOL, b != 0); }
static tn *Cs(int i) { return mk(HS_N_CONST, HS_SHORT, (uint32_t)i); }
static tn *Cl(int32_t i) { return mk(HS_N_CONST, HS_LONG, (uint32_t)i); }
static tn *Cr(float f) { uint32_t u; memcpy(&u, &f, 4); return mk(HS_N_CONST, HS_REAL, u); }
static tn *Ch(uint32_t h) { return mk(HS_N_CONST, HS_HANDLE, h); }
static tn *Cstr(const char *s) { return mk(HS_N_CONST, HS_STRING, sidx(s)); }
static tn *Cscr(int script) { return mk(HS_N_CONST, HS_SCRIPT, (uint32_t)script); }
static tn *G(int idx, int cls) { return mk(HS_N_GLOBAL, cls, (uint32_t)idx); }
static tn *P(int slot, int cls) { return mk(HS_N_PARAM, cls, (uint32_t)slot); }
static int bi(const char *name)
{
	int i;
	for (i = 0; i < nbn; i++)
		if (strcmp(bnames[i], name) == 0) return i;
	CHECK(nbn < (int)HS_MAX_BUILTINS);
	bnames[nbn] = name;
	return nbn++;
}
static tn *vcall(int kind, uint32_t data, int cls, va_list ap)
{
	tn *n = mk(kind, cls, data), *k;
	while ((k = va_arg(ap, tn *)) != NULL) { CHECK(n->nk < 300); n->kid[n->nk++] = k; }
	return n;
}
static tn *F(const char *name, int cls, ...)
{
	va_list ap;
	tn *n;
	va_start(ap, cls);
	n = vcall(HS_N_CALL, (uint32_t)bi(name), cls, ap);
	va_end(ap);
	return n;
}
static tn *SC(int script, int cls, ...)
{
	va_list ap;
	tn *n;
	va_start(ap, cls);
	n = vcall(HS_N_SCRIPT_CALL, (uint32_t)script, cls, ap);
	va_end(ap);
	return n;
}
static tn *BEGIN(int cls, ...)
{
	va_list ap;
	tn *n;
	va_start(ap, cls);
	n = vcall(HS_N_CALL, (uint32_t)bi("begin"), cls, ap);
	va_end(ap);
	return n;
}
#define FN(name, cls, ...) F(name, cls, __VA_ARGS__, NULL)
#define FN0(name, cls) F(name, cls, NULL)
static tn *SET(int g, tn *v) { return F("set", HS_VOID, G(g, HS_VOID), v, NULL); }
static tn *SLEEP(int n) { return FN("sleep", HS_VOID, Cs(n)); }

static int add_script(const char *name, int type, int rcls, tn *root)
{
	CHECK(nbs < (int)HS_MAX_SCRIPTS);
	bs[nbs].name = name; bs[nbs].type = type; bs[nbs].rcls = rcls; bs[nbs].rtype = vt_of(rcls);
	bs[nbs].nparams = 0; bs[nbs].root = root;
	return nbs++;
}
static int add_script_p(const char *name, int rcls, int np, const uint8_t *pcls, tn *root)
{
	int i = add_script(name, HS_S_STATIC, rcls, root);
	bs[i].nparams = np;
	memcpy(bs[i].pcls, pcls, (size_t)np);
	return i;
}
static int add_global(const char *name, int cls, tn *init, int ext)
{
	CHECK(nbg < (int)HS_MAX_GLOBALS);
	bg[nbg].name = name; bg[nbg].cls = cls; bg[nbg].vtype = vt_of(cls); bg[nbg].init = init; bg[nbg].ext = ext;
	return nbg++;
}

typedef struct { uint8_t kind, cls, vtype, zero; uint16_t next, first, nargs, zero2; uint32_t data; } rnode;

static int emit(rnode *out, int *n, const tn *t)
{
	int idx = (*n)++, prev = -1, i;
	out[idx].kind = t->kind; out[idx].cls = t->cls; out[idx].vtype = t->vtype; out[idx].zero = 0;
	out[idx].next = HS_NODE_NONE; out[idx].first = HS_NODE_NONE; out[idx].nargs = (uint16_t)t->nk; out[idx].zero2 = 0;
	out[idx].data = t->data;
	for (i = 0; i < t->nk; i++) {
		int k = emit(out, n, t->kid[i]);
		if (prev < 0) out[idx].first = (uint16_t)k;
		else out[prev].next = (uint16_t)k;
		prev = k;
	}
	return idx;
}

static void wr16(uint8_t *p, unsigned v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void wr32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static size_t al16(size_t n) { return (n + 15u) & ~(size_t)15u; }

/* builds the SCRP section of everything declared; returns a malloc'ed (aligned) buffer */
static uint8_t *finish(size_t *size_out)
{
	static rnode nodes[8192];
	int nn = 0, i;
	unsigned ns = (unsigned)nbs, ng = (unsigned)nbg, nb = (unsigned)nbn, nx = 0;
	unsigned root_idx[HS_MAX_SCRIPTS + 8], init_idx[HS_MAX_GLOBALS + 8];
	size_t o_nodes, o_sc, o_gl, o_bi, o_cls, o_str, total;
	unsigned name_off[HS_MAX_BUILTINS];
	uint8_t *b;
	/* builtin names go into the pool */
	for (i = 0; i < nbn; i++) name_off[i] = sidx(bnames[i]);
	{
		unsigned sname[HS_MAX_SCRIPTS + 8], gname[HS_MAX_GLOBALS + 8];
		for (i = 0; i < nbs; i++) sname[i] = sidx(bs[i].name);
		for (i = 0; i < nbg; i++) gname[i] = sidx(bg[i].name);
		for (i = 0; i < nbs; i++) root_idx[i] = bs[i].root ? (unsigned)emit(nodes, &nn, bs[i].root) : HS_NODE_NONE;
		for (i = 0; i < nbg; i++) { init_idx[i] = bg[i].init ? (unsigned)emit(nodes, &nn, bg[i].init) : HS_NODE_NONE; nx += (unsigned)bg[i].ext; }
		o_nodes = 128;
		o_sc = al16(o_nodes + (size_t)nn * 16);
		o_gl = al16(o_sc + (size_t)ns * 32);
		o_bi = al16(o_gl + (size_t)ng * 16);
		o_cls = al16(o_bi + (size_t)nb * 16);
		o_str = al16(o_cls + 64);
		total = al16(o_str + nspool);
		b = calloc(1, total);
		CHECK(b);
		memcpy(b, "SCRP", 4);
		wr16(b + 4, 1);
		wr32(b + 8, (uint32_t)total); wr32(b + 0x0C, (uint32_t)nn); wr32(b + 0x10, ns); wr32(b + 0x14, ng);
		wr32(b + 0x18, nb); wr32(b + 0x1C, nx); wr32(b + 0x20, nspool);
		wr32(b + 0x24, (uint32_t)o_nodes); wr32(b + 0x28, (uint32_t)o_sc); wr32(b + 0x2C, (uint32_t)o_gl);
		wr32(b + 0x30, (uint32_t)o_bi); wr32(b + 0x34, (uint32_t)o_str); wr32(b + 0x38, (uint32_t)o_cls);
		memcpy(b + o_nodes, nodes, (size_t)nn * 16);
		for (i = 0; i < nbs; i++) {
			uint8_t *p = b + o_sc + (size_t)i * 32;
			wr32(p, sname[i]); wr16(p + 4, (unsigned)bs[i].type); wr16(p + 6, (unsigned)bs[i].rtype);
			wr16(p + 8, root_idx[i]); p[10] = (uint8_t)bs[i].rcls; p[11] = (uint8_t)bs[i].nparams;
			memcpy(p + 12, bs[i].pcls, HS_MAX_PARAMS);
		}
		for (i = 0; i < nbg; i++) {              /* external globals come last, as the converter writes them */
			uint8_t *p = b + o_gl + (size_t)i * 16;
			CHECK(i < nbg - (int)nx || bg[i].ext);
			wr32(p, gname[i]); wr16(p + 4, (unsigned)bg[i].vtype); p[6] = (uint8_t)bg[i].cls;
			p[7] = (uint8_t)(bg[i].ext ? 1 : 0); wr16(p + 8, init_idx[i]);
		}
	}
	for (i = 0; i < nbn; i++) { uint8_t *p = b + o_bi + (size_t)i * 16; wr32(p, name_off[i]); }
	for (i = 0; i < 64; i++) b[o_cls + (size_t)i] = (uint8_t)(i == 5 ? HS_BOOL : i == 6 ? HS_REAL : i == 7 ? HS_SHORT : i == 8 ? HS_LONG :
	                                                         i == 9 ? HS_STRING : i == 10 ? HS_SCRIPT : i == 4 ? HS_VOID : HS_HANDLE);
	memcpy(b + o_str, spool, nspool);
	*size_out = total;
	return b;
}
/* NOTE: tests with external globals declare them last (after all scenario globals), as the converter does. */

/* ======================================================================================================
 * host
 * ====================================================================================================== */
typedef struct {
	int calls;
	int seq[1024];
	int nseq;
	int block_left;
	int flag_a, flag_b;
	char log[8][64];
	int nlog;
} host_t;

static int h_cnt(hs_machine *m, void *h, const hs_value *a, unsigned n, hs_value *r)
{
	host_t *H = h;
	(void)m; (void)a; (void)n;
	r->v.i = ++H->calls;
	return 0;
}
static int h_mark(hs_machine *m, void *h, const hs_value *a, unsigned n, hs_value *r)
{
	host_t *H = h;
	(void)m; (void)r;
	if (n && H->nseq < 1024) H->seq[H->nseq++] = a[0].v.i;
	return 0;
}
static int h_tickstamp(hs_machine *m, void *h, const hs_value *a, unsigned n, hs_value *r)
{
	(void)h; (void)a; (void)n;
	r->v.i = (int32_t)hs_state_ptr(m)->tick;
	return 0;
}
static int h_blocker(hs_machine *m, void *h, const hs_value *a, unsigned n, hs_value *r)
{
	host_t *H = h;
	(void)m; (void)a; (void)n; (void)r;
	return H->block_left-- > 0;
}
static int h_flag_a(hs_machine *m, void *h, const hs_value *a, unsigned n, hs_value *r) { (void)m; (void)a; (void)n; r->v.i = ((host_t *)h)->flag_a; return 0; }
static int h_flag_b(hs_machine *m, void *h, const hs_value *a, unsigned n, hs_value *r) { (void)m; (void)a; (void)n; r->v.i = ((host_t *)h)->flag_b; return 0; }
static void h_log(void *h, const char *msg)
{
	host_t *H = h;
	if (H->nlog < 8) { strncpy(H->log[H->nlog], msg, 63); H->log[H->nlog][63] = 0; H->nlog++; }
}

static void reg_host(hs_machine *m)
{
	hs_register(m, "cnt", h_cnt);
	hs_register(m, "mark", h_mark);
	hs_register(m, "tickstamp", h_tickstamp);
	hs_register(m, "blocker", h_blocker);
	hs_register(m, "flag_a", h_flag_a);
	hs_register(m, "flag_b", h_flag_b);
}

/* one machine + section per test */
typedef struct {
	uint8_t *blob;
	size_t size;
	hs_section sec;
	hs_machine m;
	host_t host;
} world;


static world *world_new(void)
{
	world *w = calloc(1, sizeof *w);
	CHECK(w);
	return w;
}
static void world_open(world *w)
{
	w->blob = finish(&w->size);
	CHECK(hs_open(&w->sec, w->blob, w->size) == HS_OK);
	hs_setup(&w->m);
	reg_host(&w->m);
	hs_set_log(&w->m, h_log);
	CHECK(hs_init(&w->m, &w->sec, &w->host) == HS_OK);
}
static void world_free(world *w) { free(w->blob); free(w); }
static void ticks(world *w, int n) { while (n-- > 0) hs_tick(&w->m); }
static int gi(world *w, int g) { return hs_global_get(&w->m, (unsigned)g).v.i; }
static float gf(world *w, int g) { return hs_global_get(&w->m, (unsigned)g).v.f; }
static hs_state *S(world *w) { return hs_state_ptr(&w->m); }

/* ======================================================================================================
 * tests
 * ====================================================================================================== */
static void test_arithmetic(void)
{
	world *w = world_new();
	int s_long, s_real, s_short, s_wrap, s_div, s_div0, s_minmax, s_cmp_i, s_cmp_r, s_eq_h, s_eq_b, s_neg, s_sub;
	hs_value v;
	reset_builder();
	s_long = add_script("a_long", HS_S_STATIC, HS_LONG, BEGIN(HS_LONG, FN("*", HS_LONG, Cr(30.0f), Cr(300.0f)), NULL));
	s_real = add_script("a_real", HS_S_STATIC, HS_REAL, BEGIN(HS_REAL, FN("+", HS_REAL, Cr(1.5f), Cr(2.25f)), NULL));
	s_short = add_script("a_short", HS_S_STATIC, HS_SHORT, BEGIN(HS_SHORT, FN("+", HS_SHORT, Cr(1.0f), Cr(2.9f)), NULL));
	s_wrap = add_script("a_wrap", HS_S_STATIC, HS_SHORT, BEGIN(HS_SHORT, FN("+", HS_SHORT, Cr(32767.0f), Cr(2.0f)), NULL));
	s_div = add_script("a_div", HS_S_STATIC, HS_REAL, BEGIN(HS_REAL, FN("/", HS_REAL, Cr(7.0f), Cr(2.0f)), NULL));
	s_div0 = add_script("a_div0", HS_S_STATIC, HS_REAL, BEGIN(HS_REAL, FN("/", HS_REAL, Cr(1.0f), Cr(0.0f)), NULL));
	s_minmax = add_script("a_minmax", HS_S_STATIC, HS_REAL,
	    BEGIN(HS_REAL, FN("+", HS_REAL, FN("max", HS_REAL, Cr(1.0f), Cr(5.0f), NULL), FN("min", HS_REAL, Cr(3.0f), Cr(-4.0f), Cr(9.0f), NULL)), NULL));
	s_cmp_i = add_script("a_cmp_i", HS_S_STATIC, HS_BOOL, BEGIN(HS_BOOL, FN("<", HS_BOOL, Cs(3), Cs(5)), NULL));
	s_cmp_r = add_script("a_cmp_r", HS_S_STATIC, HS_BOOL, BEGIN(HS_BOOL, FN(">=", HS_BOOL, Cr(2.5f), Cr(2.5f)), NULL));
	s_eq_h = add_script("a_eq_h", HS_S_STATIC, HS_BOOL, BEGIN(HS_BOOL, FN("=", HS_BOOL, Ch(0x1234), Ch(0x1234)), NULL));
	s_eq_b = add_script("a_ne_b", HS_S_STATIC, HS_BOOL, BEGIN(HS_BOOL, FN("!=", HS_BOOL, Cb(1), Cb(0)), NULL));
	s_neg = add_script("a_neg", HS_S_STATIC, HS_REAL, BEGIN(HS_REAL, FN("-", HS_REAL, Cr(4.0f), NULL), NULL));
	s_sub = add_script("a_sub", HS_S_STATIC, HS_LONG, BEGIN(HS_LONG, FN("-", HS_LONG, Cr(10.0f), Cr(3.0f), Cr(2.0f), NULL), NULL));
	world_open(w);
	v = hs_call_script(&w->m, (unsigned)s_long);  CHECK(v.type == HS_LONG && v.v.i == 9000);
	v = hs_call_script(&w->m, (unsigned)s_real);  CHECK(v.type == HS_REAL && v.v.f == 3.75f);
	v = hs_call_script(&w->m, (unsigned)s_short); CHECK(v.type == HS_SHORT && v.v.i == 3);
	v = hs_call_script(&w->m, (unsigned)s_wrap);  CHECK(v.type == HS_SHORT && v.v.i == -32767);
	v = hs_call_script(&w->m, (unsigned)s_div);   CHECK(v.type == HS_REAL && v.v.f == 3.5f);
	v = hs_call_script(&w->m, (unsigned)s_div0);  CHECK(v.type == HS_REAL && v.v.f == 0.0f);
	v = hs_call_script(&w->m, (unsigned)s_minmax); CHECK(v.type == HS_REAL && v.v.f == 1.0f);
	v = hs_call_script(&w->m, (unsigned)s_cmp_i); CHECK(v.type == HS_BOOL && v.v.i == 1);
	v = hs_call_script(&w->m, (unsigned)s_cmp_r); CHECK(v.type == HS_BOOL && v.v.i == 1);
	v = hs_call_script(&w->m, (unsigned)s_eq_h);  CHECK(v.type == HS_BOOL && v.v.i == 1);
	v = hs_call_script(&w->m, (unsigned)s_eq_b);  CHECK(v.type == HS_BOOL && v.v.i == 1);
	v = hs_call_script(&w->m, (unsigned)s_neg);   CHECK(v.type == HS_REAL && v.v.f == -4.0f);
	v = hs_call_script(&w->m, (unsigned)s_sub);   CHECK(v.type == HS_LONG && v.v.i == 5);
	world_free(w);
}

static void test_logic_and_flow(void)
{
	world *w = world_new();
	int s_and, s_or, s_andall, s_orall, s_not, s_if1, s_if2, s_if3, s_cond, s_cond_none, s_begin_val, s_empty;
	hs_value v;
	reset_builder();
	s_and = add_script("l_and", HS_S_STATIC, HS_BOOL, BEGIN(HS_BOOL, FN("and", HS_BOOL, FN("=", HS_BOOL, Cs(1), Cs(2)), FN("=", HS_BOOL, FN0("cnt", HS_LONG), Cl(0)), NULL), NULL));
	s_or = add_script("l_or", HS_S_STATIC, HS_BOOL, BEGIN(HS_BOOL, FN("or", HS_BOOL, FN("=", HS_BOOL, Cs(1), Cs(1)), FN("=", HS_BOOL, FN0("cnt", HS_LONG), Cl(0)), NULL), NULL));
	s_andall = add_script("l_andall", HS_S_STATIC, HS_BOOL, BEGIN(HS_BOOL, FN("and", HS_BOOL, Cb(1), Cb(1), Cb(1), Cb(1), NULL), NULL));
	s_orall = add_script("l_orall", HS_S_STATIC, HS_BOOL, BEGIN(HS_BOOL, FN("or", HS_BOOL, Cb(0), Cb(0), Cb(1), NULL), NULL));
	s_not = add_script("l_not", HS_S_STATIC, HS_BOOL, BEGIN(HS_BOOL, FN("not", HS_BOOL, Cb(0), NULL), NULL));
	s_if1 = add_script("f_if1", HS_S_STATIC, HS_SHORT, BEGIN(HS_SHORT, FN("if", HS_SHORT, Cb(1), Cs(11), Cs(22), NULL), NULL));
	s_if2 = add_script("f_if2", HS_S_STATIC, HS_SHORT, BEGIN(HS_SHORT, FN("if", HS_SHORT, Cb(0), Cs(11), Cs(22), NULL), NULL));
	s_if3 = add_script("f_if3", HS_S_STATIC, HS_VOID, BEGIN(HS_VOID, FN("if", HS_VOID, Cb(0), FN("mark", HS_VOID, Cs(9), NULL), NULL), NULL));
	s_cond = add_script("f_cond", HS_S_STATIC, HS_SHORT,
	    BEGIN(HS_SHORT, FN("cond", HS_SHORT, Cb(0), Cs(1), Cb(0), Cs(2), Cb(1), Cs(3), Cb(1), Cs(4), NULL), NULL));
	s_cond_none = add_script("f_cond0", HS_S_STATIC, HS_VOID, BEGIN(HS_VOID, FN("cond", HS_VOID, Cb(0), FN("mark", HS_VOID, Cs(5), NULL), NULL), NULL));
	s_begin_val = add_script("f_begin", HS_S_STATIC, HS_SHORT, BEGIN(HS_SHORT, FN("mark", HS_VOID, Cs(1), NULL), FN("mark", HS_VOID, Cs(2), NULL), Cs(77), NULL));
	s_empty = add_script("f_empty", HS_S_STATIC, HS_VOID, BEGIN(HS_VOID, NULL));
	world_open(w);
	v = hs_call_script(&w->m, (unsigned)s_and);    CHECK(v.v.i == 0 && w->host.calls == 0);          /* short circuit */
	v = hs_call_script(&w->m, (unsigned)s_or);     CHECK(v.v.i == 1 && w->host.calls == 0);
	v = hs_call_script(&w->m, (unsigned)s_andall); CHECK(v.v.i == 1);
	v = hs_call_script(&w->m, (unsigned)s_orall);  CHECK(v.v.i == 1);
	v = hs_call_script(&w->m, (unsigned)s_not);    CHECK(v.v.i == 1);
	v = hs_call_script(&w->m, (unsigned)s_if1);    CHECK(v.type == HS_SHORT && v.v.i == 11);
	v = hs_call_script(&w->m, (unsigned)s_if2);    CHECK(v.v.i == 22);
	v = hs_call_script(&w->m, (unsigned)s_if3);    CHECK(v.type == HS_VOID && w->host.nseq == 0);
	v = hs_call_script(&w->m, (unsigned)s_cond);   CHECK(v.v.i == 3);
	v = hs_call_script(&w->m, (unsigned)s_cond_none); CHECK(v.type == HS_VOID && w->host.nseq == 0);
	v = hs_call_script(&w->m, (unsigned)s_begin_val); CHECK(v.v.i == 77 && w->host.nseq == 2 && w->host.seq[0] == 1 && w->host.seq[1] == 2);
	v = hs_call_script(&w->m, (unsigned)s_empty);  CHECK(v.type == HS_VOID);
	world_free(w);
}

static void test_set_globals_and_init(void)
{
	world *w = world_new();
	int g0, g1, g2, g3, g4, gs, gh, ge, s_set;
	hs_value v;
	reset_builder();
	g0 = add_global("g0", HS_LONG, Cl(5), 0);
	g1 = add_global("g1", HS_LONG, FN("+", HS_LONG, G(0, HS_REAL), Cr(1.0f), NULL), 0);   /* reads g0 as real (promotion) */
	g2 = add_global("g2", HS_BOOL, FN(">", HS_BOOL, G(1, HS_LONG), Cl(5), NULL), 0);
	g3 = add_global("g3", HS_SHORT, NULL, 0);
	g4 = add_global("g4", HS_REAL, G(0, HS_REAL), 0);
	gs = add_global("gs", HS_STRING, Cstr("hello"), 0);
	gh = add_global("gh", HS_HANDLE, NULL, 0);
	ge = add_global("ext_var", HS_REAL, NULL, 1);
	s_set = add_script("s_set", HS_S_STATIC, HS_VOID,
	    BEGIN(HS_VOID, SET(g3, Cr(3.9f)), SET(ge, Cr(0.5f)), SET(gh, Ch(0xABCDu)), SET(g0, FN("*", HS_LONG, Cr(2.0f), Cr(70000.0f), NULL)), NULL));
	world_open(w);
	CHECK(gi(w, g0) == 5 && gi(w, g1) == 6 && gi(w, g2) == 1);                      /* initialisers in order */
	CHECK(hs_global_get(&w->m, (unsigned)g1).type == HS_LONG && hs_global_get(&w->m, (unsigned)g4).type == HS_REAL);
	CHECK(gf(w, g4) == 5.0f);
	CHECK(strcmp(hs_string(&w->sec, hs_global_get(&w->m, (unsigned)gs).v.i), "hello") == 0);
	CHECK(hs_global_get(&w->m, (unsigned)gh).v.h == HS_HANDLE_NONE);               /* neutral handle */
	CHECK(gi(w, g3) == 0);
	CHECK(hs_find_global(&w->sec, "ext_var") == ge && w->sec.external_count == 1);
	v = hs_call_script(&w->m, (unsigned)s_set);
	CHECK(v.type == HS_VOID);
	CHECK(gi(w, g3) == 3);                                                           /* real 3.9 stored in a short */
	CHECK(gf(w, ge) == 0.5f);
	CHECK(hs_global_get(&w->m, (unsigned)gh).v.h == 0xABCDu);
	CHECK(gi(w, g0) == 140000);
	CHECK(hs_global_set(&w->m, (unsigned)g3, hs_make_real(-7.5f)) == 0 && gi(w, g3) == -7);
	CHECK(hs_global_set(&w->m, 999, hs_make_int(1)) < 0);
	world_free(w);
}

static void test_script_calls(void)
{
	static const uint8_t two_shorts[2] = { HS_SHORT, HS_SHORT };
	world *w = world_new();
	int s_add2, s_use, s_nested, s_inner, s_outer, s_cd, s_cdrun, s_unit, g_n;
	hs_value v;
	reset_builder();
	g_n = add_global("n", HS_LONG, NULL, 0);
	s_add2 = add_script_p("add2", HS_SHORT, 2, two_shorts, BEGIN(HS_SHORT, FN("+", HS_SHORT, P(0, HS_REAL), P(1, HS_REAL), NULL), NULL));
	s_use = add_script("use_add2", HS_S_STATIC, HS_SHORT, BEGIN(HS_SHORT, SC(s_add2, HS_SHORT, Cs(3), Cs(4), NULL), NULL));
	s_nested = add_script("nested_add2", HS_S_STATIC, HS_SHORT,
	    BEGIN(HS_SHORT, SC(s_add2, HS_SHORT, SC(s_add2, HS_SHORT, Cs(1), Cs(2), NULL), SC(s_add2, HS_SHORT, Cs(3), Cs(4), NULL), NULL), NULL));
	s_inner = add_script("inner", HS_S_STATIC, HS_SHORT, BEGIN(HS_SHORT, Cs(5), NULL));
	s_outer = add_script("outer", HS_S_STATIC, HS_SHORT, BEGIN(HS_SHORT, FN("+", HS_SHORT, SC(s_inner, HS_REAL, NULL), SC(s_inner, HS_REAL, NULL), NULL), NULL));
	/* countdown recursion: n++ ; if n < 5 call itself (5 levels deep) */
	s_cd = nbs;
	add_script("countdown", HS_S_STATIC, HS_VOID,
	    BEGIN(HS_VOID, SET(g_n, FN("+", HS_LONG, G(g_n, HS_REAL), Cr(1.0f), NULL)),
	          FN("if", HS_VOID, FN("<", HS_BOOL, G(g_n, HS_LONG), Cl(5), NULL), SC(s_cd, HS_VOID, NULL), NULL), NULL));
	s_cdrun = add_script("cd_start", HS_S_STARTUP, HS_VOID, BEGIN(HS_VOID, SC(s_cd, HS_VOID, NULL), NULL));
	s_unit = add_script("unit_ret", HS_S_STATIC, HS_HANDLE, BEGIN(HS_HANDLE, Ch(0x77), NULL));
	(void)s_unit;
	world_open(w);
	v = hs_call_script(&w->m, (unsigned)s_use);    CHECK(v.type == HS_SHORT && v.v.i == 7);
	v = hs_call_script(&w->m, (unsigned)s_nested); CHECK(v.v.i == 10);
	v = hs_call_script(&w->m, (unsigned)s_outer);  CHECK(v.v.i == 10);
	ticks(w, 1);
	CHECK(gi(w, g_n) == 5 && S(w)->errors == 0 && hs_thread_count(&w->m) == 0);
	CHECK(S(w)->hw_calls >= 5);
	(void)s_cdrun;
	world_free(w);
}

static void test_recursion_limits(void)
{
	world *w = world_new();
	int s_rec, s_start, s_x;
	reset_builder();
	s_rec = nbs;
	add_script("rec", HS_S_STATIC, HS_VOID, BEGIN(HS_VOID, SC(s_rec, HS_VOID, NULL), NULL));
	s_start = add_script("rec_start", HS_S_STARTUP, HS_VOID, BEGIN(HS_VOID, SC(s_rec, HS_VOID, NULL), NULL));
	s_x = add_script("other", HS_S_STARTUP, HS_VOID, BEGIN(HS_VOID, SLEEP(2), FN("mark", HS_VOID, Cs(1), NULL), NULL));
	(void)s_start; (void)s_x;
	world_open(w);
	ticks(w, 1);
	CHECK(S(w)->errors == 1 && (S(w)->last_error == 1 || S(w)->last_error == 3));      /* frames or depth */
	CHECK(S(w)->last_error_script == (uint32_t)s_start);
	CHECK(!hs_script_running(&w->m, (unsigned)s_start));                               /* killed, the others go on */
	ticks(w, 3);
	CHECK(w->host.nseq == 1 && S(w)->errors == 1);
	world_free(w);
}

static void test_sleep_semantics(void)
{
	world *w = world_new();
	int g0;
	reset_builder();
	g0 = add_global("g0", HS_SHORT, NULL, 0);
	add_script("s0", HS_S_STARTUP, HS_VOID, BEGIN(HS_VOID, SET(g0, Cs(1)), SLEEP(3), SET(g0, Cs(2)), SLEEP(2), SET(g0, Cs(3)), NULL));
	world_open(w);
	CHECK(hs_thread_count(&w->m) == 1 && gi(w, g0) == 0);                    /* init does not run the script */
	ticks(w, 1); CHECK(gi(w, g0) == 1 && hs_thread_count(&w->m) == 1);       /* tick 0 */
	ticks(w, 1); CHECK(gi(w, g0) == 1);                                      /* 1 */
	ticks(w, 1); CHECK(gi(w, g0) == 1);                                      /* 2 */
	ticks(w, 1); CHECK(gi(w, g0) == 2);                                      /* 3: slept 3 ticks */
	ticks(w, 1); CHECK(gi(w, g0) == 2);                                      /* 4 */
	ticks(w, 1); CHECK(gi(w, g0) == 3 && hs_thread_count(&w->m) == 0);       /* 5: finished, thread freed */
	CHECK(S(w)->tick == 6);
	world_free(w);
}

static void test_sleep_until(void)
{
	world *w = world_new();
	int ra, rb, rc, rd, re, t;
	reset_builder();
	ra = add_global("ra", HS_LONG, NULL, 0); rb = add_global("rb", HS_LONG, NULL, 0); rc = add_global("rc", HS_LONG, NULL, 0);
	rd = add_global("rd", HS_LONG, NULL, 0); re = add_global("re", HS_LONG, NULL, 0);
	add_script("su_a", HS_S_STARTUP, HS_VOID, BEGIN(HS_VOID, FN("sleep_until", HS_VOID, FN0("flag_a", HS_BOOL), Cs(1), NULL), SET(ra, FN0("tickstamp", HS_LONG)), NULL));
	add_script("su_b", HS_S_STARTUP, HS_VOID, BEGIN(HS_VOID, FN("sleep_until", HS_VOID, FN0("flag_a", HS_BOOL), Cs(5), NULL), SET(rb, FN0("tickstamp", HS_LONG)), NULL));
	add_script("su_c", HS_S_STARTUP, HS_VOID, BEGIN(HS_VOID, FN("sleep_until", HS_VOID, FN0("flag_b", HS_BOOL), Cs(5), NULL), SET(rc, FN0("tickstamp", HS_LONG)), NULL));
	add_script("su_d", HS_S_STARTUP, HS_VOID, BEGIN(HS_VOID, FN("sleep_until", HS_VOID, FN0("flag_a", HS_BOOL), NULL), SET(rd, FN0("tickstamp", HS_LONG)), NULL));
	add_script("su_e", HS_S_STARTUP, HS_VOID, BEGIN(HS_VOID, FN("sleep_until", HS_VOID, Cb(0), Cs(5), Cl(12), NULL), SET(re, FN0("tickstamp", HS_LONG)), NULL));
	world_open(w);
	for (t = 0; t < 40; t++) {
		if (t == 4) w->host.flag_a = 1;
		if (t == 7) w->host.flag_b = 1;
		ticks(w, 1);
	}
	CHECK(gi(w, ra) == 4);        /* polls every tick: sees the flag the tick it is set */
	CHECK(gi(w, rb) == 5);        /* polls at 0, 5 */
	CHECK(gi(w, rc) == 10);       /* flag_b set before tick 7: polls at 5, 10 */
	CHECK(gi(w, rd) == 30);       /* default period: once a second */
	CHECK(gi(w, re) == 12);       /* timeout */
	CHECK(hs_thread_count(&w->m) == 0);
	world_free(w);

	/* the default period is configurable */
	w = world_new();
	reset_builder();
	rd = add_global("rd", HS_LONG, NULL, 0);
	add_script("su_d", HS_S_STARTUP, HS_VOID, BEGIN(HS_VOID, FN("sleep_until", HS_VOID, FN0("flag_a", HS_BOOL), NULL), SET(rd, FN0("tickstamp", HS_LONG)), NULL));
	w->blob = finish(&w->size);
	CHECK(hs_open(&w->sec, w->blob, w->size) == HS_OK);
	hs_setup(&w->m); reg_host(&w->m); hs_set_poll_ticks(&w->m, 3);
	CHECK(hs_init(&w->m, &w->sec, &w->host) == HS_OK);
	for (t = 0; t < 10; t++) { if (t == 4) w->host.flag_a = 1; ticks(w, 1); }
	CHECK(gi(w, rd) == 6);        /* polls at 0, 3, 6 */
	world_free(w);
}

static void test_wake_and_threads(void)
{
	world *w = world_new();
	int g_d, g_e, g_f, g_c, d, e, f_, cont, a_, b_, tgt;
	hs_value v;
	reset_builder();
	g_d = add_global("g_d", HS_LONG, NULL, 0); g_e = add_global("g_e", HS_LONG, NULL, 0);
	g_f = add_global("g_f", HS_LONG, NULL, 0); g_c = add_global("g_c", HS_LONG, NULL, 0);
	/* script indices are assigned in declaration order: a_ 0, b_ 1, d 2, e 3, f_ 4, cont 5, tgt 6 */
	a_ = 0; b_ = 1; d = 2; e = 3; f_ = 4; cont = 5; tgt = 6; (void)cont;
	add_script("a_quick", HS_S_STARTUP, HS_VOID, BEGIN(HS_VOID, FN("mark", HS_VOID, Cs(100), NULL), NULL));          /* frees thread 0 at tick 0 */
	add_script("b_waker", HS_S_STARTUP, HS_VOID,
	    BEGIN(HS_VOID, SLEEP(2), FN("wake", HS_VOID, Cscr(d), NULL), SLEEP(4), FN("wake", HS_VOID, Cscr(e), NULL),
	          SLEEP(1), FN("wake", HS_VOID, Cscr(f_), NULL), SLEEP(1), FN("wake", HS_VOID, Cscr(f_), NULL), NULL));
	add_script("d_dormant", HS_S_DORMANT, HS_VOID, BEGIN(HS_VOID, SET(g_d, FN0("tickstamp", HS_LONG)), NULL));
	add_script("e_dormant", HS_S_DORMANT, HS_VOID,
	    BEGIN(HS_VOID, FN("sleep_forever", HS_VOID, NULL), SET(g_e, FN0("tickstamp", HS_LONG)), NULL));
	add_script("f_dormant", HS_S_DORMANT, HS_VOID, BEGIN(HS_VOID, SET(g_f, FN("+", HS_LONG, G(g_f, HS_REAL), Cr(1.0f), NULL)), NULL));
	add_script("cont", HS_S_CONTINUOUS, HS_VOID,
	    BEGIN(HS_VOID, SET(g_c, FN("+", HS_LONG, G(g_c, HS_REAL), Cr(1.0f), NULL)),
	          FN("if", HS_VOID, FN(">=", HS_BOOL, G(g_c, HS_LONG), Cl(5), NULL), FN("sleep", HS_VOID, Cs(-1), NULL), NULL), NULL));
	add_script("tgt_static", HS_S_STATIC, HS_VOID, BEGIN(HS_VOID, NULL));
	world_open(w);
	CHECK(hs_thread_count(&w->m) == 3);                                    /* a_quick, b_waker, cont */
	CHECK(!hs_script_running(&w->m, (unsigned)d) && !hs_script_running(&w->m, (unsigned)e));
	ticks(w, 1);                                                           /* tick 0 */
	CHECK(hs_thread_count(&w->m) == 2 && w->host.nseq == 1 && gi(w, g_c) == 1);   /* a_quick finished, freeing thread 0 */
	CHECK(gi(w, g_d) == 0);
	ticks(w, 2);                                                           /* ticks 1, 2: b wakes d at tick 2 */
	/* d takes the lowest free slot (0), which the pass already left behind this tick: runs next tick */
	CHECK(gi(w, g_d) == 0 && hs_script_running(&w->m, (unsigned)d));
	ticks(w, 1);                                                           /* tick 3 */
	CHECK(gi(w, g_d) == 3 && !hs_script_running(&w->m, (unsigned)d));
	CHECK(gi(w, g_c) == 4);
	ticks(w, 3);                                                           /* ticks 4..6: continuous stops at 5, b wakes e at tick 6 */
	CHECK(gi(w, g_c) == 5);                                                /* asleep forever from tick 4 on */
	CHECK(hs_script_running(&w->m, (unsigned)e) && gi(w, g_e) == 0);       /* e started (slot 0, behind the pass) */
	ticks(w, 1);                                                           /* tick 7: e sleeps forever; f is woken into a later slot */
	CHECK(gi(w, g_e) == 0 && hs_script_running(&w->m, (unsigned)e) && gi(w, g_f) == 1);
	ticks(w, 1);                                                           /* tick 8: f woken again, runs again; b is done */
	CHECK(gi(w, g_f) == 2 && !hs_script_running(&w->m, (unsigned)b_));
	CHECK(hs_wake_script(&w->m, "e_dormant") == 0);                        /* cuts sleep_forever short */
	ticks(w, 1);                                                           /* tick 9 */
	CHECK(gi(w, g_e) == 9 && !hs_script_running(&w->m, (unsigned)e));
	CHECK(hs_wake_script(&w->m, "cont") == 0);                             /* the continuous script resumes after its sleep */
	ticks(w, 1);                                                           /* tick 10: finishes the pass, re-armed for tick 11 */
	CHECK(gi(w, g_c) == 5);
	ticks(w, 1);
	CHECK(gi(w, g_c) == 6);
	CHECK(hs_wake_script_index(&w->m, (unsigned)tgt) < 0);                 /* static: no thread */
	CHECK(hs_wake_script(&w->m, "nosuch") < 0);
	CHECK(hs_wake_script_index(&w->m, 9999) < 0);
	(void)a_; (void)b_; (void)v;
	world_free(w);
}

static void test_wake_order_and_rerun(void)
{
	world *w = world_new();
	int g_d, g_f, i;
	reset_builder();
	g_d = add_global("g_d", HS_LONG, NULL, 0);
	g_f = add_global("g_f", HS_LONG, NULL, 0);
	/* waker (index 0) wakes d (index 1) at tick 2: d takes slot 1 > the waker's slot 0 and runs in the same tick */
	add_script("waker", HS_S_STARTUP, HS_VOID, BEGIN(HS_VOID, SLEEP(2), FN("wake", HS_VOID, Cscr(1), NULL), SLEEP(3), FN("wake", HS_VOID, Cscr(2), NULL),
	                                                 SLEEP(1), FN("wake", HS_VOID, Cscr(2), NULL), NULL));
	add_script("d", HS_S_DORMANT, HS_VOID, BEGIN(HS_VOID, SET(g_d, FN0("tickstamp", HS_LONG)), NULL));
	add_script("f", HS_S_DORMANT, HS_VOID, BEGIN(HS_VOID, SET(g_f, FN("+", HS_LONG, G(g_f, HS_REAL), Cr(1.0f), NULL)), NULL));
	world_open(w);
	for (i = 0; i < 8; i++) {
		ticks(w, 1);
		if (i == 2) CHECK(gi(w, g_d) == 2);       /* same tick */
	}
	CHECK(gi(w, g_f) == 2);                        /* a finished dormant script runs again when woken again */
	world_free(w);
}

static void test_sleep_other_script(void)
{
	world *w = world_new();
	int g_x;
	reset_builder();
	g_x = add_global("g_x", HS_LONG, NULL, 0);
	/* 0: worker counts every tick; 1: controller puts the worker to sleep forever at tick 5 and wakes it at tick 9 */
	add_script("worker", HS_S_CONTINUOUS, HS_VOID, BEGIN(HS_VOID, SET(g_x, FN("+", HS_LONG, G(g_x, HS_REAL), Cr(1.0f), NULL)), NULL));
	add_script("ctl", HS_S_STARTUP, HS_VOID, BEGIN(HS_VOID, SLEEP(5), FN("sleep", HS_VOID, Cs(-1), Cscr(0), NULL), SLEEP(4), FN("wake", HS_VOID, Cscr(0), NULL), NULL));
	world_open(w);
	ticks(w, 6);                                   /* ticks 0..5: the worker ran every tick (slot 0 runs before the controller) */
	CHECK(gi(w, g_x) == 6);
	ticks(w, 3);                                   /* ticks 6..8: asleep */
	CHECK(gi(w, g_x) == 6);
	ticks(w, 1);                                   /* tick 9: woken, but its slot was already passed in this pass */
	CHECK(gi(w, g_x) == 6);
	ticks(w, 1);                                   /* tick 10 */
	CHECK(gi(w, g_x) == 7);
	ticks(w, 1);
	CHECK(gi(w, g_x) == 8);
	world_free(w);
}

static void test_static_sleep_and_block(void)
{
	world *w = world_new();
	int g_r, g_done, g_b, s_sl;
	reset_builder();
	g_r = add_global("r", HS_SHORT, NULL, 0); g_done = add_global("done", HS_LONG, NULL, 0); g_b = add_global("b", HS_LONG, NULL, 0);
	s_sl = add_script("sleeper", HS_S_STATIC, HS_SHORT, BEGIN(HS_SHORT, SLEEP(3), Cs(42), NULL));
	add_script("caller", HS_S_STARTUP, HS_VOID, BEGIN(HS_VOID, SET(g_r, SC(s_sl, HS_SHORT, NULL)), SET(g_done, FN0("tickstamp", HS_LONG)), NULL));
	add_script("blocked", HS_S_STARTUP, HS_VOID, BEGIN(HS_VOID, FN0("blocker", HS_VOID), SET(g_b, FN0("tickstamp", HS_LONG)), NULL));
	world_open(w);
	w->host.block_left = 3;
	ticks(w, 3);
	CHECK(gi(w, g_r) == 0 && gi(w, g_done) == 0 && gi(w, g_b) == 0);         /* caller sleeps with the static script */
	ticks(w, 1);                                                             /* tick 3 */
	CHECK(gi(w, g_r) == 42 && gi(w, g_done) == 3 && gi(w, g_b) == 3);        /* blocked at ticks 0, 1, 2, passes at 3 */
	CHECK(S(w)->blocked_calls == 3);
	world_free(w);
}

static void test_thread_cap_and_budgets(void)
{
	world *w = world_new();
	int i, g_n, base;
	char *names[HS_MAX_THREADS + 8];
	reset_builder();
	g_n = add_global("n", HS_LONG, NULL, 0);
	for (i = 0; i < (int)HS_MAX_THREADS + 2; i++) {
		names[i] = malloc(16);
		sprintf(names[i], "d%d", i);
		add_script(names[i], HS_S_DORMANT, HS_VOID, BEGIN(HS_VOID, FN("sleep_forever", HS_VOID, NULL), NULL));
	}
	base = nbs;
	world_open(w);
	CHECK(hs_thread_count(&w->m) == 0);
	for (i = 0; i < (int)HS_MAX_THREADS + 2; i++) {
		int rc = hs_wake_script_index(&w->m, (unsigned)i);
		CHECK((i < (int)HS_MAX_THREADS) == (rc == 0));
	}
	CHECK(hs_thread_count(&w->m) == HS_MAX_THREADS && S(w)->thread_overflow == 2 && S(w)->hw_threads == HS_MAX_THREADS);
	ticks(w, 3);
	CHECK(hs_thread_count(&w->m) == HS_MAX_THREADS);                          /* all asleep forever */
	world_free(w);
	for (i = 0; i < (int)HS_MAX_THREADS + 2; i++) free(names[i]);
	(void)g_n; (void)base;

	/* per-thread node budget: a 300 statement script finishes over several ticks, in order */
	w = world_new();
	reset_builder();
	{
		tn *b = BEGIN(HS_VOID, NULL);
		for (i = 0; i < 300; i++) b->kid[b->nk++] = FN("mark", HS_VOID, Cs(i), NULL);
		add_script("long", HS_S_STARTUP, HS_VOID, b);
	}
	w->blob = finish(&w->size);
	CHECK(hs_open(&w->sec, w->blob, w->size) == HS_OK);
	hs_setup(&w->m); reg_host(&w->m); hs_set_budgets(&w->m, 100, 100000);
	CHECK(hs_init(&w->m, &w->sec, &w->host) == HS_OK);
	for (i = 0; i < 20 && hs_thread_count(&w->m); i++) ticks(w, 1);
	CHECK(hs_thread_count(&w->m) == 0 && i >= 6 && i <= 10 && S(w)->budget_hits >= 5);
	CHECK(w->host.nseq == 300);
	for (i = 0; i < 300; i++) CHECK(w->host.seq[i] == i);
	world_free(w);

	/* per-tick budget: 40 threads of 10 marks each finish eventually, nobody starves, every mark runs once */
	w = world_new();
	reset_builder();
	for (i = 0; i < 40; i++) {
		tn *b = BEGIN(HS_VOID, NULL);
		int k;
		for (k = 0; k < 10; k++) b->kid[b->nk++] = FN("mark", HS_VOID, Cs(i), NULL);
		add_script("t", HS_S_STARTUP, HS_VOID, b);
	}
	w->blob = finish(&w->size);
	CHECK(hs_open(&w->sec, w->blob, w->size) == HS_OK);
	hs_setup(&w->m); reg_host(&w->m); hs_set_budgets(&w->m, 1000, 150);
	CHECK(hs_init(&w->m, &w->sec, &w->host) == HS_OK);
	for (i = 0; i < 40 && hs_thread_count(&w->m); i++) ticks(w, 1);
	CHECK(hs_thread_count(&w->m) == 0 && i > 1 && S(w)->tick_budget_hits > 0 && w->host.nseq == 400);
	{
		int count[40] = { 0 }, k;
		for (k = 0; k < 400; k++) count[w->host.seq[k]]++;
		for (k = 0; k < 40; k++) CHECK(count[k] == 10);
	}
	world_free(w);
}

static void test_unregistered(void)
{
	world *w = world_new();
	int s_b, s_r, s_h, s_l, s_s, s_v, g_h;
	hs_value v;
	const char *name = NULL;
	uint32_t calls = 0;
	reset_builder();
	g_h = add_global("gh", HS_HANDLE, NULL, 0);
	s_b = add_script("u_b", HS_S_STATIC, HS_BOOL, BEGIN(HS_BOOL, FN("unk_bool", HS_BOOL, Cs(1), NULL), NULL));
	s_r = add_script("u_r", HS_S_STATIC, HS_REAL, BEGIN(HS_REAL, FN("unk_real", HS_REAL, NULL), NULL));
	s_h = add_script("u_h", HS_S_STATIC, HS_HANDLE, BEGIN(HS_HANDLE, FN("unk_handle", HS_HANDLE, Ch(5), NULL), NULL));
	s_l = add_script("u_l", HS_S_STATIC, HS_LONG, BEGIN(HS_LONG, FN("unk_long", HS_LONG, NULL), NULL));
	s_s = add_script("u_s", HS_S_STATIC, HS_STRING, BEGIN(HS_STRING, FN("unk_string", HS_STRING, NULL), NULL));
	s_v = add_script("u_v", HS_S_STATIC, HS_VOID, BEGIN(HS_VOID, FN("unk_void", HS_VOID, FN("unk_bool", HS_BOOL, NULL), NULL), SET(g_h, FN("unk_handle", HS_HANDLE, NULL)), NULL));
	world_open(w);
	v = hs_call_script(&w->m, (unsigned)s_b); CHECK(v.type == HS_BOOL && v.v.i == 0);
	v = hs_call_script(&w->m, (unsigned)s_r); CHECK(v.type == HS_REAL && v.v.f == 0.0f);
	v = hs_call_script(&w->m, (unsigned)s_h); CHECK(v.type == HS_HANDLE && v.v.h == HS_HANDLE_NONE);
	v = hs_call_script(&w->m, (unsigned)s_l); CHECK(v.type == HS_LONG && v.v.i == 0);
	v = hs_call_script(&w->m, (unsigned)s_s); CHECK(v.type == HS_STRING && strcmp(hs_string(&w->sec, v.v.i), "") == 0);
	v = hs_call_script(&w->m, (unsigned)s_v); CHECK(v.type == HS_VOID);
	CHECK(S(w)->unreg_total == 8);
	CHECK(hs_unregistered(&w->m, 0, &name, &calls) == 6);
	{
		unsigned k;
		uint32_t total = 0;
		for (k = 0; k < 6; k++) { hs_unregistered(&w->m, k, &name, &calls); CHECK(name && calls); total += calls; }
		CHECK(total == 8);
	}
	/* registering afterwards binds it */
	{
		hs_register(&w->m, "unk_long", h_cnt);
		v = hs_call_script(&w->m, (unsigned)s_l); CHECK(v.v.i == 1);
	}
	world_free(w);
}

static void test_begin_random_and_random(void)
{
	world *w = world_new();
	int s_br, s_rr, s_rf, seed, i, k, distinct = 0;
	int seen[256] = { 0 };
	reset_builder();
	s_br = add_script("br", HS_S_STATIC, HS_VOID,
	    BEGIN(HS_VOID, FN("begin_random", HS_VOID, FN("mark", HS_VOID, Cs(0), NULL), FN("mark", HS_VOID, Cs(1), NULL),
	                      FN("mark", HS_VOID, Cs(2), NULL), FN("mark", HS_VOID, Cs(3), NULL), NULL), NULL));
	s_rr = add_script("rr", HS_S_STATIC, HS_SHORT, BEGIN(HS_SHORT, FN("random_range", HS_SHORT, Cs(5), Cs(10), NULL), NULL));
	s_rf = add_script("rf", HS_S_STATIC, HS_REAL, BEGIN(HS_REAL, FN("real_random_range", HS_REAL, Cr(1.0f), Cr(2.0f), NULL), NULL));
	world_open(w);
	for (seed = 1; seed <= 300; seed++) {
		int perm = 0, used = 0;
		hs_seed(&w->m, (uint32_t)seed * 2654435761u);
		w->host.nseq = 0;
		hs_call_script(&w->m, (unsigned)s_br);
		CHECK(w->host.nseq == 4);
		for (i = 0; i < 4; i++) { CHECK(w->host.seq[i] >= 0 && w->host.seq[i] < 4 && !(used & (1 << w->host.seq[i]))); used |= 1 << w->host.seq[i]; }
		CHECK(used == 15);
		for (i = 0; i < 4; i++) perm = perm * 4 + w->host.seq[i];
		seen[perm] = 1;
	}
	for (k = 0; k < 256; k++) distinct += seen[k];
	CHECK(distinct >= 12);                                    /* the order varies */
	{
		int hist[5] = { 0 };
		for (i = 0; i < 2000; i++) {
			hs_value v = hs_call_script(&w->m, (unsigned)s_rr);
			CHECK(v.type == HS_SHORT && v.v.i >= 5 && v.v.i < 10);
			hist[v.v.i - 5]++;
		}
		for (i = 0; i < 5; i++) CHECK(hist[i] > 250);
		for (i = 0; i < 2000; i++) {
			hs_value v = hs_call_script(&w->m, (unsigned)s_rf);
			CHECK(v.type == HS_REAL && v.v.f >= 1.0f && v.v.f < 2.0f);
		}
		hs_seed(&w->m, 99);
		{ hs_value a = hs_call_script(&w->m, (unsigned)s_rr), b; hs_seed(&w->m, 99); b = hs_call_script(&w->m, (unsigned)s_rr); CHECK(a.v.i == b.v.i); }
	}
	world_free(w);
}

static void test_print_inspect(void)
{
	world *w = world_new();
	int s;
	reset_builder();
	s = add_script("pi", HS_S_STATIC, HS_VOID,
	    BEGIN(HS_VOID, FN("print", HS_VOID, Cstr("hello world"), NULL), FN("inspect", HS_VOID, Cs(-42), NULL),
	          FN("inspect", HS_VOID, Cr(1.5f), NULL), FN("inspect", HS_VOID, Cb(1), NULL), FN("inspect", HS_VOID, Ch(0xBEEF), NULL),
	          FN("inspect", HS_VOID, Cl(70000), NULL), NULL));
	world_open(w);
	hs_call_script(&w->m, (unsigned)s);
	CHECK(w->host.nlog == 6);
	CHECK(strcmp(w->host.log[0], "hello world") == 0 && strcmp(w->host.log[1], "-42") == 0 && strcmp(w->host.log[2], "1.500") == 0);
	CHECK(strcmp(w->host.log[3], "true") == 0 && strcmp(w->host.log[4], "0x0000beef") == 0 && strcmp(w->host.log[5], "70000") == 0);
	world_free(w);
}

/* ---- the bigger world used by the snapshot and corruption tests ---- */
static int build_busy_world(int *g_out)
{
	int i, g0, g1, g2, g3;
	reset_builder();
	g0 = add_global("count", HS_LONG, NULL, 0);
	g1 = add_global("phase", HS_SHORT, NULL, 0);
	g2 = add_global("rnd", HS_LONG, NULL, 0);
	g3 = add_global("flagbit", HS_BOOL, Cb(0), 0);
	g_out[0] = g0; g_out[1] = g1; g_out[2] = g2; g_out[3] = g3;
	/* 0: static helper that sleeps */
	add_script("helper", HS_S_STATIC, HS_SHORT, BEGIN(HS_SHORT, SLEEP(7), FN("random_range", HS_SHORT, Cs(0), Cs(100), NULL), NULL));
	/* 1: startup, phases with different sleeps, calls the helper, random order block */
	add_script("main", HS_S_STARTUP, HS_VOID,
	    BEGIN(HS_VOID, SET(g1, Cs(1)), SLEEP(11), SET(g1, Cs(2)), SET(g2, SC(0, HS_LONG, NULL)), SET(g1, Cs(3)),
	          FN("begin_random", HS_VOID, FN("mark", HS_VOID, Cs(1), NULL), FN("mark", HS_VOID, Cs(2), NULL), FN("mark", HS_VOID, Cs(3), NULL), NULL),
	          SLEEP(13), FN("wake", HS_VOID, Cscr(3), NULL), SET(g1, Cs(4)), NULL));
	/* 2: continuous ticker with a polling wait */
	add_script("ticker", HS_S_CONTINUOUS, HS_VOID,
	    BEGIN(HS_VOID, SET(g0, FN("+", HS_LONG, G(g0, HS_REAL), Cr(1.0f), NULL)), FN("sleep_until", HS_VOID, FN(">", HS_BOOL, FN("random_range", HS_SHORT, Cs(0), Cs(10), NULL), Cs(6), NULL), Cs(3), Cl(20), NULL), NULL));
	/* 3: dormant: waits on the flag with the default period, flips it */
	add_script("late", HS_S_DORMANT, HS_VOID,
	    BEGIN(HS_VOID, FN("sleep_until", HS_VOID, FN(">", HS_BOOL, G(g0, HS_LONG), Cl(40), NULL), NULL), SET(g3, Cb(1)),
	          FN("mark", HS_VOID, Cs(9), NULL), NULL));
	/* 4: startup that sleeps forever, then is woken by main's wake? (never) */
	add_script("idle", HS_S_STARTUP, HS_VOID, BEGIN(HS_VOID, FN("sleep_forever", HS_VOID, NULL), FN("mark", HS_VOID, Cs(-1), NULL), NULL));
	/* 5: a few staggered startup scripts waking each other */
	for (i = 0; i < 6; i++)
		add_script("stag", HS_S_STARTUP, HS_VOID,
		    BEGIN(HS_VOID, SLEEP(3 + i * 5), SET(g0, FN("+", HS_LONG, G(g0, HS_REAL), Cr(10.0f), NULL)), SLEEP(40), FN("mark", HS_VOID, Cs(20 + i), NULL), NULL));
	return nbs;
}

static int states_equal(world *a, world *b)
{
	return memcmp(hs_state_ptr(&a->m), hs_state_ptr(&b->m), hs_state_size()) == 0;
}

static void test_snapshot(void)
{
	world *a = world_new(), *b = world_new();
	int g[4], i, n_before = 14, n_after = 150, n_threads;
	uint8_t *snap;
	hs_value gv[4];
	reset_builder();
	build_busy_world(g);
	world_open(a);
	hs_seed(&a->m, 12345);
	for (i = 0; i < n_before; i++) ticks(a, 1);
	/* threads sleep at different points: inside sleep_until, inside a static script, at plain statements */
	{
		unsigned t, deep = 0, polling = 0, wakes[HS_MAX_THREADS], nw = 0, d, k;
		for (t = 0; t < HS_MAX_THREADS; t++) {
			const hs_thread *th = &S(a)->threads[t];
			if (th->state != HS_T_ACTIVE) continue;
			deep += th->fsp >= 4;
			polling += th->f[th->fsp - 1u].op == HS_OP_SLEEP_UNTIL;
			for (k = 0; k < nw && wakes[k] != th->wake; k++) {}
			if (k == nw) wakes[nw++] = th->wake;
		}
		d = nw;
		CHECK(hs_thread_count(&a->m) >= 6 && deep >= 1 && polling >= 1 && d >= 4);
	}
	snap = malloc(hs_state_size());
	CHECK(snap && hs_state_size() <= HS_STATE_BYTES_MAX);
	n_threads = hs_thread_count(&a->m);
	memcpy(snap, hs_state_ptr(&a->m), hs_state_size());
	a->host.nseq = 0;
	ticks(a, n_after);
	for (i = 0; i < 4; i++) gv[i] = hs_global_get(&a->m, (unsigned)g[i]);
	CHECK(gv[0].v.i > 40);
	/* restore into a fresh machine rebound to the same section */
	b->blob = a->blob; b->size = a->size; b->sec = a->sec;
	hs_setup(&b->m);
	reg_host(&b->m);
	hs_set_log(&b->m, h_log);
	memcpy(hs_state_ptr(&b->m), snap, hs_state_size());
	CHECK(hs_rebind(&b->m, &b->sec, &b->host) == HS_OK);
	CHECK(memcmp(hs_state_ptr(&b->m), snap, hs_state_size()) == 0);
	ticks(b, n_after);
	for (i = 0; i < 4; i++) {
		hs_value v = hs_global_get(&b->m, (unsigned)g[i]);
		CHECK(v.type == gv[i].type && v.v.i == gv[i].v.i);
	}
	CHECK(hs_thread_count(&a->m) == hs_thread_count(&b->m));
	for (i = 0; i < (int)HS_MAX_THREADS; i++) {
		const hs_thread *x = &S(a)->threads[i], *y = &S(b)->threads[i];
		CHECK(x->state == y->state && x->wake == y->wake && x->script == y->script && x->fsp == y->fsp && x->vsp == y->vsp);
	}
	CHECK(states_equal(a, b));
	CHECK(a->host.nseq == b->host.nseq && memcmp(a->host.seq, b->host.seq, sizeof(int) * (size_t)a->host.nseq) == 0);
	CHECK(S(a)->tick == (uint32_t)(n_before + n_after) && S(b)->tick == S(a)->tick);
	printf("[hs] snapshot: state %zu bytes (limit %u), %u threads at the snapshot, identical after %d ticks\n",
	       hs_state_size(), HS_STATE_BYTES_MAX, (unsigned)n_threads, n_after);

	/* a snapshot of another section / a corrupt tag is refused */
	{
		hs_state *st = hs_state_ptr(&b->m);
		hs_machine *m = &b->m;
		memcpy(st, snap, hs_state_size());
		st->section_hash ^= 1;
		CHECK(hs_rebind(m, &b->sec, &b->host) == HS_ERR_STATE);
		hs_tick(m);                                       /* an unbound machine ignores ticks */
	}
	free(snap);
	free(a->blob);
	free(a); free(b);
}

static void walk_run(const uint8_t *img, size_t n, int nticks)
{
	hs_section sec;
	static hs_machine *m;
	host_t host;
	uint8_t *copy = malloc(n);
	CHECK(copy);
	memcpy(copy, img, n);
	if (hs_open(&sec, copy, n) == HS_OK) {
		if (!m) m = malloc(sizeof *m);
		CHECK(m);
		memset(&host, 0, sizeof host);
		hs_setup(m);
		reg_host(m);
		if (hs_init(m, &sec, &host) == HS_OK) {
			int t;
			for (t = 0; t < nticks; t++) hs_tick(m);
			(void)hs_run_expression(m, 0);
			(void)hs_call_script(m, 0);
		}
	}
	free(copy);
}

static void test_corrupt_sections(void)
{
	world *w = world_new();
	int g[4];
	size_t i;
	hs_section sec;
	uint8_t *copy;
	reset_builder();
	build_busy_world(g);
	w->blob = finish(&w->size);
	CHECK(hs_open(&sec, w->blob, w->size) == HS_OK);
	CHECK(hs_open(&sec, w->blob, 64) == HS_ERR_TRUNCATED && hs_open(&sec, NULL, 0) == HS_ERR_TRUNCATED);
	for (i = 0; i < w->size; i++) {                                       /* every truncation is refused */
		copy = malloc(i ? i : 1);
		memcpy(copy, w->blob, i);
		CHECK(hs_open(&sec, copy, i) != HS_OK);
		free(copy);
	}
	{
		uint8_t *tmp = malloc(w->size + 8);
		memcpy(tmp + 1, w->blob, w->size);
		CHECK(hs_open(&sec, tmp + 1, w->size) == HS_ERR_ALIGN);
		free(tmp);
	}
	{
		unsigned accepted = 0;
		for (i = 0; i < w->size; i++) {                                   /* flip every byte, several patterns */
			static const uint8_t xs[] = { 0xFF, 0x01, 0x80 };
			unsigned k;
			for (k = 0; k < sizeof xs; k++) {
				uint8_t *c = malloc(w->size);
				memcpy(c, w->blob, w->size);
				c[i] ^= xs[k];
				if (hs_open(&sec, c, w->size) == HS_OK) { accepted++; walk_run(c, w->size, 60); }
				free(c);
			}
		}
		printf("[hs] corrupt sections: %zu bytes x 3 patterns, %u accepted (and run safely)\n", w->size, accepted);
	}
	world_free(w);
}

static void test_corrupt_state(void)
{
	world *a = world_new();
	int g[4], i, k, accepted = 0, rejected = 0;
	uint8_t *snap, *c;
	size_t sz, hot;
	unsigned seed = 7;
	reset_builder();
	build_busy_world(g);
	world_open(a);
	for (i = 0; i < 25; i++) ticks(a, 1);
	sz = hs_state_size();
	snap = malloc(sz);
	memcpy(snap, hs_state_ptr(&a->m), sz);
	hot = (size_t)((uint8_t *)&S(a)->threads[8] - (uint8_t *)S(a));
	c = malloc(sz);
	for (k = 0; k < 6000; k++) {
		hs_machine *m = malloc(sizeof *m);
		host_t host;
		size_t pos;
		seed = seed * 1664525u + 1013904223u;
		pos = (seed >> 8) % hot;
		memcpy(c, snap, sz);
		c[pos] ^= (uint8_t)(1u << ((seed >> 3) & 7));
		if (k % 3 == 0) c[(pos + 5) % hot] ^= 0x5A;
		memset(&host, 0, sizeof host);
		hs_setup(m);
		reg_host(m);
		memcpy(hs_state_ptr(m), c, sz);
		if (hs_rebind(m, &a->sec, &host) == HS_OK) {
			accepted++;
			for (i = 0; i < 40; i++) hs_tick(m);
		} else {
			rejected++;
			for (i = 0; i < 3; i++) hs_tick(m);              /* unbound: ignored */
		}
		free(m);
	}
	printf("[hs] corrupt states: 6000 single/double bit flips in the hot part, %d refused, %d accepted (and run safely)\n", rejected, accepted);
	CHECK(rejected > 0);
	free(c); free(snap);
	world_free(a);
}

/* ======================================================================================================
 * real maps
 * ====================================================================================================== */
static uint8_t *load_scrp(const char *path, size_t *size)
{
	FILE *f = fopen(path, "rb");
	uint8_t *img, *raw;
	long n;
	hp2_pack pk;
	const hp2_section *sec;
	if (!f) return NULL;
	fseek(f, 0, SEEK_END); n = ftell(f); rewind(f);
	img = malloc((size_t)n);
	CHECK(img && fread(img, 1, (size_t)n, f) == (size_t)n);
	fclose(f);
	CHECK(hp2_open(&pk, img, (size_t)n) == HP2_OK);
	sec = hp2_find(&pk, "SCRP");
	CHECK(sec);
	raw = malloc(sec->raw_size);
	CHECK(raw && hp2_load(&pk, sec, raw, sec->raw_size) == HP2_OK);
	*size = sec->raw_size;
	free(img);
	return raw;
}

/* host that answers every built-in it is given: bools / numbers / handles from a generator, 5 % blocking */
static uint32_t chaos_state = 1;
static uint32_t chaos(void) { chaos_state ^= chaos_state << 13; chaos_state ^= chaos_state >> 17; chaos_state ^= chaos_state << 5; return chaos_state; }
static int h_chaos(hs_machine *m, void *h, const hs_value *a, unsigned n, hs_value *r)
{
	uint32_t x = chaos();
	(void)m; (void)h; (void)a; (void)n;
	if (x % 20 == 0) return 1;
	switch (r->type) {
	case HS_BOOL: r->v.i = (x >> 8) & 1; break;
	case HS_SHORT: r->v.i = (int32_t)((x >> 8) % 7) - 1; break;
	case HS_LONG: r->v.i = (int32_t)((x >> 8) % 200); break;
	case HS_REAL: r->v.f = (float)((x >> 8) % 100) * 0.5f; break;
	case HS_HANDLE: r->v.h = (x >> 8) % 5 ? (x >> 8) % 64 : HS_HANDLE_NONE; break;
	default: break;
	}
	return 0;
}

/* The SCRP section written by tools/tests/test_scriptconv.py (Xbox-layout nodes -> converter -> pack): 32 nodes, 4 scripts
 * (startup, dormant, static, continuous), 4 globals of which 1 external, 7 built-ins; fx_host is the only host built-in. */
static int h_fx_host(hs_machine *m, void *h, const hs_value *a, unsigned n, hs_value *r)
{
	(void)m; (void)a; (void)n; (void)r;
	(*(int *)h)++;
	return 0;
}

static void test_fixture(const char *path)
{
	size_t size;
	uint8_t *raw = load_scrp(path, &size);
	hs_section sec;
	hs_machine *m = malloc(sizeof *m);
	int calls = 0, g_a, g_b, g_flag, g_ext, i;
	CHECK(raw && m);
	CHECK(hs_open(&sec, raw, size) == HS_OK);
	CHECK(sec.node_count == 32 && sec.script_count == 4 && sec.global_count == 4 && sec.builtin_count == 7 && sec.external_count == 1);
	CHECK(strcmp(hs_script_name(&sec, 0), "fx_start") == 0 && sec.scripts[0].type == HS_S_STARTUP && sec.scripts[1].type == HS_S_DORMANT);
	CHECK(sec.scripts[2].type == HS_S_STATIC && sec.scripts[2].rcls == HS_SHORT && sec.scripts[3].type == HS_S_CONTINUOUS);
	CHECK(hs_find_builtin(&sec, "fx_host") == 6 && hs_find_builtin(&sec, "sleep") == 4 && hs_find_builtin(&sec, "ai_place") < 0);
	g_a = hs_find_global(&sec, "fx_a"); g_b = hs_find_global(&sec, "fx_b"); g_flag = hs_find_global(&sec, "fx_flag");
	g_ext = hs_find_global(&sec, "fx_engine");
	CHECK(g_a == 0 && g_b == 1 && g_flag == 2 && g_ext == 3 && (sec.globals[g_ext].flags & HS_G_EXTERNAL));
	hs_setup(m);
	hs_register(m, "fx_host", h_fx_host);
	CHECK(hs_init(m, &sec, &calls) == HS_OK);
	CHECK(hs_global_get(m, (unsigned)g_a).v.i == 3 && hs_global_get(m, (unsigned)g_b).v.i == 7 && hs_global_get(m, (unsigned)g_flag).v.i == 0);
	CHECK(hs_thread_count(m) == 2);                                           /* startup + continuous */
	hs_tick(m);
	CHECK(hs_global_get(m, (unsigned)g_flag).v.i == 1 && hs_global_get(m, (unsigned)g_a).v.i == 3 && calls == 1);
	for (i = 0; i < 3; i++) hs_tick(m);                                      /* ticks 1..3: sleep 3, then the rest, then wake */
	CHECK(hs_global_get(m, (unsigned)g_a).v.i == 13);
	CHECK(hs_global_get(m, (unsigned)g_ext).type == HS_REAL && hs_global_get(m, (unsigned)g_ext).v.f == 2.5f);
	CHECK(hs_global_get(m, (unsigned)g_b).v.i == 14);                         /* the woken dormant script called the static one */
	CHECK(calls == 4 && hs_thread_count(m) == 1 && hs_state_ptr(m)->errors == 0);
	CHECK(hs_state_ptr(m)->unreg_total == 0);
	printf("[hs] fixture: converter output (%zu bytes) runs as expected\n", size);
	free(m);
	free(raw);
}

static double now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

static void real_run(const char *path, int mode, int bench)
{
	size_t size;
	uint8_t *raw = load_scrp(path, &size);
	hs_section sec;
	hs_machine *m;
	host_t host;
	int t, peak_threads = 0, chaotic = mode >= 1, stress = mode == 2;
	double t0, ms, worst_ms = 0;
	uint64_t nodes = 0;
	unsigned b, nfn;
	const char *slash = strrchr(path, '/');
	char name[32];
	CHECK(raw);
	strncpy(name, slash ? slash + 1 : path, 31); name[31] = 0;
	if (strchr(name, '.')) *strchr(name, '.') = 0;
	CHECK(hs_open(&sec, raw, size) == HS_OK);
	m = malloc(sizeof *m);
	CHECK(m);
	memset(&host, 0, sizeof host);
	hs_setup(m);
	if (chaotic) {
		chaos_state = 12345;
		for (b = 0; b < sec.builtin_count; b++) hs_register(m, hs_builtin_name(&sec, b), h_chaos);
		hs_seed(m, 777);
	}
	CHECK(hs_init(m, &sec, &host) == HS_OK);
	t0 = now_ms();
	for (t = 0; t < 3000; t++) {
		double a;
		if (stress && (t == 0 || t == 300 || t == 900 || t == 1800)) {      /* every dormant script at once, repeatedly */
			unsigned k;
			for (k = 0; k < sec.script_count; k++) hs_wake_script_index(m, k);
		}
		a = now_ms();
		hs_tick(m);
		a = now_ms() - a;
		if (a > worst_ms) worst_ms = a;
		nodes += hs_state_ptr(m)->nodes_tick;
		if ((int)hs_thread_count(m) > peak_threads) peak_threads = (int)hs_thread_count(m);
	}
	ms = now_ms() - t0;
	nfn = hs_unregistered(m, 0, NULL, NULL);
	{
		const hs_state *st = hs_state_ptr(m);
		CHECK(st->tick == 3000);
		if (!stress) CHECK(st->tick_budget_hits == 0);     /* never ran into the per-tick budget */
		printf("[hs] %s %s: 3000 ticks, nodes/tick avg %.1f max %u, threads peak %d (hw %u), frames hw %u values hw %u calls hw %u, "
		       "errors %u, budget stops %u, blocked %u, unregistered %u built-ins / %u calls, overflow %u",
		       name, stress ? "stress " : chaotic ? "chaos  " : "neutral", (double)nodes / 3000.0, st->nodes_max_tick, peak_threads, st->hw_threads,
		       st->hw_frames, st->hw_values, st->hw_calls, st->errors, st->budget_hits, st->blocked_calls, nfn, st->unreg_total,
		       st->thread_overflow);
		if (bench) printf(", host %.1f us/tick avg, worst %.0f us", ms * 1000.0 / 3000.0, worst_ms * 1000.0);
		printf("\n");
		if (chaotic && nfn && !stress) { const char *un = ""; uint32_t uc = 0; hs_unregistered(m, 0, &un, &uc); printf("      unregistered: %s (%u)\n", un, uc); }
		CHECK(st->errors == 0);                            /* no stack / depth failure on any real script */
		if (!stress) CHECK(st->thread_overflow == 0);
	}
	free(m);
	free(raw);
}


int main(int argc, char **argv)
{
	int i, bench = getenv("HS_BENCH") != NULL;
	test_arithmetic();
	test_logic_and_flow();
	test_set_globals_and_init();
	test_script_calls();
	test_recursion_limits();
	test_sleep_semantics();
	test_sleep_until();
	test_wake_and_threads();
	test_wake_order_and_rerun();
	test_sleep_other_script();
	test_static_sleep_and_block();
	test_thread_cap_and_budgets();
	test_unregistered();
	test_begin_random_and_random();
	test_print_inspect();
	test_snapshot();
	test_corrupt_sections();
	test_corrupt_state();
	if (argc >= 2) test_fixture(argv[1]);
	for (i = 2; i < argc; i++) {
		real_run(argv[i], 0, bench);
		real_run(argv[i], 1, bench);
		real_run(argv[i], 2, bench);
	}
	printf("test_hs: ok\n");
	return 0;
}
