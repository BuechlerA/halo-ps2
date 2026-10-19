#include "world/tex.h"
#include <string.h>

#define HEADER_SIZE 64u
#define TEXTURE_REC 32u
#define BINDING_REC 32u
#define PAGE_REC 32u

static uint32_t rd16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
static uint32_t rd32(const uint8_t *p) { return rd16(p) | rd16(p + 2) << 16; }
static float rdf(const uint8_t *p) { uint32_t u = rd32(p); float f; memcpy(&f, &u, 4); return f; }
static int pow2(uint32_t v) { return v != 0 && (v & (v - 1)) == 0; }
static uint32_t pad16(uint64_t v) { return (uint32_t)((v + 15) & ~(uint64_t)15); }

uint32_t tex_max_mips(uint32_t w, uint32_t h)
{
	uint32_t n = 1;
	while (n < TEX_MAX_MIPS && (w < h ? w : h) > TEX_MIN_DIM) { w >>= 1; h >>= 1; n++; }
	return n;
}

/* Bytes of one level, padded to 16 (64-bit arithmetic: w, h <= 512). */
static uint32_t level_bytes(uint32_t psm, uint32_t w, uint32_t h)
{
	return pad16((uint64_t)w * h * (psm == TEX_PSMT4 ? 4u : 8u) / 8u);
}

tex_status tex_open(tex_section *s, const uint8_t *d, size_t size)
{
	memset(s, 0, sizeof *s);
	if (size < HEADER_SIZE) return TEX_ERR_TRUNCATED;
	if (memcmp(d, "TEXS", 4) != 0 || rd16(d + 4) != 1) return TEX_ERR_MAGIC;
	uint32_t nt = rd32(d + 8), nb = rd32(d + 12), to = rd32(d + 16), bo = rd32(d + 20);
	uint32_t dof = rd32(d + 24);
	if (rd32(d + 28) != size) return TEX_ERR_LAYOUT;
	if (nt >= TEX_NONE || nb > 0xFFFFu) return TEX_ERR_LAYOUT;
	if (to % 16 || bo % 16 || dof % 16 || dof > size) return TEX_ERR_LAYOUT;
	if (to < HEADER_SIZE || bo < HEADER_SIZE) return TEX_ERR_LAYOUT;
	if (to > size || (uint64_t)nt * TEXTURE_REC > size - to) return TEX_ERR_TRUNCATED;
	if (bo > size || (uint64_t)nb * BINDING_REC > size - bo) return TEX_ERR_TRUNCATED;
	s->data = d;
	s->size = size;
	s->texture_count = nt;
	s->binding_count = nb;
	s->textures = d + to;
	s->bindings = d + bo;

	for (uint32_t i = 0; i < nt; i++) {
		const uint8_t *p = s->textures + (size_t)i * TEXTURE_REC;
		uint32_t w = rd16(p + 4), h = rd16(p + 6), psm = p[8], mip_n = p[9];
		uint32_t co = rd32(p + 12), po = rd32(p + 16), pb = rd32(p + 20), ce = rd16(p + 24);
		if ((psm != TEX_PSMT8 && psm != TEX_PSMT4) || !pow2(w) || !pow2(h)) return TEX_ERR_LAYOUT;
		if (w < TEX_MIN_DIM || w > TEX_MAX_DIM || h < TEX_MIN_DIM || h > TEX_MAX_DIM) return TEX_ERR_LAYOUT;
		if (mip_n < 1 || mip_n > tex_max_mips(w, h)) return TEX_ERR_LAYOUT;
		if (ce != (psm == TEX_PSMT8 ? 256u : 16u)) return TEX_ERR_LAYOUT;
		if (co % 16 || po % 16 || co < dof || (uint64_t)co + ce * 4u > po) return TEX_ERR_LAYOUT;
		if (po > size || pb > size - po) return TEX_ERR_TRUNCATED;
		uint64_t want = 0;
		uint32_t mw = w, mh = h;
		for (uint32_t m = 0; m < mip_n; m++, mw >>= 1, mh >>= 1) want += level_bytes(psm, mw, mh);
		if (want != pb) return TEX_ERR_LAYOUT;
	}
	uint32_t last = 0;
	for (uint32_t i = 0; i < nb; i++) {
		const uint8_t *p = s->bindings + (size_t)i * BINDING_REC;
		uint32_t id = rd32(p), tex = rd16(p + 4);
		if (i > 0 && id <= last) return TEX_ERR_LAYOUT;          /* sorted, unique: binary search */
		last = id;
		if (tex != TEX_NONE && tex >= nt) return TEX_ERR_LAYOUT;
	}
	return TEX_OK;
}

