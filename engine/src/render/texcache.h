/* VRAM texture residency (docs/03 §5.1 step 3). Pure C11, host-tested.
 *
 * The texture window is managed in 8 KB GS pages. A resource (a texture with its mip chain, or a
 * lightmap page) is identified by a small integer key and occupies a contiguous run of pages plus
 * one CLUT slot. Lookups touch the entry; misses claim free pages, evicting least-recently-used
 * entries that were not used this frame or last frame (the GS may still be drawing the previous
 * frame from them). A per-frame upload budget caps PATH3 traffic (spec: 1.5 MB). */
#ifndef RENDER_TEXCACHE_H
#define RENDER_TEXCACHE_H
#include <stdint.h>

#define TC_MAX_PAGES 512
#define TC_MAX_KEYS  4096
#define TC_MAX_ENTRIES 512
#define TC_PAGE_BYTES 8192u

typedef struct {
	uint32_t key, last_used;
	uint16_t first_page, pages, clut_slot;
	uint8_t  valid;
} tc_entry;

typedef struct {
	tc_entry entry[TC_MAX_ENTRIES];
	uint16_t map[TC_MAX_KEYS];          /* key -> entry index + 1 (0 = not resident) */
	uint16_t owner[TC_MAX_PAGES];       /* page -> entry index + 1 (0 = free) */
	uint8_t  clut_used[256];
	uint16_t total_pages, clut_slots, max_keys;
	uint32_t frame;
	uint32_t upload_bytes, budget_bytes;   /* this frame */
	/* statistics for the profiler */
	uint32_t hits, misses, evictions, deferred, failed;
} tc_t;

typedef enum {
	TC_HIT = 0,        /* resident; entry valid */
	TC_UPLOAD,         /* newly allocated: the caller must upload into entry.first_page / clut_slot */
	TC_DEFER,          /* over this frame's upload budget: try again next frame */
	TC_FULL            /* no space even after evicting everything evictable */
} tc_result;

void tc_init(tc_t *t, unsigned total_pages, unsigned clut_slots, unsigned max_keys, uint32_t budget_bytes);
/* Start of a frame: resets the upload budget and per-frame statistics. */
void tc_begin_frame(tc_t *t, uint32_t frame);
/* Look up `key` (< max_keys); on a miss allocate `pages` pages and `bytes` of upload traffic. */
tc_result tc_acquire(tc_t *t, uint32_t key, unsigned pages, uint32_t bytes, const tc_entry **out);
/* Forget everything (BSP switch). */
void tc_flush(tc_t *t);
unsigned tc_pages_free(const tc_t *t);
/* The valid entry of `key` or NULL; does not touch it (no LRU update, no statistics). */
const tc_entry *tc_find(const tc_t *t, uint32_t key);

#endif
