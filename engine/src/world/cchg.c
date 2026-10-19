#include "world/cchg.h"
#include <string.h>

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

int cchg_open(cchg_t *c, const uint8_t *data, size_t size)
{
	uint32_t n;
	memset(c, 0, sizeof *c);
	if (!data || size < 12 || memcmp(data, "CCHG", 4) != 0 || data[4] != 1) return -1;
	n = rd32(data + 8);
	if (n > (size - 12) / CCHG_REC_SIZE) return -1;
	c->rec = data + 12;
	c->count = n;
	return 0;
}

const uint8_t *cchg_find(const cchg_t *c, uint32_t model_tag)
{
	uint32_t lo = 0, hi = c->count;
	while (lo < hi) {
		uint32_t mid = (lo + hi) / 2, id = rd32(c->rec + (size_t)mid * CCHG_REC_SIZE);
		if (id == model_tag) return c->rec + (size_t)mid * CCHG_REC_SIZE;
		if (id < model_tag) lo = mid + 1; else hi = mid;
	}
	return NULL;
}

int cchg_tint(const uint8_t *rec, unsigned source, uint32_t seed, uint8_t rgb[3])
{
	const uint8_t *ch;
	unsigned n, j, k, r, t;
	if (!rec || source < 1 || source > CCHG_MAX_CHANGE) return 0;
	ch = rec + 4 + (size_t)(source - 1) * 68;
	n = ch[0];
	if (n == 0 || n > CCHG_MAX_PERM) return 0;
	seed = seed * 2654435761u + source * 40503u;
	seed ^= seed >> 15; seed *= 2246822519u; seed ^= seed >> 13;
	r = seed & 255u; t = (seed >> 8) & 255u;
	for (j = 0; j + 1 < n && ch[4 + 8 * j] < r; j++) {}                     /* first permutation whose cumulative weight reaches r */
	for (k = 0; k < 3; k++) {
		unsigned lo = ch[5 + 8 * j + k], hi = ch[8 + 8 * j + k];
		rgb[k] = (uint8_t)((lo * (255u - t) + hi * t + 127u) / 255u);
	}
	return 1;
}

int cchg_tint_perm(const uint8_t *rec, unsigned source, unsigned perm, uint32_t seed, uint8_t rgb[3])
{
	const uint8_t *ch;
	unsigned n, k, t;
	if (!rec || source < 1 || source > CCHG_MAX_CHANGE) return 0;
	ch = rec + 4 + (size_t)(source - 1) * 68;
	n = ch[0];
	if (n == 0 || n > CCHG_MAX_PERM) return 0;
	if (perm >= n) perm = n - 1;
	seed = seed * 2654435761u + source * 40503u;
	seed ^= seed >> 15; seed *= 2246822519u; seed ^= seed >> 13;
	t = (seed >> 8) & 255u;
	for (k = 0; k < 3; k++) {
		unsigned lo = ch[5 + 8 * perm + k], hi = ch[8 + 8 * perm + k];
		rgb[k] = (uint8_t)((lo * (255u - t) + hi * t + 127u) / 255u);
	}
	return 1;
}
