/* Library built-ins of the script machine: arithmetic, comparison, logic, random, print / inspect, sleep, sleep_forever
 * and wake, plus the table that names the special forms the evaluator in hs.c handles itself. Everything here is an
 * ordinary hs_builtin registered by name in hs_setup(); a host may override any of them with hs_register().
 *
 * Arithmetic follows the compiled data: the arguments of +, -, *, /, min, max are REAL values (the compiler turns
 * integer literals into real constants) and the result is converted to the type of the call node (short, long or
 * real), truncating toward zero. Comparison compares integers exactly and everything else as floats. Division by
 * zero gives 0 (the original engine's behaviour there is unverified). */
#include "script/hs.h"
#include <string.h>

static int32_t f2i(float f)
{
	if (!(f == f)) return 0;
	if (f >= 2147483520.0f) return 2147483647;
	if (f <= -2147483648.0f) return (-2147483647 - 1);
	return (int32_t)f;
}

static int is_intish(unsigned t) { return t == HS_BOOL || t == HS_SHORT || t == HS_LONG; }
static float numf(const hs_value *v) { return v->type == HS_REAL ? v->v.f : (float)v->v.i; }
static int32_t numi(const hs_value *v) { return v->type == HS_REAL ? f2i(v->v.f) : v->v.i; }

static void set_num(hs_value *r, float f)
{
	if (r->type == HS_SHORT) r->v.i = (int32_t)(int16_t)(uint16_t)(uint32_t)f2i(f);
	else if (r->type == HS_LONG) r->v.i = f2i(f);
	else { r->type = HS_REAL; r->v.f = f; }
}
static void set_int(hs_value *r, int32_t i)
{
	if (r->type == HS_REAL) r->v.f = (float)i;
	else if (r->type == HS_SHORT) r->v.i = (int32_t)(int16_t)(uint16_t)(uint32_t)i;
	else { if (r->type != HS_LONG) r->type = HS_LONG; r->v.i = i; }
}
static void set_bool(hs_value *r, int b) { r->type = HS_BOOL; r->v.i = b != 0; }

/* -1 / 0 / 1 */
static int cmp(const hs_value *a, const hs_value *b)
{
	if (is_intish(a->type) && is_intish(b->type)) return (a->v.i > b->v.i) - (a->v.i < b->v.i);
	{
		float x = numf(a), y = numf(b);
		return (x > y) - (x < y);
	}
}

static int op_add(hs_machine *m, void *h, const hs_value *a, unsigned n, hs_value *r)
{
	float s = 0.0f;
	unsigned i;
	(void)m; (void)h;
	for (i = 0; i < n; i++) s += numf(&a[i]);
	set_num(r, s);
	return 0;
}
static int op_sub(hs_machine *m, void *h, const hs_value *a, unsigned n, hs_value *r)
{
	float s = n ? numf(&a[0]) : 0.0f;
	unsigned i;
	(void)m; (void)h;
	if (n == 1) s = -s;
	for (i = 1; i < n; i++) s -= numf(&a[i]);
	set_num(r, s);
	return 0;
}
static int op_mul(hs_machine *m, void *h, const hs_value *a, unsigned n, hs_value *r)
{
	float s = n ? 1.0f : 0.0f;
	unsigned i;
	(void)m; (void)h;
	for (i = 0; i < n; i++) s *= numf(&a[i]);
	set_num(r, s);
	return 0;
}
static int op_div(hs_machine *m, void *h, const hs_value *a, unsigned n, hs_value *r)
{
	float s = n ? numf(&a[0]) : 0.0f;
	unsigned i;
	(void)m; (void)h;
	for (i = 1; i < n; i++) {
		float d = numf(&a[i]);
		s = d == 0.0f ? 0.0f : s / d;
	}
	set_num(r, s);
	return 0;
}
static int op_min(hs_machine *m, void *h, const hs_value *a, unsigned n, hs_value *r)
{
	float s = n ? numf(&a[0]) : 0.0f;
	unsigned i;
	(void)m; (void)h;
	for (i = 1; i < n; i++) { float v = numf(&a[i]); if (v < s) s = v; }
	set_num(r, s);
	return 0;
}
static int op_max(hs_machine *m, void *h, const hs_value *a, unsigned n, hs_value *r)
{
	float s = n ? numf(&a[0]) : 0.0f;
	unsigned i;
	(void)m; (void)h;
	for (i = 1; i < n; i++) { float v = numf(&a[i]); if (v > s) s = v; }
	set_num(r, s);
	return 0;
}

