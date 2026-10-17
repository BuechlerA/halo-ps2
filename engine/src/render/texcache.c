#include "render/texcache.h"
#include <string.h>

void tc_init(tc_t *t, unsigned total_pages, unsigned clut_slots, unsigned max_keys, uint32_t budget_bytes)
{
	memset(t, 0, sizeof *t);
	t->total_pages = (uint16_t)(total_pages > TC_MAX_PAGES ? TC_MAX_PAGES : total_pages);
	t->clut_slots = (uint16_t)(clut_slots > 256 ? 256 : clut_slots);
	t->max_keys = (uint16_t)(max_keys > TC_MAX_KEYS ? TC_MAX_KEYS : max_keys);
	t->budget_bytes = budget_bytes;
}

void tc_begin_frame(tc_t *t, uint32_t frame)
{
	t->frame = frame;
	t->upload_bytes = 0;
	t->hits = t->misses = t->evictions = t->deferred = t->failed = 0;
}

void tc_flush(tc_t *t)
{
	memset(t->entry, 0, sizeof t->entry);
	memset(t->map, 0, sizeof t->map);
	memset(t->owner, 0, sizeof t->owner);
	memset(t->clut_used, 0, sizeof t->clut_used);
}

unsigned tc_pages_free(const tc_t *t)
{
	unsigned i, n = 0;
	for (i = 0; i < t->total_pages; i++) n += t->owner[i] == 0;
	return n;
}

static void evict(tc_t *t, unsigned e)
{
	tc_entry *x = &t->entry[e];
	unsigned i;
	for (i = 0; i < x->pages; i++) t->owner[x->first_page + i] = 0;
	t->clut_used[x->clut_slot] = 0;
	t->map[x->key] = 0;
	x->valid = 0;
	t->evictions++;
}

/* First free run of `pages` pages, or -1. */
static int find_run(const tc_t *t, unsigned pages)
{
	unsigned i, run = 0;
	for (i = 0; i < t->total_pages; i++) {
		run = t->owner[i] ? 0 : run + 1;
		if (run == pages) return (int)(i + 1 - pages);
	}
	return -1;
}

static int find_clut(const tc_t *t)
{
	unsigned i;
	for (i = 0; i < t->clut_slots; i++) if (!t->clut_used[i]) return (int)i;
	return -1;
}

/* Evictable = not touched this frame or the previous one. Returns the LRU victim or -1. */
static int pick_victim(const tc_t *t)
{
	int best = -1;
	unsigned i;
	for (i = 0; i < TC_MAX_ENTRIES; i++) {
		const tc_entry *x = &t->entry[i];
		if (!x->valid || x->last_used + 1 >= t->frame) continue;
		if (best < 0 || x->last_used < t->entry[best].last_used) best = (int)i;
	}
	return best;
}

tc_result tc_acquire(tc_t *t, uint32_t key, unsigned pages, uint32_t bytes, const tc_entry **out)
{
	int run, clut, slot = -1;
	unsigned i;
	if (key >= t->max_keys || pages == 0 || pages > t->total_pages) { t->failed++; return TC_FULL; }
	if (t->map[key]) {
		tc_entry *x = &t->entry[t->map[key] - 1];
		x->last_used = t->frame;
		t->hits++;
		*out = x;
		return TC_HIT;
	}
	if (t->upload_bytes + bytes > t->budget_bytes && t->upload_bytes > 0) { t->deferred++; return TC_DEFER; }

	for (;;) {
		run = find_run(t, pages);
		clut = find_clut(t);
		if (run >= 0 && clut >= 0) break;
		slot = pick_victim(t);
		if (slot < 0) { t->failed++; return TC_FULL; }
		evict(t, (unsigned)slot);
	}
	for (i = 0; i < TC_MAX_ENTRIES; i++) if (!t->entry[i].valid) break;
	if (i == TC_MAX_ENTRIES) { t->failed++; return TC_FULL; }
	{
		tc_entry *x = &t->entry[i];
		x->key = key; x->last_used = t->frame; x->first_page = (uint16_t)run;
		x->pages = (uint16_t)pages; x->clut_slot = (uint16_t)clut; x->valid = 1;
		for (slot = 0; slot < (int)pages; slot++) t->owner[run + slot] = (uint16_t)(i + 1);
		t->clut_used[clut] = 1;
		t->map[key] = (uint16_t)(i + 1);
		t->upload_bytes += bytes;
		t->misses++;
		*out = x;
	}
	return TC_UPLOAD;
}
