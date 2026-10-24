/* Host test: the streamed high resolution texture pack (world/hires.c) on the pack tools/tests/test_objtex_hero.py writes (HIRES_FIXTURE): 5 base textures, variants for
 * 1 (256 x 256 PSMT4), 3 (128 x 64 PSMT8) and 4 (256 x 128 PSMT8) with known pixel patterns. Checks the tables, the asynchronous loading into the shared staging ring (latency, one read in
 * flight, contents, slot lifetimes, the per frame budget, unclaimed variants, failures, two sets on one ring), refusal of unmapped textures and of unreadable packs, and that every
 * corrupted / truncated header is refused or safe (ASan / UBSan). */
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

/* a fake asynchronous reader: the data is copied at start, the read "finishes" `latency` polls later */
typedef struct { img_t *m; int pending, latency, left, result, starts, fail_start; } aio_t;
static int a_start(void *u, uint32_t off, void *dst, uint32_t bytes)
{
	aio_t *a = u;
	if (a->pending || a->fail_start) return -1;
	a->result = rd(a->m, off, dst, bytes) == 0 ? 1 : -1;
	a->pending = 1; a->left = a->latency; a->starts++;
	return 0;
}
static int a_poll(void *u)
{
	aio_t *a = u;
	if (!a->pending) return -1;
	if (a->left > 0) { a->left--; return 0; }
	a->pending = 0;
	return a->result;
}
int main(int argc, char **argv)
{
	FILE *f;
	uint8_t *buf, *mut;
	long n;
	img_t m;
	hires_t h, h2;
	hires_ring_t ring;
	aio_t io = { 0 }, io2 = { 0 };
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
	io.m = &m; io2.m = &m;

	arena_init(&a, "t", mem, sizeof mem);
	CHECK(hires_open(&h, &a, rd, &m, (size_t)n) == 0);
	CHECK(h.tex.texture_count == 3 && h.map_count == 5);
	CHECK(h.max_bytes >= 256 * 128 + 1024 && h.max_bytes <= 256 * 128 + 4096);          /* the largest variant: 256 x 128 PSMT8 + its CLUT */
	CHECK(hires_ring_init(&ring, &a, 3, h.max_bytes) == 0 && ring.slots == 3 && ring.flight == -1 && ring.max_loads == 1);
	hires_attach(&h, &ring, a_start, a_poll, &io);
	CHECK(hires_info(&h, 0, &v) != 0 && hires_info(&h, 2, &v) != 0 && hires_info(&h, 5, &v) != 0 && hires_info(&h, 100, &v) != 0);
	CHECK(hires_info(&h, 1, &v) == 0 && v.w == 256 && v.h == 256 && v.psm == TEX_PSMT4 && v.pixel_bytes == 256 * 256 / 2 && v.clut_bytes == 64 && v.pixels == NULL);
	CHECK(hires_info(&h, 3, &v) == 0 && v.w == 128 && v.h == 64 && v.psm == TEX_PSMT8 && v.pixel_bytes == 128 * 64 && v.clut_bytes == 1024);

	/* a request starts an asynchronous read and answers -1; the variant is there `latency` frames later; one read is in flight at a time */
	io.latency = 2;
	hires_ring_begin_frame(&ring);
	CHECK(hires_request(&h, 0, &v) != 0);                                           /* no variant: nothing started */
	CHECK(hires_request(&h, 1, &v1) != 0 && io.starts == 1 && ring.flight >= 0 && ring.loads == 1);
	CHECK(hires_request(&h, 1, &v1) != 0 && io.starts == 1);                        /* still in flight: no second read */
	CHECK(hires_request(&h, 3, &v) != 0 && io.starts == 1 && ring.refused == 1);    /* the ring has one read in flight, the frame one load */
	hires_ring_begin_frame(&ring);
	CHECK(hires_request(&h, 1, &v1) != 0);                                          /* running */
	hires_ring_begin_frame(&ring);
	CHECK(hires_request(&h, 1, &v1) != 0 && ring.flight >= 0);
	hires_ring_begin_frame(&ring);                                                  /* finished */
	CHECK(ring.flight == -1);
	CHECK(hires_request(&h, 1, &v1) == 0 && v1.w == 256 && pixels_ok(&v1, 1) && ((uintptr_t)v1.clut & 63u) == 0);
	CHECK(hires_request(&h, 1, &v) == 0 && v.pixels == v1.pixels);                  /* asked again while held: the same slot */

	/* a second variant loads while the first is on hold; both stay intact for HIRES_HOLD frames */
	io.latency = 0;
	CHECK(hires_request(&h, 3, &v3) != 0);
	hires_ring_begin_frame(&ring);
	CHECK(hires_request(&h, 3, &v3) == 0 && pixels_ok(&v3, 3) && v3.pixels != v1.pixels && pixels_ok(&v1, 1));
	for (i = 0; i < HIRES_HOLD; i++) { hires_ring_begin_frame(&ring); CHECK(pixels_ok(&v3, 3)); }
	hires_ring_begin_frame(&ring);                                                  /* the hold is over: both slots are free again */
	for (i = 0; i < ring.slots; i++) CHECK(ring.state[i] == HIRES_SLOT_FREE);

	/* a loaded variant nobody asks for is dropped after HIRES_READY_TTL frames (the object left the view) */
	CHECK(hires_request(&h, 4, &v) != 0);
	hires_ring_begin_frame(&ring);
	for (i = 0; i < HIRES_READY_TTL + 1; i++) hires_ring_begin_frame(&ring);
	CHECK(ring.stale == 1);
	for (i = 0; i < ring.slots; i++) CHECK(ring.state[i] == HIRES_SLOT_FREE);

	/* when every slot waits or is on hold, the oldest unclaimed ready one is recycled; with none to recycle the request is refused */
	ring.max_loads = 4;
	for (i = 0; i < 3; i++) {
		static const unsigned order[3] = { 1, 3, 4 };
		hires_ring_begin_frame(&ring);
		CHECK(hires_request(&h, order[i], &v) != 0);
		hires_ring_begin_frame(&ring);                                              /* ready, unclaimed */
	}
	for (i = 0; i < ring.slots; i++) CHECK(ring.state[i] == HIRES_SLOT_READY);
	{
		unsigned stale = ring.stale;
		CHECK(hires_request(&h, 1, &v1) == 0 && pixels_ok(&v1, 1));                 /* claim one: it goes on hold */
		CHECK(hires_request(&h, 3, &v3) == 0 && hires_request(&h, 4, &v) == 0);      /* all three on hold */
		hires_ring_begin_frame(&ring);
		{
			unsigned refused = ring.refused;
			io.latency = 0;
			CHECK(h.map[1] != HIRES_NONE);
			CHECK(hires_info(&h, 1, &v) == 0);
			hires_ring_begin_frame(&ring);
			CHECK(ring.refused >= refused && ring.stale == stale);
		}
	}
	for (i = 0; i < HIRES_HOLD + 2; i++) hires_ring_begin_frame(&ring);

	/* a read that cannot start, and a read that fails: the slot goes back, the statistics count it, the next request works */
	{
		unsigned failed = ring.failed;
		io.fail_start = 1; hires_ring_begin_frame(&ring);
		CHECK(hires_request(&h, 3, &v) != 0 && ring.failed == failed + 1 && ring.flight == -1);
		io.fail_start = 0;
		m.fail_after = (int)m.reads;
		hires_ring_begin_frame(&ring);
		CHECK(hires_request(&h, 3, &v) != 0 && ring.flight >= 0);
		hires_ring_begin_frame(&ring);
		CHECK(ring.flight == -1 && ring.failed == failed + 2);
		m.fail_after = -1;
		hires_ring_begin_frame(&ring);
		CHECK(hires_request(&h, 3, &v) != 0);
		hires_ring_begin_frame(&ring);
		CHECK(hires_request(&h, 3, &v) == 0 && pixels_ok(&v, 3));
	}
	for (i = 0; i < HIRES_HOLD + 2; i++) hires_ring_begin_frame(&ring);

	/* two sets on one ring (objects and the BSP): the same base texture number is a different texture in each, and the slots never mix */
	CHECK(hires_open(&h2, &a, rd, &m, (size_t)n) == 0);
	hires_attach(&h2, &ring, a_start, a_poll, &io2);
	hires_ring_begin_frame(&ring);
	CHECK(hires_request(&h, 1, &v1) != 0);
	hires_ring_begin_frame(&ring);
	CHECK(hires_request(&h2, 1, &v) != 0);                                          /* the other set's own read, not h's slot */
	hires_ring_begin_frame(&ring);
	CHECK(hires_request(&h, 1, &v1) == 0 && pixels_ok(&v1, 1));
	CHECK(hires_request(&h2, 1, &v) == 0 && pixels_ok(&v, 1) && v.pixels != v1.pixels);
	CHECK(io.starts > 0 && io2.starts == 1);
	for (i = 0; i < HIRES_HOLD + 2; i++) hires_ring_begin_frame(&ring);

	/* refused packs */
	{
		hires_t x;
		arena_t b;
		arena_init(&b, "t2", mem, sizeof mem);
		CHECK(hires_open(&x, &b, NULL, NULL, (size_t)n) != 0);
		m.fail_after = 0;
		CHECK(hires_open(&x, &b, rd, &m, (size_t)n) != 0);
		m.fail_after = -1;
		arena_init(&b, "tiny", mem, 256);
		CHECK(hires_open(&x, &b, rd, &m, (size_t)n) != 0);          /* arena too small */
		arena_init(&b, "ring", mem, 256);
		CHECK(hires_ring_init(&ring, &b, 3, 4096) != 0);            /* no room for the slots */
	}
	/* every byte of the first 4 KB and the tables flipped, every truncation: refused or safe */
	for (i = 0; i < 4096u + 512u && i < (unsigned)n; i++) {
		hires_t x;
		hires_ring_t rr;
		arena_t b;
		aio_t ii = { 0 };
		img_t mm = { mut, (size_t)n, 0, -1 };
		ii.m = &mm;
		memcpy(mut, buf, (size_t)n);
		mut[i] ^= 0xA5u;
		arena_init(&b, "fz", mem, sizeof mem);
		if (hires_open(&x, &b, rd, &mm, (size_t)n) == 0 && hires_ring_init(&rr, &b, 3, x.max_bytes) == 0) {
			unsigned k;
			hires_view w;
			hires_attach(&x, &rr, a_start, a_poll, &ii);
			for (k = 0; k < 8; k++) { hires_ring_begin_frame(&rr); (void)hires_info(&x, k % 6, &w); (void)hires_request(&x, k % 6, &w); }
		}
	}
	for (i = 0; i < 4096u + 512u && i < (unsigned)n; i += 7) {
		hires_t x;
		arena_t b;
		img_t mm = { buf, i, 0, -1 };
		arena_init(&b, "tr", mem, sizeof mem);
		(void)hires_open(&x, &b, rd, &mm, i);
	}
	puts("ok");
	free(buf); free(mut);
	return 0;
}