static int equal(const hs_value *a, const hs_value *b)
{
	if (a->type == HS_REAL || b->type == HS_REAL) return numf(a) == numf(b);
	return a->v.i == b->v.i;       /* bool, short, long, handle, string, script: the 32 bits */
}
static int op_eq(hs_machine *m, void *h, const hs_value *a, unsigned n, hs_value *r)
{
	(void)m; (void)h;
	set_bool(r, n >= 2 && equal(&a[0], &a[1]));
	return 0;
}
static int op_ne(hs_machine *m, void *h, const hs_value *a, unsigned n, hs_value *r)
{
	(void)m; (void)h;
	set_bool(r, n >= 2 && !equal(&a[0], &a[1]));
	return 0;
}
static int op_lt(hs_machine *m, void *h, const hs_value *a, unsigned n, hs_value *r) { (void)m; (void)h; set_bool(r, n >= 2 && cmp(&a[0], &a[1]) < 0); return 0; }
static int op_gt(hs_machine *m, void *h, const hs_value *a, unsigned n, hs_value *r) { (void)m; (void)h; set_bool(r, n >= 2 && cmp(&a[0], &a[1]) > 0); return 0; }
static int op_le(hs_machine *m, void *h, const hs_value *a, unsigned n, hs_value *r) { (void)m; (void)h; set_bool(r, n >= 2 && cmp(&a[0], &a[1]) <= 0); return 0; }
static int op_ge(hs_machine *m, void *h, const hs_value *a, unsigned n, hs_value *r) { (void)m; (void)h; set_bool(r, n >= 2 && cmp(&a[0], &a[1]) >= 0); return 0; }

static int op_not(hs_machine *m, void *h, const hs_value *a, unsigned n, hs_value *r)
{
	(void)m; (void)h;
	set_bool(r, n < 1 || !a[0].v.i);
	return 0;
}

/* random_range(lo, hi): an integer in [lo, hi) (lo when the range is empty) */
static int op_random_range(hs_machine *m, void *h, const hs_value *a, unsigned n, hs_value *r)
{
	int32_t lo, hi;
	(void)h;
	if (n < 2) return 0;
	lo = numi(&a[0]);
	hi = numi(&a[1]);
	if (hi <= lo) { set_int(r, lo); return 0; }
	set_int(r, (int32_t)((uint32_t)lo + hs_random(m) % ((uint32_t)hi - (uint32_t)lo)));
	return 0;
}
static int op_real_random_range(hs_machine *m, void *h, const hs_value *a, unsigned n, hs_value *r)
{
	float lo, hi, u;
	(void)h;
	if (n < 2) return 0;
	lo = numf(&a[0]);
	hi = numf(&a[1]);
	u = (float)(hs_random(m) >> 8) * (1.0f / 16777216.0f);
	set_num(r, lo + (hi - lo) * u);
	return 0;
}

/* ---- print / inspect ---- */
static unsigned put_uint(char *out, unsigned long v)
{
	char tmp[12];
	unsigned n = 0, k = 0;
	do { tmp[n++] = (char)('0' + v % 10u); v /= 10u; } while (v);
	while (n) out[k++] = tmp[--n];
	return k;
}

static int op_print(hs_machine *m, void *h, const hs_value *a, unsigned n, hs_value *r)
{
	(void)r;
	if (m->log && n >= 1) m->log(h, hs_string(m->sec, a[0].v.i));
	return 0;
}

