#include "world/hires.h"
#include "stream/hp2.h"
#include <string.h>

#define HDR_READ 4096u

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint32_t pad64(uint32_t v) { return (v + 63u) & ~63u; }

int hires_open(hires_t *h, arena_t *a, hires_read_fn read, void *user, size_t file_size)
{
	uint8_t hdr[HDR_READ];
	hp2_pack pk;
	const hp2_section *ts, *hs;
	uint8_t *tables, *map;
	uint32_t dof, i, need = 0;
	memset(h, 0, sizeof *h);
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
	h->max_bytes = pad64(need);
	h->tex_base = ts->offset;
	(void)read;
	return h->tex.texture_count ? 0 : -1;
}

int hires_ring_init(hires_ring_t *r, arena_t *a, unsigned slots, uint32_t slot_bytes)
{
	unsigned i;
	memset(r, 0, sizeof *r);
	if (slots < HIRES_SLOTS_MIN) slots = HIRES_SLOTS_MIN;
	if (slots > HIRES_SLOTS_MAX) slots = HIRES_SLOTS_MAX;
	if (!slot_bytes) return -1;
	r->slot_bytes = pad64(slot_bytes);
	for (i = 0; i < slots; i++) {
		r->slot[i] = arena_alloc(a, r->slot_bytes, 64);
		if (!r->slot[i]) return -1;
	}
	r->slots = slots;
	r->flight = -1;
	r->max_loads = 1;
	return 0;
}

void hires_attach(hires_t *h, hires_ring_t *r, hires_start_fn start, hires_poll_fn poll, void *aio)
{
	h->ring = r; h->start = start; h->poll = poll; h->aio = aio;
}

void hires_ring_begin_frame(hires_ring_t *r)
{
	unsigned i;
	r->frame++;
	r->loads_frame = 0;
	if (r->flight >= 0) {
		const hires_t *o = r->owner[r->flight];
		int rc = o && o->poll ? o->poll(o->aio) : -1;
		if (rc > 0) { r->state[r->flight] = HIRES_SLOT_READY; r->age[r->flight] = 0; r->flight = -1; }
		else if (rc < 0) { r->state[r->flight] = HIRES_SLOT_FREE; r->failed++; r->flight = -1; }
	}
	for (i = 0; i < r->slots; i++) {
		if (r->state[i] == HIRES_SLOT_READY) {
			if (++r->age[i] > HIRES_READY_TTL) { r->state[i] = HIRES_SLOT_FREE; r->stale++; }
		} else if (r->state[i] == HIRES_SLOT_HOLD) {
			if (++r->age[i] > HIRES_HOLD) r->state[i] = HIRES_SLOT_FREE;
		}
	}
}

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

int hires_request(hires_t *h, unsigned base, hires_view *out)
{
	int v = variant_of(h, base);
	hires_ring_t *r = h->ring;
	tex_info t;
	uint32_t n;
	unsigned i;
	int free_slot = -1;
	memset(out, 0, sizeof *out);
	if (v < 0 || !r || !h->start) return -1;
	t = tex_get(&h->tex, (uint32_t)v);
	n = (uint32_t)t.clut_entries * 4u + t.pixel_bytes;
	if (n > r->slot_bytes) return -1;
	for (i = 0; i < r->slots; i++) {
		if (r->owner[i] != h || r->base[i] != base || r->state[i] == HIRES_SLOT_FREE) continue;
		if (r->state[i] == HIRES_SLOT_FLIGHT) return -1;
		r->state[i] = HIRES_SLOT_HOLD;                          /* ready (or already handed out): the caller uploads it from here */
		r->age[i] = 0;
		out->w = t.width; out->h = t.height; out->psm = t.psm;
		out->clut = r->slot[i]; out->clut_bytes = (uint32_t)t.clut_entries * 4u;
		out->pixels = r->slot[i] + out->clut_bytes; out->pixel_bytes = t.pixel_bytes;
		return 0;
	}
	if (r->flight >= 0 || r->loads_frame >= r->max_loads) { r->refused++; return -1; }
	for (i = 0; i < r->slots; i++) if (r->state[i] == HIRES_SLOT_FREE) { free_slot = (int)i; break; }
	if (free_slot < 0) {                                         /* every slot is waiting or on hold: recycle the oldest unclaimed ready one */
		unsigned best_age = 0;
		for (i = 0; i < r->slots; i++)
			if (r->state[i] == HIRES_SLOT_READY && r->age[i] >= best_age) { best_age = r->age[i]; free_slot = (int)i; }
		if (free_slot >= 0) r->stale++;
	}
	if (free_slot < 0) { r->refused++; return -1; }
	if (h->start(h->aio, h->tex_base + t.clut_offset, r->slot[free_slot], n) != 0) { r->failed++; return -1; }
	r->state[free_slot] = HIRES_SLOT_FLIGHT;
	r->owner[free_slot] = h; r->base[free_slot] = (uint16_t)base; r->age[free_slot] = 0;
	r->flight = free_slot;
	r->loads_frame++; r->loads++; r->bytes_read += n;
	return -1;
}
