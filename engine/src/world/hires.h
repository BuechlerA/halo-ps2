/* Streamed high resolution object textures (ADR-074, tools/halo2ps2/objtex.py build_hires, <map>.hires.hp2).
 *
 * A pack next to the object pack holds, for the object textures that have more detail than the resident copy, a variant at the source's resolution (256 per side,
 * 512 for hero textures): a TEXS section of textures only (one mip level, no bindings, stored uncompressed) and HMAP (base texture index -> variant). Only the tables are
 * resident (a few KB). When a frame draws an object big enough on screen the renderer asks for the variant of each of its textures: if the GS texture cache has it
 * nothing is read, otherwise `hires_load` reads CLUT + pixels from the pack into one of a few staging slots (the DMA reads them at the end of the frame, so a slot
 * must outlive two frames: the ring has HIRES_SLOTS_MIN or more) and the renderer uploads them. The VRAM cache is the only LRU: the staging slots are a ring.
 * At most `max_loads` per frame (default 1), so a close-up shot ramps up over a few frames instead of stalling on one. Pure C11; the file access is a callback. */
#ifndef WORLD_HIRES_H
#define WORLD_HIRES_H
#include <stddef.h>
#include <stdint.h>
#include "core/mem.h"
#include "world/tex.h"

#define HIRES_SLOTS_MIN 3u
#define HIRES_NONE 0xFFFFu

/* Read `bytes` at file offset `offset` into dst; 0 on success. */
typedef int (*hires_read_fn)(void *user, uint32_t offset, void *dst, uint32_t bytes);

typedef struct {
	const void *pixels, *clut;      /* valid after hires_load until HIRES_SLOTS - 1 later loads */
	uint32_t    pixel_bytes, clut_bytes;
	uint16_t    w, h;
	uint8_t     psm;                /* TEX_PSMT8 / TEX_PSMT4 */
} hires_view;

typedef struct {
	tex_section    tex;             /* tables only */
	const uint16_t *map;
	uint32_t       map_count;
	hires_read_fn  read;
	void          *user;
	uint32_t       tex_base;        /* file offset of the TEXS section */
	uint8_t       *slot[8];
	uint32_t       slot_bytes, slots, next_slot;
	uint32_t       max_loads, loads_frame;
	uint32_t       loads, bytes_read, refused;   /* statistics */
} hires_t;

/* Parse the pack through `read` (hp2 header, TEXS tables, HMAP) and allocate `slots` staging slots from `a`. 0 = ok, -1 = unusable (the caller draws without variants). */
int  hires_open(hires_t *h, arena_t *a, hires_read_fn read, void *user, size_t file_size, unsigned slots);
void hires_begin_frame(hires_t *h);
/* Does base texture `base` have a variant? Fills the geometry (pixels / clut NULL). */
int  hires_info(const hires_t *h, unsigned base, hires_view *out);
/* Read the variant into the next staging slot; 0 = ok (out filled), -1 = none, over this frame's budget, or unreadable. */
int  hires_load(hires_t *h, unsigned base, hires_view *out);

#endif
