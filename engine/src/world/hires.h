/* Streamed high resolution textures (docs/37; ADR-074 for objects, ADR-082 for the BSP and the asynchronous reads).
 *
 * A *set* is a pack next to a texture pack: `<map>.hires.hp2` for the object textures (tools/halo2ps2/objtex.py build_hires) and `<map>.thires.hp2` for the BSP's
 * (texconv.py build_hires_pack). Both hold the textures that have more detail than the resident copy as variants at the source's resolution (a TEXS section of
 * textures only: one mip level, no bindings, stored uncompressed, so CLUT + pixels of a variant are one contiguous range) and HMAP (base texture index -> variant).
 * Only the tables are resident (a few KB).
 *
 * Variants travel through one *ring* of staging slots shared by every set (the arena has no room for a ring per set): a renderer that wants a variant calls
 * hires_request(); if the variant has arrived it is returned (a slot stays untouched for HIRES_HOLD frames, because the GS DMA reads it at the end of the frame
 * and the chain may run a frame later) and the renderer uploads it into the GS texture cache, which is the only LRU; otherwise the call starts an *asynchronous*
 * read of the variant into a free slot (one read in flight for the whole ring, at most `max_loads` started per frame) and returns -1, and the renderer draws the
 * resident texture until a later frame finds the variant ready. Nothing here blocks: the reads run on an IO thread (stream/aread.c). A variant that nobody
 * asks for within HIRES_READY_TTL frames is dropped (the object or surface left the view). Pure C11; the file access is a set of callbacks. */
#ifndef WORLD_HIRES_H
#define WORLD_HIRES_H
#include <stddef.h>
#include <stdint.h>
#include "core/mem.h"
#include "world/tex.h"

#define HIRES_SLOTS_MIN 3u
#define HIRES_SLOTS_MAX 8u
#define HIRES_NONE 0xFFFFu
#define HIRES_READY_TTL 12u
#define HIRES_HOLD 3u

/* Blocking read of `bytes` at file offset `offset` into dst; 0 on success (open time only). */
typedef int (*hires_read_fn)(void *user, uint32_t offset, void *dst, uint32_t bytes);
/* Asynchronous read: start (0 = started, else error), then poll until it returns 1 (finished), < 0 (failed). One read at a time per `user`. */
typedef int (*hires_start_fn)(void *user, uint32_t offset, void *dst, uint32_t bytes);
typedef int (*hires_poll_fn)(void *user);

typedef struct {
	const void *pixels, *clut;      /* valid until HIRES_HOLD frames after the request that returned them */
	uint32_t    pixel_bytes, clut_bytes;
	uint16_t    w, h;
	uint8_t     psm;                /* TEX_PSMT8 / TEX_PSMT4 */
} hires_view;

typedef struct hires_t hires_t;

enum { HIRES_SLOT_FREE = 0, HIRES_SLOT_FLIGHT, HIRES_SLOT_READY, HIRES_SLOT_HOLD };

typedef struct {
	uint8_t       *slot[HIRES_SLOTS_MAX];
	uint8_t        state[HIRES_SLOTS_MAX], age[HIRES_SLOTS_MAX];
	uint16_t       base[HIRES_SLOTS_MAX];          /* the base texture a slot holds / is loading */
	const hires_t *owner[HIRES_SLOTS_MAX];         /* the set it belongs to */
	uint32_t       slot_bytes, slots;
	int            flight;                         /* slot whose read is in flight, -1 none */
	uint32_t       max_loads, loads_frame, frame;
	uint32_t       loads, bytes_read, refused, failed, stale;   /* statistics */
} hires_ring_t;

struct hires_t {
	tex_section    tex;             /* tables only */
	const uint16_t *map;
	uint32_t       map_count;
	uint32_t       max_bytes;       /* the largest variant: CLUT + pixels */
	uint32_t       tex_base;        /* file offset of the TEXS section */
	hires_start_fn start;
	hires_poll_fn  poll;
	void          *aio;
	hires_ring_t  *ring;
};

/* Parse the pack through the blocking `read` (hp2 header, TEXS tables, HMAP). 0 = ok, -1 = unusable (the caller draws without variants). */
int  hires_open(hires_t *h, arena_t *a, hires_read_fn read, void *user, size_t file_size);
/* The ring: `slots` staging slots of `slot_bytes` (the largest `max_bytes` of the sets that use it), from `a`. */
int  hires_ring_init(hires_ring_t *r, arena_t *a, unsigned slots, uint32_t slot_bytes);
void hires_attach(hires_t *h, hires_ring_t *r, hires_start_fn start, hires_poll_fn poll, void *aio);
/* Once per frame, before anything asks: completes the read in flight, ages slots, resets the per frame budget. */
void hires_ring_begin_frame(hires_ring_t *r);
/* Does base texture `base` have a variant? Fills the geometry (pixels / clut NULL). */
int  hires_info(const hires_t *h, unsigned base, hires_view *out);
/* 0 and `out` filled when the variant is in a slot; otherwise -1 (no variant, still loading, or over the budget): a read is started when possible. */
int  hires_request(hires_t *h, unsigned base, hires_view *out);

#endif
