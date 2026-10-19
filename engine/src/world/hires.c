#include "world/hires.h"
#include "stream/hp2.h"
#include <string.h>

#define HDR_READ 4096u

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint32_t pad64(uint32_t v) { return (v + 63u) & ~63u; }

int hires_open(hires_t *h, arena_t *a, hires_read_fn read, void *user, size_t file_size, unsigned slots)
{
	uint8_t hdr[HDR_READ];
	hp2_pack pk;
	const hp2_section *ts, *hs;
	uint8_t *tables, *map;
	uint32_t dof, i, need = 0;
	memset(h, 0, sizeof *h);
	if (slots < HIRES_SLOTS_MIN) slots = HIRES_SLOTS_MIN;
	if (slots > 8u) slots = 8u;
	if (!read || read(user, 0, hdr, HDR_READ) != 0) return -1;
	if (hp2_open_header(&pk, hdr, HDR_READ, file_size) != HP2_OK) return -1;
	ts = hp2_find(&pk, "TEXS");
	hs = hp2_find(&pk, "HMAP");
	if (!ts || !hs || ts->codec != HP2_CODEC_STORED || hs->codec != HP2_CODEC_STORED || ts->raw_size < 64u || hs->raw_size < 4u) return -1;
	{                                                           /* the TEXS header says where its tables end */
		uint8_t th[64];
		if (read(user, ts->offset, th, sizeof th) != 0 || memcmp(th, "TEXS", 4) != 0) return -1;
		dof = rd32(th + 24);
		if (dof < 64u || dof > ts->raw_size || dof > (1u << 20)) return -1;
	}
	tables = arena_alloc(a, pad64(dof), 64);
	map = arena_alloc(a, pad64(hs->raw_size), 64);
	if (!tables || !map) return -1;
	if (read(user, ts->offset, tables, dof) != 0 || read(user, hs->offset, map, hs->raw_size) != 0) return -1;
	if (tex_open_tables(&h->tex, tables, dof, ts->raw_size) != TEX_OK) return -1;
	h->map_count = rd32(map);
	if (h->map_count > 0xFFFFu || 4u + 2u * h->map_count > hs->raw_size) return -1;
	h->map = (const uint16_t *)(map + 4);
	for (i = 0; i < h->tex.texture_count; i++) {                 /* one mip level each; the slot holds CLUT + pixels contiguously */
		tex_info t = tex_get(&h->tex, i);
		uint32_t n;
		if (t.mip_count != 1 || t.pixel_offset < t.clut_offset || t.pixel_offset - t.clut_offset != (uint32_t)t.clut_entries * 4u) return -1;
		n = (uint32_t)t.clut_entries * 4u + t.pixel_bytes;
		if (n > need) need = n;
	}
	h->slot_bytes = pad64(need);
	h->slots = slots;
	for (i = 0; i < slots && need; i++) {
		h->slot[i] = arena_alloc(a, h->slot_bytes, 64);
		if (!h->slot[i]) return -1;
	}
	h->read = read; h->user = user;
	h->tex_base = ts->offset;
	h->max_loads = 1;
	return h->tex.texture_count ? 0 : -1;
}

void hires_begin_frame(hires_t *h) { h->loads_frame = 0; }

static int variant_of(const hires_t *h, unsigned base)
{
	uint16_t v;
	if (!h->map || base >= h->map_count) return -1;
	v = (uint16_t)(((const uint8_t *)h->map)[2 * base] | ((const uint8_t *)h->map)[2 * base + 1] << 8);
	return v == HIRES_NONE || v >= h->tex.texture_count ? -1 : (int)v;
}

int hires_info(const hires_t *h, unsigned base, hires_view *out)
{
	int v = variant_of(h, base);
	tex_info t;
	memset(out, 0, sizeof *out);
	if (v < 0) return -1;
	t = tex_get(&h->tex, (uint32_t)v);
	out->w = t.width; out->h = t.height; out->psm = t.psm;
	out->pixel_bytes = t.pixel_bytes; out->clut_bytes = (uint32_t)t.clut_entries * 4u;
	return 0;
}

int hires_load(hires_t *h, unsigned base, hires_view *out)
{
	int v = variant_of(h, base);
	tex_info t;
	uint8_t *dst;
	uint32_t n;
	memset(out, 0, sizeof *out);
	if (v < 0 || !h->read || !h->slots) return -1;
	if (h->loads_frame >= h->max_loads) { h->refused++; return -1; }
	t = tex_get(&h->tex, (uint32_t)v);
	n = (uint32_t)t.clut_entries * 4u + t.pixel_bytes;
	dst = h->slot[h->next_slot];
	if (!dst || n > h->slot_bytes) return -1;
	if (h->read(h->user, h->tex_base + t.clut_offset, dst, n) != 0) return -1;
	h->next_slot = (h->next_slot + 1u) % h->slots;
	h->loads_frame++; h->loads++; h->bytes_read += n;
	out->w = t.width; out->h = t.height; out->psm = t.psm;
	out->clut = dst; out->clut_bytes = (uint32_t)t.clut_entries * 4u;
	out->pixels = dst + out->clut_bytes; out->pixel_bytes = t.pixel_bytes;
	return 0;
}
