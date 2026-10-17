#include "sim/save.h"
#include <string.h>

void checkpoint_save(checkpoint_t *c, const game_t *g, const fpview_t *fp, const fx_t *fx)
{
	c->game = *g;
	c->objects = *g->objects;
	c->fp = *fp;
	c->has_fx = fx != NULL;
	if (fx) c->fx = *fx;
	c->tick = g->tick;
	c->valid = 1;
}

/* the pointers of game_t that belong to the live session */
typedef struct {
	const unitdef_section *unit; const weap_section *weap; const cmod_section *cmod; const ansm_section *ansm; const fxdef_section *fx; const coll_bsp *coll;
	object_world_t ow; object_table_t *objects; fpview_t *fp; phys_world phys; int has_phys;
} live_ptrs;

static void keep(live_ptrs *p, const game_t *g)
{
	p->unit = g->unit; p->weap = g->weap; p->cmod = g->cmod; p->ansm = g->ansm; p->fx = g->fx; p->coll = g->coll;
	p->ow = g->ow; p->objects = g->objects; p->fp = g->fp; p->phys = g->phys; p->has_phys = g->has_phys;
}

static void restore(const live_ptrs *p, game_t *g)
{
	g->unit = p->unit; g->weap = p->weap; g->cmod = p->cmod; g->ansm = p->ansm; g->fx = p->fx; g->coll = p->coll;
	g->ow = p->ow; g->objects = p->objects; g->fp = p->fp; g->phys = p->phys; g->has_phys = p->has_phys;
}

int checkpoint_revert(const checkpoint_t *c, game_t *g, object_table_t *objects, fpview_t *fp, fx_t *fx)
{
	live_ptrs lp;
	const fxdef_section *fxdef = fx ? fx->def : NULL;
	if (!c->valid) return 0;
	keep(&lp, g);
	*g = c->game;
	restore(&lp, g);                          /* the snapshot may come from another run: use this session's pointers */
	*objects = c->objects;
	*fp = c->fp;
	if (fx && c->has_fx) { *fx = c->fx; fx->def = fxdef; }
	g->objects = objects; g->fp = fp;
	g->ev_count = 0;
	return 1;
}

/* ---- relocatable image ---------------------------------------------------------------------------------------------- */
#define PACK_MAGIC 0x504B4348u        /* 'HCKP' */
#define PACK_VERSION 1u

typedef struct { uint32_t magic, version, size, game_size, objects_size, fp_size, fx_size, payload, checksum; } pack_header;

static uint32_t checksum(const uint8_t *p, size_t n)
{
	uint32_t a = 1, b = 0;
	size_t i;
	for (i = 0; i < n; i++) { a = (a + p[i]) % 65521u; b = (b + a) % 65521u; }
	return b << 16 | a;
}

/* zero-run coding: u16 literal length, literal bytes, u16 zero-run length; repeated until the input ends */
static size_t rle_encode(const uint8_t *in, size_t n, uint8_t *out, size_t cap)
{
	size_t i = 0, o = 0;
	while (i < n) {
		size_t lit = 0, zr = 0;
		while (i + lit < n && lit < 0xFFFF) {                    /* literal until a zero run of >= 6 starts */
			if (in[i + lit] == 0) {
				size_t z = 0;
				while (i + lit + z < n && in[i + lit + z] == 0 && z < 6) z++;
				if (z >= 6) break;
			}
			lit++;
		}
		while (i + lit + zr < n && in[i + lit + zr] == 0 && zr < 0xFFFF) zr++;
		if (o + 4 + lit > cap) return 0;
		out[o++] = (uint8_t)lit; out[o++] = (uint8_t)(lit >> 8);
		memcpy(out + o, in + i, lit); o += lit;
		out[o++] = (uint8_t)zr; out[o++] = (uint8_t)(zr >> 8);
		i += lit + zr;
	}
	return o;
}

static int rle_decode(const uint8_t *in, size_t n, uint8_t *out, size_t out_size)
{
	size_t i = 0, o = 0;
	while (i < n) {
		size_t lit, zr;
		if (i + 2 > n) return 0;
		lit = (size_t)in[i] | (size_t)in[i + 1] << 8; i += 2;
		if (i + lit + 2 > n || o + lit > out_size) return 0;
		memcpy(out + o, in + i, lit); i += lit; o += lit;
		zr = (size_t)in[i] | (size_t)in[i + 1] << 8; i += 2;
		if (o + zr > out_size) return 0;
		memset(out + o, 0, zr); o += zr;
	}
	return o == out_size;
}

size_t checkpoint_pack_max(void) { return sizeof(pack_header) + sizeof(checkpoint_t) + sizeof(checkpoint_t) / 64u + 64u; }

size_t checkpoint_pack(const checkpoint_t *c, uint8_t *out, size_t cap)
{
	pack_header h;
	size_t n;
	if (cap < sizeof h) return 0;
	n = rle_encode((const uint8_t *)c, sizeof *c, out + sizeof h, cap - sizeof h);
	if (!n) return 0;
	h.magic = PACK_MAGIC; h.version = PACK_VERSION; h.size = (uint32_t)sizeof *c;
	h.game_size = (uint32_t)sizeof c->game; h.objects_size = (uint32_t)sizeof c->objects; h.fp_size = (uint32_t)sizeof c->fp; h.fx_size = (uint32_t)sizeof c->fx;
	h.payload = (uint32_t)n;
	h.checksum = checksum(out + sizeof h, n);
	memcpy(out, &h, sizeof h);
	return sizeof h + n;
}

int checkpoint_unpack(checkpoint_t *s, const uint8_t *in, size_t n)
{
	pack_header h;
	if (n < sizeof h) return 0;
	memcpy(&h, in, sizeof h);
	if (h.magic != PACK_MAGIC || h.version != PACK_VERSION || h.size != sizeof *s || h.game_size != sizeof s->game || h.objects_size != sizeof s->objects ||
	    h.fp_size != sizeof s->fp || h.fx_size != sizeof s->fx || sizeof h + (size_t)h.payload > n) return 0;
	if (checksum(in + sizeof h, h.payload) != h.checksum) return 0;
	if (!rle_decode(in + sizeof h, h.payload, (uint8_t *)s, sizeof *s)) { s->valid = 0; return 0; }
	return s->valid == 1;
}

/* ---- generic blobs ---- */
typedef struct { uint32_t magic, version, size, payload, checksum; } blob_header;

size_t blob_pack_max(size_t n) { return sizeof(blob_header) + n + n / 64u + 64u; }

size_t blob_pack(const void *src, size_t n, uint32_t magic, uint8_t *out, size_t cap)
{
	blob_header h;
	size_t k;
	if (cap < sizeof h) return 0;
	k = rle_encode((const uint8_t *)src, n, out + sizeof h, cap - sizeof h);
	if (!k) return 0;
	h.magic = magic; h.version = 1; h.size = (uint32_t)n; h.payload = (uint32_t)k; h.checksum = checksum(out + sizeof h, k);
	memcpy(out, &h, sizeof h);
	return sizeof h + k;
}

int blob_unpack(void *dst, size_t n, uint32_t magic, const uint8_t *in, size_t in_n)
{
	blob_header h;
	if (in_n < sizeof h) return 0;
	memcpy(&h, in, sizeof h);
	if (h.magic != magic || h.version != 1 || h.size != n || sizeof h + (size_t)h.payload > in_n) return 0;
	if (checksum(in + sizeof h, h.payload) != h.checksum) return 0;
	return rle_decode(in + sizeof h, h.payload, (uint8_t *)dst, n);
}