static int op_inspect(hs_machine *m, void *h, const hs_value *a, unsigned n, hs_value *r)
{
	char buf[48];
	unsigned k = 0;
	const hs_value *v;
	(void)r;
	if (!m->log || n < 1) return 0;
	v = &a[0];
	switch (v->type) {
	case HS_BOOL: memcpy(buf, v->v.i ? "true" : "false", v->v.i ? 5u : 6u); k = v->v.i ? 4u : 5u; break;
	case HS_REAL: {
		float f = v->v.f;
		int32_t ip, frac;
		if (!(f == f)) { memcpy(buf, "nan", 3); k = 3; break; }
		if (f < 0.0f) { buf[k++] = '-'; f = -f; }
		if (f > 2000000000.0f) f = 2000000000.0f;
		ip = (int32_t)f;
		frac = (int32_t)((f - (float)ip) * 1000.0f);
		k += put_uint(buf + k, (unsigned long)ip);
		buf[k++] = '.';
		buf[k++] = (char)('0' + frac / 100);
		buf[k++] = (char)('0' + frac / 10 % 10);
		buf[k++] = (char)('0' + frac % 10);
		break;
	}
	case HS_SHORT:
	case HS_LONG: {
		int32_t i = v->v.i;
		unsigned long u = i < 0 ? (unsigned long)(-(int64_t)i) : (unsigned long)i;
		if (i < 0) buf[k++] = '-';
		k += put_uint(buf + k, u);
		break;
	}
	case HS_STRING: m->log(h, hs_string(m->sec, v->v.i)); return 0;
	default: {
		static const char hex[] = "0123456789abcdef";
		int s;
		buf[k++] = '0'; buf[k++] = 'x';
		for (s = 28; s >= 0; s -= 4) buf[k++] = hex[(v->v.h >> s) & 15u];
		break;
	}
	}
	buf[k] = 0;
	m->log(h, buf);
	return 0;
}

/* ---- sleep / wake ---- */
static int op_sleep(hs_machine *m, void *h, const hs_value *a, unsigned n, hs_value *r)
{
	int32_t ticks;
	(void)h; (void)r;
	if (n < 1) return 0;
	ticks = numi(&a[0]);
	if (n >= 2 && a[1].type == HS_SCRIPT) hs_sleep_script(m, (unsigned)a[1].v.i, ticks);
	else hs_sleep_current(m, ticks);
	return 0;
}
static int op_sleep_forever(hs_machine *m, void *h, const hs_value *a, unsigned n, hs_value *r)
{
	(void)h; (void)r;
	if (n >= 1 && a[0].type == HS_SCRIPT) hs_sleep_script(m, (unsigned)a[0].v.i, -1);
	else hs_sleep_current(m, -1);
	return 0;
}
static int op_wake(hs_machine *m, void *h, const hs_value *a, unsigned n, hs_value *r)
{
	(void)h; (void)r;
	if (n >= 1 && a[0].type == HS_SCRIPT) hs_wake_script_index(m, (unsigned)a[0].v.i);
	return 0;
}

unsigned hs_core_op(const char *name)
{
	static const struct { const char *name; unsigned op; } table[] = {
		{ "begin", HS_OP_BEGIN }, { "begin_random", HS_OP_BEGIN_RANDOM }, { "if", HS_OP_IF }, { "cond", HS_OP_COND },
		{ "set", HS_OP_SET }, { "and", HS_OP_AND }, { "or", HS_OP_OR }, { "sleep_until", HS_OP_SLEEP_UNTIL },
	};
	unsigned i;
	for (i = 0; i < sizeof table / sizeof table[0]; i++)
		if (strcmp(table[i].name, name) == 0) return table[i].op;
	return HS_OP_GENERIC;
}

void hs_core_register(hs_machine *m)
{
	static const struct { const char *name; hs_builtin fn; } lib[] = {
		{ "+", op_add }, { "-", op_sub }, { "*", op_mul }, { "/", op_div }, { "min", op_min }, { "max", op_max },
		{ "=", op_eq }, { "!=", op_ne }, { "<", op_lt }, { ">", op_gt }, { "<=", op_le }, { ">=", op_ge },
		{ "not", op_not }, { "random_range", op_random_range }, { "real_random_range", op_real_random_range },
		{ "print", op_print }, { "inspect", op_inspect }, { "sleep", op_sleep }, { "sleep_forever", op_sleep_forever },
		{ "wake", op_wake },
	};
	unsigned i;
	for (i = 0; i < sizeof lib / sizeof lib[0]; i++) hs_register(m, lib[i].name, lib[i].fn);
}
