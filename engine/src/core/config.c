#include "core/config.h"
#include <stdlib.h>
#include <string.h>

static int is_space(char c) { return c == ' ' || c == '\t' || c == '\r'; }

int cfg_parse(cfg_t *c, const char *text, size_t len)
{
	size_t i = 0;
	int bad = 0;
	c->count = 0;
	while (i < len) {
		size_t end = i, k, a;
		size_t n;
		while (end < len && text[end] != '\n') end++;
		n = end - i;
		if (n < CFG_KEY_LEN + CFG_ARG_LEN + 2) {
			const char *p = text + i, *q = text + end;
			while (p < q && is_space(*p)) p++;
			while (q > p && is_space(q[-1])) q--;
			if (p < q && *p != '#') {
				for (k = 0; p + k < q && !is_space(p[k]); k++) {}
				a = k;
				while (p + a < q && is_space(p[a])) a++;
				if (k >= CFG_KEY_LEN || (size_t)(q - (p + a)) >= CFG_ARG_LEN || c->count >= CFG_MAX_ENTRIES) {
					bad++;
				} else {
					cfg_entry *e = &c->entry[c->count++];
					memcpy(e->key, p, k);
					e->key[k] = 0;
					memcpy(e->arg, p + a, (size_t)(q - (p + a)));
					e->arg[q - (p + a)] = 0;
				}
			}
		} else {
			bad++;
		}
		i = end + 1;
	}
	return bad;
}

int cfg_set(cfg_t *c, const char *key, const char *arg)
{
	int i;
	if (strlen(key) >= CFG_KEY_LEN || strlen(arg) >= CFG_ARG_LEN) return -1;
	for (i = 0; i < c->count; i++)
		if (strcmp(c->entry[i].key, key) == 0) { strcpy(c->entry[i].arg, arg); return 0; }
	if (c->count >= CFG_MAX_ENTRIES) return -1;
	strcpy(c->entry[c->count].key, key); strcpy(c->entry[c->count].arg, arg); c->count++;
	return 0;
}

const char *cfg_nth(const cfg_t *c, const char *key, int n)
{
	int i;
	for (i = 0; i < c->count; i++)
		if (strcmp(c->entry[i].key, key) == 0 && n-- == 0) return c->entry[i].arg;
	return NULL;
}

const char *cfg_str(const cfg_t *c, const char *key, const char *def)
{
	const char *v = cfg_nth(c, key, 0);
	return v && *v ? v : def;
}

long cfg_int(const cfg_t *c, const char *key, long def)
{
	const char *v = cfg_nth(c, key, 0);
	char *end;
	long r;
	if (!v || !*v) return def;
	r = strtol(v, &end, 0);
	return end == v ? def : r;
}

int cfg_floats(const cfg_t *c, const char *key, float *out, int max)
{
	const char *v = cfg_nth(c, key, 0);
	int n = 0;
	while (v && *v && n < max) {
		char *end;
		float f = strtof(v, &end);
		if (end == v) break;
		out[n++] = f;
		v = end;
	}
	return n;
}
