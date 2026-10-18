/* CCHG reader: hand-built section (layout of tools/halo2ps2/chgconv.py), lookup, weighted pick, corrupt input. */
#include "world/cchg.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

/* record: change colour a with two permutations (cumulative 128 / 255): red (200,0,0)..(200,0,0) and blue (0,0,100)..(0,0,200) */
static void rec(uint8_t *r, uint32_t tag)
{
	uint8_t *ch = r + 4;
	memset(r, 0, CCHG_REC_SIZE);
	put32(r, tag);
	ch[0] = 2;
	ch[4] = 128; ch[5] = 200; ch[8] = 200;
	ch[12] = 255; ch[15] = 100; ch[18] = 200;
}

int main(void)
{
	static uint8_t sec[12 + 3 * CCHG_REC_SIZE];
	cchg_t c;
	const uint8_t *r;
	uint8_t rgb[3];
	unsigned reds = 0, blues = 0, i;
	memcpy(sec, "CCHG", 4); sec[4] = 1; put32(sec + 8, 3);
	rec(sec + 12, 0x1000); rec(sec + 12 + CCHG_REC_SIZE, 0x2000); rec(sec + 12 + 2 * CCHG_REC_SIZE, 0x3000);
	assert(cchg_open(&c, sec, sizeof sec) == 0 && c.count == 3);
	assert(cchg_find(&c, 0x2000) == sec + 12 + CCHG_REC_SIZE && !cchg_find(&c, 0x2001) && !cchg_find(&c, 0));
	r = cchg_find(&c, 0x3000);
	assert(!cchg_tint(r, 0, 1, rgb) && !cchg_tint(r, 2, 1, rgb) && !cchg_tint(r, 5, 1, rgb) && !cchg_tint(NULL, 1, 1, rgb));   /* only change colour a exists */
	for (i = 0; i < 1000; i++) {
		assert(cchg_tint(r, 1, i, rgb));
		if (rgb[0] == 200 && rgb[1] == 0 && rgb[2] == 0) reds++;
		else { assert(rgb[0] == 0 && rgb[1] == 0 && rgb[2] >= 100 && rgb[2] <= 200); blues++; }
	}
	assert(reds > 400 && blues > 400);                                  /* about half and half */
	{ uint8_t a[3], b[3]; cchg_tint(r, 1, 7, a); cchg_tint(r, 1, 7, b); assert(!memcmp(a, b, 3)); }   /* deterministic per seed */
	assert(cchg_open(&c, sec, 11) != 0 && c.count == 0);               /* truncated, bad magic, bad count */
	sec[0] = 'X'; assert(cchg_open(&c, sec, sizeof sec) != 0); sec[0] = 'C';
	put32(sec + 8, 4); assert(cchg_open(&c, sec, sizeof sec) != 0);
	puts("ok");
	return 0;
}
