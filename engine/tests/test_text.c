/* Host tests for render/text. */
#include "render/text.h"
#include "render/gs.h"
#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

int main(void)
{
	static uint8_t atlas[TEXT_ATLAS_W * TEXT_ATLAS_H];
	static uint32_t clut[256];
	static uint64_t mem[(1 + 1 + 6 + 40 * 6 + 4) * 2] __attribute__((aligned(16)));
	static text_batch_t b;
	chain_t c;
	int x, y, lit = 0, code;
	const uint64_t *q;

	text_build_atlas(atlas);
	/* 'A' = code 65: cell column (65-32)%16 = 1, row 2. Top row of A is 0x0E = .XXX. */
	{
		int cx = 1 * 8, cy = 2 * 8;
		CHECK(atlas[cy * TEXT_ATLAS_W + cx + 0] == 0 && atlas[cy * TEXT_ATLAS_W + cx + 1] == 1 &&
		      atlas[cy * TEXT_ATLAS_W + cx + 3] == 1 && atlas[cy * TEXT_ATLAS_W + cx + 4] == 0);
		CHECK(atlas[(cy + 3) * TEXT_ATLAS_W + cx] == 1 && atlas[(cy + 3) * TEXT_ATLAS_W + cx + 4] == 1);   /* crossbar */
		CHECK(atlas[(cy + 7) * TEXT_ATLAS_W + cx + 2] == 0);                                              /* 8th row blank */
	}
	for (y = 0; y < TEXT_ATLAS_H; y++) for (x = 0; x < TEXT_ATLAS_W; x++) lit += atlas[y * TEXT_ATLAS_W + x] > 1;
	CHECK(lit == 0);                                                      /* indices 0/1 only */
	/* lower case folds to upper case; unknown characters draw '?'. */
	code = 'a'; CHECK(memcmp(&atlas[((code - 32) / 16 * 8) * TEXT_ATLAS_W], &atlas[(('A' - 32) / 16 * 8) * TEXT_ATLAS_W], 1) == 0);
	{
		int cell = '~' - 32, qc = '?' - 32, same = 1;
		for (y = 0; y < 8; y++) for (x = 0; x < 8; x++)
			same &= atlas[((cell / 16) * 8 + y) * TEXT_ATLAS_W + (cell % 16) * 8 + x] ==
			        atlas[((qc / 16) * 8 + y) * TEXT_ATLAS_W + (qc % 16) * 8 + x];
		CHECK(same);
	}
	text_build_clut(clut);
	CHECK(clut[0] == 0 && clut[1] == 0x80FFFFFFu && clut[2] == 0);

	text_batch_clear(&b);
	text_batch_print(&b, 10, 20, 0xFF8000, "AB \nC");
	CHECK(b.nchars == 3);                                                 /* spaces are skipped */
	CHECK(b.c[0].x == 10 && b.c[1].x == 16 && b.c[2].x == 10 && b.c[2].y == 28);
	CHECK(b.c[0].r == 0xFF && b.c[0].g == 0x80 && b.c[0].b == 0);
	{
		int i;
		text_batch_clear(&b);
		for (i = 0; i < TEXT_MAX_CHARS + 10; i++) text_batch_print(&b, 0, 0, 0, "X");
		CHECK(b.nchars == TEXT_MAX_CHARS);                                /* capped, no overflow */
	}

	text_batch_clear(&b);
	text_batch_print(&b, 0, 0, 0xFFFFFF, "HI");
	memset(mem, 0xAB, sizeof mem);
	chain_init(&c, mem, 64);
	CHECK(text_batch_emit(&b, &c, 100, 50, 512, 448) == 0);
	{
		/* the DIRECT length must equal what was written: the last allocated qword is filled in,
		 * the one after it is untouched */
		const uint8_t *last = (const uint8_t *)mem + (size_t)(chain_qwords(&c) - 1) * 16, *after = last + 16;
		int i, last_written = 0, after_clean = 1;
		for (i = 0; i < 16; i++) { last_written |= last[i] != 0xAB; after_clean &= after[i] == 0xAB; }
		CHECK(last_written && after_clean);
	}
	CHECK(chain_qwords(&c) == 1 + 1 + (1 + 4 + 1 + 2 * 6) + 0);
	q = mem + 2;                                                           /* after the tag */
	CHECK(q[0] == 0);                                                      /* words 0,1: NOP NOP */
	CHECK((uint32_t)q[1] == VIF_CODE(VIF_FLUSHA, 0, 0) && (uint32_t)(q[1] >> 32) == VIF_CODE(VIF_DIRECT, 0, 1 + 4 + 1 + 12));
	text_batch_clear(&b);
	CHECK(text_batch_emit(&b, &c, 0, 0, 512, 448) == 0);                  /* nothing to draw: no chain growth */

	if (failures) { printf("%d failure(s)\n", failures); return 1; }
	printf("ok\n");
	return 0;
}
