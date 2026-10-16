/* Loading a map's BSP (and its textures) from .hp2 packs and wiring them to the renderer. */
#pragma once
#include "core/mem.h"
#include "render/bsp_render.h"
#include "world/bsp.h"
#include "world/tex.h"

typedef struct {
	bsp_section bsp;
	int         bsp_index;
	tex_section tex;
	lm_section  lm;
	int         has_tex, has_lm;
	tex_source_t source;
} world_t;

/* Decompresses section `tag` of the pack at `path` into `a` (64-byte aligned) without reading the
 * rest of the pack, and flushes it from the D-cache for DMA. Returns the buffer, or NULL; a missing
 * section is only reported when `optional` is 0. */
uint8_t *world_load_section(arena_t *a, const char *path, const char *tag, size_t *raw_size, int optional);

/* Loads BSP `bsp_index` from `pack_path`, and TEXS + LMn from `tex_path` when given. Returns 0 on success;
 * missing or broken texture data is reported and the world draws flat. */
int world_load(world_t *w, arena_t *a, const char *pack_path, const char *tex_path, int bsp_index);
