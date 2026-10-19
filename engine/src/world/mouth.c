#include "world/mouth.h"
#include <string.h>

#define M_HEADER 16u
#define M_REC 12u

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint32_t rd16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }

int mouth_open(mouth_section *s, const uint8_t *data, size_t size)
{
	uint32_t count, pool;
	memset(s, 0, sizeof *s);
	if (!data || size < M_HEADER || memcmp(data, "MOUT", 4) != 0 || rd16(data + 4) != 1u) return -1;
	count = rd32(data + 8);
	pool = rd32(data + 12);
	if (pool != M_HEADER + M_REC * count || pool > size) return -1;
	s->recs = data + M_HEADER;
	s->pool = data + pool;
	s->count = count;
	/* every record's bytes must lie inside the pool */
	{
		uint32_t i;
		for (i = 0; i < count; i++) {
			const uint8_t *r = s->recs + M_REC * i;
			if ((size_t)pool + rd32(r + 8) + rd16(r + 6) > size) { memset(s, 0, sizeof *s); return -1; }
		}
	}
	return 0;
}

const uint8_t *mouth_find(const mouth_section *s, uint32_t tag, unsigned variant, unsigned *len)
{
	uint32_t lo = 0, hi = s ? s->count : 0;
	if (len) *len = 0;
	while (lo < hi) {                                  /* records are sorted by (tag, variant) */
		uint32_t mid = (lo + hi) / 2u;
		const uint8_t *r = s->recs + M_REC * mid;
		uint32_t t = rd32(r), v = rd16(r + 4);
		if (t == tag && v == variant) {
			if (len) *len = rd16(r + 6);
			return s->pool + rd32(r + 8);
		}
		if (t < tag || (t == tag && v < variant)) lo = mid + 1u; else hi = mid;
	}
	return NULL;
}
