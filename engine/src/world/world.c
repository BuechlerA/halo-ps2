#include "world/world.h"
#include "stream/file.h"
#include "stream/hp2.h"
#include "render/gs.h"
#include <kernel.h>
#include <stdio.h>
#include <string.h>

uint8_t *world_load_section(arena_t *a, const char *path, const char *tag, size_t *raw_size, int optional)
{
	static uint8_t hdr[64 + 32 * HP2_MAX_SECTIONS] __attribute__((aligned(16)));
	hp2_pack pk;
	const hp2_section *s;
	hp2_status st;
	long fsize = file_size_of(path);
	size_t mark;
	uint8_t *raw, *stored;
	size_t hdr_bytes = sizeof hdr;
	if (fsize < 0) { printf("[err] cannot open %s\n", path); return NULL; }
	if ((long)hdr_bytes > fsize) hdr_bytes = (size_t)fsize;
	if (file_read_at(path, 0, hdr, hdr_bytes)) { printf("[err] cannot read the header of %s\n", path); return NULL; }
	st = hp2_open_header(&pk, hdr, hdr_bytes, (size_t)fsize);
	if (st != HP2_OK) { printf("[err] %s: bad pack header (%d)\n", path, (int)st); return NULL; }
	s = hp2_find(&pk, tag);
	if (!s) { if (!optional) printf("[err] %s has no %s section\n", path, tag); return NULL; }
	raw = arena_alloc(a, s->raw_size ? s->raw_size : 16, 64);
	if (!raw) { printf("[err] no room for %s (%u bytes, %u free)\n", tag, (unsigned)s->raw_size, (unsigned)arena_free(a)); return NULL; }
	mark = arena_mark(a);
	stored = arena_alloc(a, s->stored_size ? s->stored_size : 16, 16);       /* scratch: released below */
	if (!stored) { printf("[err] no scratch room for %s (%u bytes)\n", tag, (unsigned)s->stored_size); return NULL; }
	if (file_read_at(path, (long)s->offset, stored, s->stored_size)) { printf("[err] cannot read %s from %s\n", tag, path); return NULL; }
	st = hp2_load_stored(s, stored, raw, s->raw_size);
	arena_release(a, mark);
	if (st != HP2_OK) { printf("[err] %s: decoding %s failed (%d)\n", path, tag, (int)st); return NULL; }
	SyncDCache(raw, raw + s->raw_size);      /* DMA reads batches and textures straight from this memory */
	*raw_size = s->raw_size;
	return raw;
}

/* ---- adapter: TEXS / LMn readers -> renderer texture source ------------------------------------ */
static int src_binding(void *user, uint32_t shader_id, mat_binding_t *out)
{
	const world_t *w = user;
	tex_binding b;
	if (!w->has_tex || !tex_find_binding(&w->tex, shader_id, &b)) return -1;
	out->tex = b.texture == TEX_NONE ? (int16_t)-1 : (int16_t)b.texture;
	out->flags = 0;
	if (b.flags & TEX_B_ALPHA_TEST) out->flags |= MAT_ALPHA_TEST;
	if (b.blend >= TEX_BLEND_ALPHA) out->flags |= MAT_BLEND;
	if (b.flags & TEX_B_TWO_SIDED) out->flags |= MAT_TWO_SIDED;
	out->uv_scale[0] = b.uv_scale[0];
	out->uv_scale[1] = b.uv_scale[1];
	return out->tex < 0 ? -1 : 0;
}

static int src_texture(void *user, int tex, tex_image_t *out)
{
	const world_t *w = user;
	tex_info t;
	tex_mip_view m;
	if (!w->has_tex || tex < 0 || (uint32_t)tex >= w->tex.texture_count) return -1;
	t = tex_get(&w->tex, (uint32_t)tex);
	if (!tex_mip(&w->tex, &t, 0, &m)) return -1;
	out->pixels = m.pixels;
	out->pixel_bytes = m.bytes;
	out->clut = tex_clut(&w->tex, &t);
	out->clut_bytes = (uint32_t)t.clut_entries * 4u;
	out->w = t.width;
	out->h = t.height;
	out->psm = t.psm;
	return 0;
}

static int src_lightmap(void *user, unsigned page, tex_image_t *out)
{
	const world_t *w = user;
	lm_page_info p;
	if (!w->has_lm || !lm_get_page(&w->lm, page, &p)) return -1;
	out->pixels = p.pixels;
	out->pixel_bytes = p.pixel_bytes;
	out->clut = w->lm.clut;
	out->clut_bytes = LM_CLUT_BYTES;
	out->w = p.width;
	out->h = p.height;
	out->psm = p.psm;
	return 0;
}

int world_load(world_t *w, arena_t *a, const char *pack_path, const char *tex_path, int bsp_index)
{
	size_t raw;
	uint8_t *sec;
	char tag[8];
	bsp_status bs;
	memset(w, 0, sizeof *w);
	w->bsp_index = bsp_index;
	/* section tags: BSP0..BSP9, BSPA.. (docs/08-bsp-section.md) */
	snprintf(tag, sizeof tag, "BSP%c", bsp_index < 10 ? '0' + bsp_index : 'A' + (bsp_index - 10));
	sec = world_load_section(a, pack_path, tag, &raw, 0);
	if (!sec) return -1;
	bs = bsp_open(&w->bsp, sec, raw);
	if (bs != BSP_OK) { printf("[err] %s: bsp_open failed (%d)\n", tag, (int)bs); return -1; }
	printf("[pack] %s %s: %u materials, %u batches, %u tris, %u clusters, %u portals, %u KB\n", pack_path, tag,
	       (unsigned)w->bsp.material_count, (unsigned)w->bsp.batch_count, (unsigned)w->bsp.triangle_count,
	       (unsigned)w->bsp.cluster_count, (unsigned)w->bsp.portal_count, (unsigned)(raw / 1024));

	if (tex_path) {
		sec = world_load_section(a, tex_path, "TEXS", &raw, 0);
		if (sec && tex_open(&w->tex, sec, raw) == TEX_OK) {
			w->has_tex = 1;
			printf("[tex] %s TEXS: %u textures, %u shader bindings, %u KB\n", tex_path, (unsigned)w->tex.texture_count,
			       (unsigned)w->tex.binding_count, (unsigned)(raw / 1024));
		} else {
			printf("[warn] no usable TEXS in %s: drawing flat\n", tex_path);
		}
		snprintf(tag, sizeof tag, "LM%d", bsp_index);
		sec = world_load_section(a, tex_path, tag, &raw, 1);
		if (sec && lm_open(&w->lm, sec, raw) == TEX_OK) {
			w->has_lm = 1;
			printf("[tex] %s %s: %u lightmap pages (%u used), gain %.2f, %u KB\n", tex_path, tag, (unsigned)w->lm.page_count,
			       (unsigned)w->lm.used_count, w->lm.gain, (unsigned)(raw / 1024));
		} else {
			printf("[tex] %s has no %s section (this BSP has no lightmap): no lightmap pass\n", tex_path, tag);
		}
	}
	w->source.binding = src_binding;
	w->source.texture = src_texture;
	w->source.lightmap = src_lightmap;
	w->source.user = w;
	w->source.texture_count = w->has_tex ? w->tex.texture_count : 0;
	return 0;
}
