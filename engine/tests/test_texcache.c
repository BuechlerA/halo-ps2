/* Host tests for render/texcache. */
#include "render/texcache.h"
#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static tc_t t;

static void check_invariants(void)
{
	unsigned i, p, owned = 0;
	for (i = 0; i < TC_MAX_ENTRIES; i++) {
		const tc_entry *x = &t.entry[i];
		if (!x->valid) continue;
		CHECK(t.map[x->key] == i + 1);
		CHECK(t.clut_used[x->clut_slot]);
		CHECK(x->first_page + x->pages <= t.total_pages);
		for (p = 0; p < x->pages; p++) CHECK(t.owner[x->first_page + p] == i + 1);
		owned += x->pages;
	}
	CHECK(owned + tc_pages_free(&t) == t.total_pages);
}

int main(void)
{
	const tc_entry *e, *e2;
	unsigned k;
	tc_init(&t, 16, 4, 100, 3 * TC_PAGE_BYTES);

	tc_begin_frame(&t, 10);
	CHECK(tc_find(&t, 1) == NULL && tc_find(&t, 99) == NULL && tc_find(&t, 5000) == NULL);
	CHECK(tc_acquire(&t, 1, 4, TC_PAGE_BYTES, &e) == TC_UPLOAD && e->first_page == 0);
	CHECK(tc_acquire(&t, 2, 4, TC_PAGE_BYTES, &e) == TC_UPLOAD && e->first_page == 4);
	CHECK(tc_acquire(&t, 1, 4, TC_PAGE_BYTES, &e) == TC_HIT && e->first_page == 0);
	CHECK(t.hits == 1 && t.misses == 2);
	{                                                           /* tc_find: no touch, no statistics */
		uint32_t lu = t.entry[t.map[2] - 1].last_used, h0 = t.hits, m0 = t.misses;
		CHECK(tc_find(&t, 2) != NULL && tc_find(&t, 2)->first_page == 4 && tc_find(&t, 3) == NULL);
		CHECK(t.hits == h0 && t.misses == m0 && t.entry[t.map[2] - 1].last_used == lu);
	}
	/* Budget: 2 pages already uploaded; one more fits exactly, the next is deferred. */
	CHECK(tc_acquire(&t, 3, 2, TC_PAGE_BYTES, &e) == TC_UPLOAD);
	CHECK(tc_acquire(&t, 4, 2, TC_PAGE_BYTES, &e) == TC_DEFER && t.deferred == 1);
	check_invariants();

	/* Next frame: budget resets; entries used last frame are protected from eviction. */
	tc_begin_frame(&t, 11);
	CHECK(tc_acquire(&t, 4, 8, TC_PAGE_BYTES, &e) == TC_FULL);   /* 10 pages free? no: 16-10 = 6 < 8, all protected */
	CHECK(tc_pages_free(&t) == 6);
	tc_begin_frame(&t, 12);                                      /* now everything is >= 2 frames old */
	CHECK(tc_acquire(&t, 1, 4, TC_PAGE_BYTES, &e) == TC_HIT);    /* touch 1 */
	CHECK(tc_acquire(&t, 4, 8, TC_PAGE_BYTES, &e) == TC_UPLOAD);
	CHECK(t.evictions >= 1);
	CHECK(t.map[1] != 0);                                         /* the touched one survived */
	CHECK(t.map[2] == 0 || t.map[3] == 0);                        /* LRU victims went first */
	check_invariants();

	/* CLUT slots are a separate resource: 4 slots, so a 5th resident texture forces an eviction. */
	tc_flush(&t);
	tc_init(&t, 64, 4, 100, 1u << 30);
	tc_begin_frame(&t, 1);
	for (k = 0; k < 4; k++) CHECK(tc_acquire(&t, k, 1, 100, &e) == TC_UPLOAD);
	CHECK(tc_acquire(&t, 9, 1, 100, &e) == TC_FULL);              /* all 4 used this frame */
	tc_begin_frame(&t, 5);
	CHECK(tc_acquire(&t, 9, 1, 100, &e) == TC_UPLOAD && t.evictions == 1);
	CHECK(e->clut_slot < 4);
	check_invariants();

	/* Fragmentation: freeing the middle gives a run only of the hole's size. */
	tc_init(&t, 8, 8, 100, 1u << 30);
	tc_begin_frame(&t, 1);
	CHECK(tc_acquire(&t, 0, 3, 1, &e) == TC_UPLOAD);
	CHECK(tc_acquire(&t, 1, 2, 1, &e2) == TC_UPLOAD);
	CHECK(tc_acquire(&t, 2, 3, 1, &e) == TC_UPLOAD);
	tc_begin_frame(&t, 4);
	CHECK(tc_acquire(&t, 0, 3, 1, &e) == TC_HIT);
	CHECK(tc_acquire(&t, 2, 3, 1, &e) == TC_HIT);
	CHECK(tc_acquire(&t, 3, 2, 1, &e) == TC_UPLOAD && e->first_page == 3);   /* reuses key 1's hole */
	CHECK(t.map[1] == 0);
	check_invariants();

	/* Bad arguments never corrupt state. */
	CHECK(tc_acquire(&t, 5000, 1, 1, &e) == TC_FULL);
	CHECK(tc_acquire(&t, 7, 0, 1, &e) == TC_FULL);
	CHECK(tc_acquire(&t, 7, 9, 1, &e) == TC_FULL);
	check_invariants();

	if (failures) { printf("%d failure(s)\n", failures); return 1; }
	printf("ok\n");
	return 0;
}
