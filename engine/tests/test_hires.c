/* Host test: the streamed high resolution texture pack (world/hires.c) on the pack tools/tests/test_objtex_hero.py writes (HIRES_FIXTURE): 5 base textures, variants for
 * 1 (256 x 256 PSMT4), 3 (128 x 64 PSMT8) and 4 (256 x 128 PSMT8) with known pixel patterns. Checks the tables, loading into the staging ring (contents, lifetime of
 * slots, the per frame budget), refusal of unmapped textures and of unreadable packs, and that every corrupted / truncated header is refused or safe (ASan / UBSan). */
#include "world/hires.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); return 1; } } while (0)

typedef struct { const uint8_t *d; size_t n; unsigned reads; int fail_after; } img_t;

static int rd(void *u, uint32_t off, void *dst, uint32_t bytes)
{
	img_t *m = u;
	if ((uint64_t)off + bytes > m->n) return -1;
	if (m->fail_after >= 0 && (int)m->reads >= m->fail_after) return -1;
	m->reads++;
	memcpy(dst, m->d + off, bytes);
	return 0;
}

static int pixels_ok(const hires_view *v, unsigned base)
{
	uint32_t i;
	const uint8_t *p = v->pixels, *c = v->clut;
	for (i = 0; i < v->pixel_bytes; i++) if (p[i] != (uint8_t)(base * 31u + i)) return 0;
	for (i = 0; i < v->clut_bytes; i++) if (c[i] != (uint8_t)(base * 7u + i)) return 0;
	return 1;
}

int main(int argc, char **argv)
{
	FILE *f;
	uint8_t *buf, *mut;
	long n;
	img_t m;
	hires_t h;
	hires_view v, v1, v3;
	arena_t a;
	static uint8_t mem[1 << 21];
	unsigned i;
	if (argc < 2) { fprintf(stderr, "usage: test_hires pack\n"); return 2; }
	f = fopen(argv[1], "rb");
	CHECK(f != NULL);
	fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
	buf = malloc((size_t)n); mut = malloc((size_t)n);
	CHECK(buf && mut && fread(buf, 1, (size_t)n, f) == (size_t)n);
	fclose(f);
	m.d = buf; m.n = (size_t)n; m.reads = 0; m.fail_after = -1;

	arena_init(&a, "t", mem, sizeof mem);
	CHECK(hires_open(&h, &a, rd, &m, (size_t)n, 3) == 0);
	CHECK(h.tex.texture_count == 3 && h.map_count == 5 && h.slots == 3 && h.max_loads == 1);
	CHECK(h.slot_bytes >= 256 * 128 + 1024 && h.slot_bytes <= 256 * 128 + 4096);       /* the largest variant: 256 x 128 PSMT8 + its CLUT */
	CHECK(hires_info(&h, 0, &v) != 0 && hires_info(&h, 2, &v) != 0 && hires_info(&h, 5, &v) != 0 && hires_info(&h, 100, &v) != 0);
	CHECK(hires_info(&h, 1, &v) == 0 && v.w == 256 && v.h == 256 && v.psm == TEX_PSMT4 && v.pixel_bytes == 256 * 256 / 2 && v.clut_bytes == 64 && v.pixels == NULL);
	CHECK(hires_info(&h, 3, &v) == 0 && v.w == 128 && v.h == 64 && v.psm == TEX_PSMT8 && v.pixel_bytes == 128 * 64 && v.clut_bytes == 1024);

	/* loading: one per frame, contents exact, the ring keeps the last two (slots 3) */
	hires_begin_frame(&h);
	CHECK(hires_load(&h, 0, &v) != 0);                                              /* no variant */
	CHECK(hires_load(&h, 1, &v1) == 0 && v1.w == 256 && pixels_ok(&v1, 1));
	CHECK(((uintptr_t)v1.clut & 63u) == 0);                                         /* 64 aligned: DMA */
	CHECK(hires_load(&h, 3, &v) != 0 && h.refused == 1);                            /* over the frame's budget */
	hires_begin_frame(&h);
	CHECK(hires_load(&h, 3, &v3) == 0 && pixels_ok(&v3, 3) && v3.pixels != v1.pixels);
	hires_begin_frame(&h);
	CHECK(hires_load(&h, 4, &v) == 0 && pixels_ok(&v, 4));
	CHECK(pixels_ok(&v1, 1) && pixels_ok(&v3, 3));                                  /* still intact: two frames of lifetime */
	hires_begin_frame(&h);
	CHECK(hires_load(&h, 1, &v) == 0 && v.pixels == v1.pixels && pixels_ok(&v, 1));   /* the ring came round */
	CHECK(h.loads == 4 && h.bytes_read > 0);
	h.max_loads = 3; hires_begin_frame(&h);
	CHECK(hires_load(&h, 3, &v) == 0 && hires_load(&h, 4, &v) == 0 && hires_load(&h, 1, &v) == 0 && hires_load(&h, 3, &v) != 0);

	/* an unreadable pack: the load fails and the ring does not advance */
	m.fail_after = (int)m.reads; hires_begin_frame(&h);
	CHECK(hires_load(&h, 4, &v) != 0);
	m.fail_after = -1;

	/* refused packs */
	{
		hires_t x;
		arena_t b;
		arena_init(&b, "t2", mem, sizeof mem);
		CHECK(hires_open(&x, &b, NULL, NULL, (size_t)n, 3) != 0);
		m.fail_after = 0;
		CHECK(hires_open(&x, &b, rd, &m, (size_t)n, 3) != 0);
		m.fail_after = -1;
		arena_init(&b, "tiny", mem, 256);
		CHECK(hires_open(&x, &b, rd, &m, (size_t)n, 3) != 0);          /* arena too small */
	}
	/* every byte of the first 4 KB and the tables flipped, every truncation: refused or safe */
	for (i = 0; i < 4096u + 512u && i < (unsigned)n; i++) {
		hires_t x;
		arena_t b;
		img_t mm = { mut, (size_t)n, 0, -1 };
		memcpy(mut, buf, (size_t)n);
		mut[i] ^= 0xA5u;
		arena_init(&b, "fz", mem, sizeof mem);
		if (hires_open(&x, &b, rd, &mm, (size_t)n, 3) == 0) {
			unsigned k;
			hires_view w;
			for (k = 0; k < 6; k++) { hires_begin_frame(&x); (void)hires_info(&x, k, &w); (void)hires_load(&x, k, &w); }
		}
	}
	for (i = 0; i < 4096u + 512u && i < (unsigned)n; i += 7) {
		hires_t x;
		arena_t b;
		img_t mm = { buf, i, 0, -1 };
		arena_init(&b, "tr", mem, sizeof mem);
		(void)hires_open(&x, &b, rd, &mm, i, 3);
	}
	puts("ok");
	free(buf); free(mut);
	return 0;
}