tex_info tex_get(const tex_section *s, uint32_t i)
{
	const uint8_t *p = s->textures + (size_t)i * TEXTURE_REC;
	tex_info t = { rd32(p), (uint16_t)rd16(p + 4), (uint16_t)rd16(p + 6), p[8], p[9], (uint16_t)rd16(p + 10),
	               (uint16_t)rd16(p + 24), rd32(p + 28), rd32(p + 12), rd32(p + 16), rd32(p + 20) };
	return t;
}

int tex_mip(const tex_section *s, const tex_info *t, uint32_t level, tex_mip_view *out)
{
	memset(out, 0, sizeof *out);
	if (level >= t->mip_count) return 0;
	uint32_t off = t->pixel_offset, w = t->width, h = t->height;
	for (uint32_t m = 0; m < level; m++, w >>= 1, h >>= 1) off += level_bytes(t->psm, w, h);
	out->pixels = s->data + off;
	out->width = (uint16_t)w;
	out->height = (uint16_t)h;
	out->bytes = level_bytes(t->psm, w, h);
	return 1;
}

const uint8_t *tex_clut(const tex_section *s, const tex_info *t)
{
	return s->data + t->clut_offset;
}

tex_binding tex_get_binding(const tex_section *s, uint32_t i)
{
	const uint8_t *p = s->bindings + (size_t)i * BINDING_REC;
	tex_binding b = { rd32(p), (uint16_t)rd16(p + 4), (uint16_t)rd16(p + 6), p[8], p[9], (uint16_t)rd16(p + 10),
	                  { rdf(p + 12), rdf(p + 16) }, rd32(p + 20), (uint16_t)rd16(p + 24) };
	return b;
}

int tex_find_binding(const tex_section *s, uint32_t shader_id, tex_binding *out)
{
	uint32_t lo = 0, hi = s->binding_count;
	while (lo < hi) {
		uint32_t mid = lo + (hi - lo) / 2;
		uint32_t id = rd32(s->bindings + (size_t)mid * BINDING_REC);
		if (id == shader_id) { *out = tex_get_binding(s, mid); return 1; }
		if (id < shader_id) lo = mid + 1; else hi = mid;
	}
	return 0;
}

/* ---- LMAP ---------------------------------------------------------------------------------- */
tex_status lm_open(lm_section *s, const uint8_t *d, size_t size)
{
	memset(s, 0, sizeof *s);
	if (size < HEADER_SIZE) return TEX_ERR_TRUNCATED;
	if (memcmp(d, "LMAP", 4) != 0 || rd16(d + 4) != 1) return TEX_ERR_MAGIC;
	uint32_t count = rd32(d + 8), to = rd32(d + 12), co = rd32(d + 16), dof = rd32(d + 20);
	if (rd32(d + 24) != size) return TEX_ERR_LAYOUT;
	if (count > 0xFFFFu || to != HEADER_SIZE) return TEX_ERR_LAYOUT;
	if (co != to + count * PAGE_REC || dof != co + LM_CLUT_BYTES) return TEX_ERR_LAYOUT;
	if (dof > size) return TEX_ERR_TRUNCATED;
	s->data = d;
	s->size = size;
	s->page_count = count;
	s->used_count = rd32(d + 32);
	s->gain = rdf(d + 28);
	s->pages = d + to;
	s->clut = d + co;
	for (uint32_t i = 0; i < count; i++) {
		const uint8_t *p = s->pages + (size_t)i * PAGE_REC;
		uint32_t w = rd16(p), h = rd16(p + 2);
		if (w == 0) continue;
		uint32_t po = rd32(p + 8), pb = rd32(p + 12);
		if (!pow2(w) || !pow2(h) || w > 1024u || h > 1024u || p[4] != TEX_PSMT8) return TEX_ERR_LAYOUT;
		if (po % 16 || po < dof || pb != pad16((uint64_t)w * h)) return TEX_ERR_LAYOUT;
		if (po > size || pb > size - po) return TEX_ERR_TRUNCATED;
	}
	return TEX_OK;
}

int lm_get_page(const lm_section *s, uint32_t index, lm_page_info *out)
{
	memset(out, 0, sizeof *out);
	if (index >= s->page_count) return 0;
	const uint8_t *p = s->pages + (size_t)index * PAGE_REC;
	if (rd16(p) == 0) return 0;
	out->width = (uint16_t)rd16(p);
	out->height = (uint16_t)rd16(p + 2);
	out->psm = p[4];
	out->pixel_offset = rd32(p + 8);
	out->pixel_bytes = rd32(p + 12);
	out->src_width = (uint16_t)rd16(p + 16);
	out->src_height = (uint16_t)rd16(p + 18);
	out->avg_rgba = rd32(p + 20);
	out->avg_luma = rd32(p + 24);
	out->pixels = s->data + out->pixel_offset;
	return 1;
}
