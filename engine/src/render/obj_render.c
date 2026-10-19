#include "render/obj_render.h"
#include "render/gs.h"
#include "platform/timer.h"
#include <kernel.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#define VU_MODEL_BASE 136
#define VU_MODEL_PALETTE 12

static float s_pal[SKEL_MAX_NODES][5][4];
static unsigned s_pal_serial, s_last_serial;          /* which draw_model call filled s_pal / uploaded the last palette */
static uint8_t  s_last_palette[MODEL_MAX_PALETTE];
static unsigned s_last_count;
static object_nodes_t s_nodes;

static u32 f2u(float f) { u32 u; memcpy(&u, &f, 4); return u; }

static void emit_model_setup(chain_t *c, const render_ctx_t *rc)
{
	float fk, fb;
	u32 *w = chain_cnt(c, 1 + (1 + 2) + (1 + 3) + (1 + 1), rb_take_pending());
	float *f;
	if (!w) return;
	rb_fog_params(&fk, &fb);
	/* qw0: wait for VU1 idle, move the double buffer to the model layout */
	w[0] = VIF_CODE(VIF_FLUSHA, 0, 0); w[1] = VIF_CODE(VIF_BASE, 0, VU_MODEL_BASE);
	w[2] = VIF_CODE(VIF_OFFSET, 0, BSP_VU_QWORDS); w[3] = VIF_CODE(VIF_NOP, 0, 0);
	w += 4;
	w[0] = VIF_CODE(VIF_NOP, 0, 0); w[1] = VIF_CODE(VIF_NOP, 0, 0); w[2] = VIF_CODE(VIF_NOP, 0, 0);
	w[3] = VIF_CODE(VIF_UNPACK_V4_32, 2, 0);                                   /* abs 0, 1: screen scale, offset */
	f = (float *)(w + 4);
	f[0] = (float)VIDEO_WIDTH * 0.5f; f[1] = (float)VIDEO_HEIGHT * 0.5f; f[2] = 16777215.0f; f[3] = 1.0f;
	f[4] = 2048.0f; f[5] = 2048.0f; f[6] = 0.0f; f[7] = 0.0f;
	w += 12;
	w[0] = VIF_CODE(VIF_NOP, 0, 0); w[1] = VIF_CODE(VIF_NOP, 0, 0); w[2] = VIF_CODE(VIF_NOP, 0, 0);
	w[3] = VIF_CODE(VIF_UNPACK_V4_32, 3, 4);                                   /* abs 4..6: fog, kS, kB */
	f = (float *)(w + 4);
	f[0] = fk; f[1] = fb; f[2] = 0.0f; f[3] = 4080.0f;
	f[4] = 1.0f / CLIP_GUARD; f[5] = 1.0f / CLIP_GUARD; f[6] = 2.0f; f[7] = 0.0f;
	f[8] = 0.0f; f[9] = 0.0f; f[10] = -1.0f; f[11] = 0.0f;
	w += 16;
	w[0] = VIF_CODE(VIF_NOP, 0, 0); w[1] = VIF_CODE(VIF_NOP, 0, 0); w[2] = VIF_CODE(VIF_NOP, 0, 0);
	w[3] = VIF_CODE(VIF_UNPACK_V4_32, 1, 9);                                   /* abs 9: 1/127, 1/32767, 255, 128 */
	f = (float *)(w + 4);
	f[0] = 1.0f / 127.0f; f[1] = 1.0f / 32767.0f; f[2] = 255.0f; f[3] = 128.0f;
	(void)rc;
}

static void emit_light(chain_t *c, const scen_light *l)
{
	u32 *w = chain_cnt(c, 1 + 2, rb_take_pending());
	float *f;
	if (!w) return;
	w[0] = VIF_CODE(VIF_NOP, 0, 0); w[1] = VIF_CODE(VIF_NOP, 0, 0); w[2] = VIF_CODE(VIF_FLUSHA, 0, 0);
	w[3] = VIF_CODE(VIF_UNPACK_V4_32, 2, 7);
	f = (float *)(w + 4);
	f[0] = (float)l->ambient[0]; f[1] = (float)l->ambient[1]; f[2] = (float)l->ambient[2]; f[3] = 0.0f;
	f[4] = (float)l->sun[0]; f[5] = (float)l->sun[1]; f[6] = (float)l->sun[2]; f[7] = 0.0f;
}

/* Part state: A+D registers through PATH2 (FLUSHA first) and the uv scale / GIF tag template (abs 2, 3). */
static void emit_part_state(chain_t *c, const gs_ad *ad, unsigned n, float su, float sv, u64 prim)
{
	u32 *w = chain_cnt(c, 1 + 1 + n + 1 + 2, rb_take_pending());
	u64 *q;
	float *f;
	unsigned i;
	if (!w) return;
	w[0] = VIF_CODE(VIF_NOP, 0, 0); w[1] = VIF_CODE(VIF_NOP, 0, 0);
	w[2] = VIF_CODE(VIF_FLUSHA, 0, 0); w[3] = VIF_CODE(VIF_DIRECT, 0, 1 + n);
	q = (u64 *)(w + 4);
	q[0] = GIF_TAG(n, 1, 0, 0, GIFX_FLG_PACKED, 1); q[1] = GIF_REGS_AD;
	for (i = 0; i < n; i++) { q[2 + 2 * i] = ad[i].data; q[3 + 2 * i] = ad[i].addr; }
	w = (u32 *)(q + 2 + 2 * n);
	w[0] = VIF_CODE(VIF_NOP, 0, 0); w[1] = VIF_CODE(VIF_NOP, 0, 0); w[2] = VIF_CODE(VIF_NOP, 0, 0);
	w[3] = VIF_CODE(VIF_UNPACK_V4_32, 2, 2);
	f = (float *)(w + 4);
	f[0] = su; f[1] = sv; f[2] = 0.0f; f[3] = 0.0f;
	q = (u64 *)(f + 4);
	q[0] = GIF_TAG(0, 0, 1, prim, GIFX_FLG_PACKED, 3);
	q[1] = 0x412;
	(void)f2u;
}

/* GS state only (A+D through PATH2), nothing written to VU1 memory. The sky pass runs between the BSP frame
 * header and the BSP batches, so it must not touch VU1 data memory: emit_part_state's UNPACK lands at absolute
 * addresses 2..3 (the model layout's uv scale and GIF tag), which hold rows 2..3 of the BSP program's clip matrix
 * (docs/03 §5.2, ADR-057). The sky pass packs its own GIF tags (sky_emit_polys) and needs neither word. */
static void emit_gs_state(chain_t *c, const gs_ad *ad, unsigned n)
{
	u32 *w = chain_cnt(c, 1 + 1 + n, rb_take_pending());
	u64 *q;
	unsigned i;
	if (!w) return;
	w[0] = VIF_CODE(VIF_NOP, 0, 0); w[1] = VIF_CODE(VIF_NOP, 0, 0);
	w[2] = VIF_CODE(VIF_FLUSHA, 0, 0); w[3] = VIF_CODE(VIF_DIRECT, 0, 1 + n);
	q = (u64 *)(w + 4);
	q[0] = GIF_TAG(n, 1, 0, 0, GIFX_FLG_PACKED, 1); q[1] = GIF_REGS_AD;
	for (i = 0; i < n; i++) { q[2 + 2 * i] = ad[i].data; q[3 + 2 * i] = ad[i].addr; }
}

/* One batch: its data (a REF into the section, UNPACKed into the idle buffer while the previous batch runs), then
 * FLUSHE (the previous program must be done with the palette), the palette of the nodes it uses, and MSCAL. */
static void emit_batch(chain_t *c, const render_ctx_t *rc, const model_section *ms, const model_batch *b)
{
	unsigned pc = b->palette_count, s;
	u32 *w;
	(void)rc;
	if (s_last_serial == s_pal_serial && s_last_count == pc && memcmp(s_last_palette, b->palette, pc) == 0) {
		/* the VU1 memory already holds this palette (same object, same nodes): just queue the batch; MSCAL rides on
		 * the next tag and the VIF stalls it until the previous program has finished */
		chain_ref(c, model_batch_vif(ms, b), b->vif_qwords, rb_take_pending());
		rb_set_pending(VIF_PAIR(VIF_CODE(VIF_MSCAL, 0, rb_model_entry()), 0));
		return;
	}
	chain_ref(c, model_batch_vif(ms, b), b->vif_qwords, rb_take_pending());
	w = chain_cnt(c, 1 + 5 * pc + 1, 0);
	if (!w) return;
	w[0] = VIF_CODE(VIF_FLUSHE, 0, 0); w[1] = VIF_CODE(VIF_NOP, 0, 0); w[2] = VIF_CODE(VIF_NOP, 0, 0);
	w[3] = VIF_CODE(VIF_UNPACK_V4_32, 5 * pc, VU_MODEL_PALETTE);
	for (s = 0; s < pc; s++) memcpy(w + 4 + 20 * s, s_pal[b->palette[s] < SKEL_MAX_NODES ? b->palette[s] : 0], 80);
	w += 4 + 20 * pc;
	w[0] = VIF_CODE(VIF_MSCAL, 0, rb_model_entry()); w[1] = VIF_CODE(VIF_NOP, 0, 0);
	w[2] = VIF_CODE(VIF_NOP, 0, 0); w[3] = VIF_CODE(VIF_NOP, 0, 0);
	memcpy(s_last_palette, b->palette, pc);
	s_last_count = pc;
	s_last_serial = s_pal_serial;
}

static unsigned pick_lod(const model_section *ms, const model_info *mi, const uint16_t *perm, unsigned regions, float diameter_px, int bias)
{
	int lod = (int)model_pick_lod(mi, diameter_px) + bias, l;
	if (lod >= MODEL_LODS) lod = MODEL_LODS - 1;
	if (lod < 0) lod = 0;
	for (l = lod; l < MODEL_LODS; l++) {            /* the nearest existing LOD at or below this detail, else above */
		unsigned r, parts = 0;
		for (r = 0; r < regions; r++) parts += model_parts(ms, mi, perm[r], (uint32_t)l).part_count;
		if (parts) return (unsigned)l;
	}
	for (l = lod - 1; l >= 0; l--) {
		unsigned r, parts = 0;
		for (r = 0; r < regions; r++) parts += model_parts(ms, mi, perm[r], (uint32_t)l).part_count;
		if (parts) return (unsigned)l;
	}
	return (unsigned)lod;
}

/* Draw one model with the given node matrices. Returns the number of batches emitted. */
/* Change colours of an object: tint[c] (rgb 0..255) is valid when has[c] (c = 0..3 for change colour a..d). */
typedef struct { uint8_t rgb[CCHG_MAX_CHANGE][3]; uint8_t has[CCHG_MAX_CHANGE]; } cc_tints;

static void cc_pick(const obj_render_t *o, int model, uint32_t seed, cc_tints *t, int obj)
{
	const uint8_t *rec;
	unsigned c;
	memset(t, 0, sizeof *t);
	if (!o->cchg || !o->world.models || model < 0) return;
	rec = cchg_find(o->cchg, model_get(o->world.models, (uint32_t)model).tag_id);
	if (!rec) return;
	for (c = 0; c < CCHG_MAX_CHANGE; c++) {
		if (c == 0 && obj >= 0 && obj < OBJ_MAX && o->cc_perm[obj] != 0xFFu) t->has[c] = (uint8_t)cchg_tint_perm(rec, 1, o->cc_perm[obj], seed, t->rgb[c]);     /* picked from the Xbox video (cfg ccperm) */
		else t->has[c] = (uint8_t)cchg_tint(rec, c + 1, seed, t->rgb[c]);
	}
}

/* Does any part of the model at this lod draw blended (alpha / additive, not alpha tested)? Decides whether the object gets the second, far-to-near pass. */
static int model_has_blend(const obj_render_t *o, int model, const uint16_t *perm, unsigned regions, unsigned lod)
{
	const model_section *ms = o->world.models;
	model_info mi = model_get(ms, (uint32_t)model);
	unsigned r, p;
	for (r = 0; r < regions; r++) {
		model_part_range pr = model_parts(ms, &mi, perm[r], lod);
		for (p = 0; p < pr.part_count; p++) {
			model_part part = model_get_part(ms, (uint32_t)pr.first_part + p);
			mat_binding_t mb = { -1, 0, 0, { 1.0f, 1.0f }, 0, -1 };
			if (o->textures && o->textures->binding && o->textures->binding(o->textures->user, part.shader_id, &mb) != 0) mb.tex = -1;
			if (((mb.flags & MAT_BLEND) != 0 || (part.flags & 2)) && !((mb.flags & MAT_ALPHA_TEST) != 0 || (part.flags & 1))) return 1;
		}
	}
	return 0;
}

/* GS TEX0 of the streamed high resolution variant of object texture `tex`, or 0 (none, not yet loaded, over this frame's load budget): the caller falls back to the resident one. */
/* A variant whose upload failed (the GS texture cache had no room) is not read from the disk again for HIRES_RETRY frames: without this the bay shot at tick ~1200 re-read a 129 KB
 * variant every frame (ADR-077: the object pass cost 20 ms instead of 4 and the intro dropped 10 % of its frames). */
#define HIRES_RETRY 90u
#define HIRES_RETRY_MAX 512u
static uint32_t s_hires_retry[HIRES_RETRY_MAX];

static u64 hires_tex0(obj_render_t *o, int tex)
{
	hires_view v;
	tex_image_t img;
	int was_resident;
	u64 t0;
	if (!o->hires || tex < 0 || hires_info(o->hires, (unsigned)tex, &v) != 0) return 0;
	was_resident = rb_extra_resident(o->hires_key_base, (unsigned)tex);
	if (!was_resident) {
		if ((unsigned)tex < HIRES_RETRY_MAX && o->frame_no < s_hires_retry[tex]) return 0;
		if (hires_load(o->hires, (unsigned)tex, &v) != 0) return 0;
		SyncDCache((void *)v.clut, (u8 *)v.pixels + v.pixel_bytes);       /* the DMA reads RAM, not the cache */
	}
	img.pixels = v.pixels; img.clut = v.clut; img.pixel_bytes = v.pixel_bytes; img.clut_bytes = v.clut_bytes; img.w = v.w; img.h = v.h; img.psm = v.psm;
	t0 = rb_extra_tex0(o->hires_key_base, (unsigned)tex, &img, 0);
	if (!t0 && !was_resident && (unsigned)tex < HIRES_RETRY_MAX) s_hires_retry[tex] = o->frame_no + HIRES_RETRY;
	return o->want_hi == 2 ? 0 : t0;
}


/* ---- EE clipping of the triangles the model program drops (ADR-078) --------------------------------------------
 * model.vsm collapses every triangle with a vertex nearer than the near plane or beyond the guard band. For people that is invisible, but the intro's
 * extreme close-up of the captain puts his nose 0.03 from the lens (near 0.03): the Xbox clips those triangles, here they were dropped, leaving holes in the
 * face. A batch whose posed bounding sphere comes near the near plane or the band is decoded on the EE (positions, two-node skinning, lighting, uv), the triangles
 * the VU1 would drop are clipped with the BSP's clipper and drawn through PATH2 right after the batch. */
#define EE_CLIP_ZMAX 16777215.0f
#define EE_CLIP_MAX_VERTS 1200u

/* Can any vertex of this batch leave the guard band or cross the near plane? Conservative: the batch's box sphere under the palette nodes its vertices actually use
 * (an integer scan of the two slots of every vertex; testing the sphere under every node of the palette flags nearly every batch of a character). */
static int batch_unsafe(const model_section *ms, const model_batch *b, float near_z)
{
	const uint8_t *d = (const uint8_t *)model_batch_vif(ms, b);
	unsigned n = 3u * b->triangles, v, s;
	float ctr[3], scale, rr;
	uint32_t used = 0;
	const uint8_t *pos = d + 40, *uv = d + 44 + 8u * n;
	memcpy(ctr, d + 4, 12); memcpy(&scale, d + 16, 4);
	rr = scale * 32767.0f * 1.7321f;
	for (v = 0; v < n; v++) used |= 1u << ((unsigned)((const int16_t *)(pos + 8u * v))[3] & 31u) | 1u << ((unsigned)((const int16_t *)(uv + 8u * v))[2] & 31u);
	for (s = 0; s < b->palette_count; s++) {
		unsigned node;
		const float (*m)[4];
		float cx, cy, cw, gx, gy, gw, wmin, lim = CLIP_GUARD * 0.97f;
		if (!(used & (1u << s))) continue;
		node = b->palette[s] < SKEL_MAX_NODES ? b->palette[s] : 0;
		m = s_pal[node];
		cx = ctr[0] * m[0][0] + ctr[1] * m[1][0] + ctr[2] * m[2][0] + m[3][0]; cy = ctr[0] * m[0][1] + ctr[1] * m[1][1] + ctr[2] * m[2][1] + m[3][1];
		cw = ctr[0] * m[0][3] + ctr[1] * m[1][3] + ctr[2] * m[2][3] + m[3][3];
		gx = sqrtf(m[0][0] * m[0][0] + m[1][0] * m[1][0] + m[2][0] * m[2][0]); gy = sqrtf(m[0][1] * m[0][1] + m[1][1] * m[1][1] + m[2][1] * m[2][1]);
		gw = sqrtf(m[0][3] * m[0][3] + m[1][3] * m[1][3] + m[2][3] * m[2][3]);
		wmin = cw - rr * gw;
		if (wmin < near_z * 1.02f) return 1;
		if (fabsf(cx) + rr * gx > lim * wmin || fabsf(cy) + rr * gy > lim * wmin) return 1;
	}
	return 0;
}

static void emit_batch_clipped(obj_render_t *o, chain_t *c, const render_ctx_t *rc, const model_section *ms, const model_batch *b, const scen_light *light, float su, float sv, u64 prim)
{
	const uint8_t *d = (const uint8_t *)model_batch_vif(ms, b);
	unsigned n = 3u * b->triangles, t, v, k, nv = 0, i;
	float ctr[3], scale;
	const uint8_t *pos = d + 40, *uv = d + 44 + 8u * n, *nrm = d + 48 + 16u * n;
	static cvert poly[EE_CLIP_MAX_VERTS];
	static uint8_t cnt[24 * 10];
	unsigned npoly = 0;
	clip_gs g;
	u32 *w;
	u64 *q;
	float fk, fb, near_z = rc->cam->near_z;
	u32 t_start = timer_cycles();
	memcpy(ctr, d + 4, 12); memcpy(&scale, d + 16, 4);
	rb_fog_params(&fk, &fb);
	g.near_z = near_z; g.zmax = EE_CLIP_ZMAX; g.sx = (float)VIDEO_WIDTH * 0.5f; g.sy = (float)VIDEO_HEIGHT * 0.5f; g.ox = 2048.0f; g.oy = 2048.0f; g.fog_k = fk; g.fog_b = fb;
	for (t = 0; t < b->triangles && npoly < sizeof cnt; t++) {
		cvert tri[3], out[CLIP_MAX_VERTS];
		int m;
		for (v = 0; v < 3; v++) {                              /* phase 1: the clip position only (two node images blended, one for a rigid vertex) */
			const int16_t *p = (const int16_t *)(pos + 8u * (3u * t + v)), *qq = (const int16_t *)(uv + 8u * (3u * t + v));
			float x = ctr[0] + (float)p[0] * scale, y = ctr[1] + (float)p[1] * scale, z = ctr[2] + (float)p[2] * scale, w0 = (float)qq[3] / 32767.0f;
			unsigned s0 = (unsigned)(uint16_t)p[3], s1 = (unsigned)(uint16_t)qq[2];
			unsigned n0 = (s0 < b->palette_count && b->palette[s0] < SKEL_MAX_NODES) ? b->palette[s0] : 0, n1 = (s1 < b->palette_count && b->palette[s1] < SKEL_MAX_NODES) ? b->palette[s1] : 0;
			for (k = 0; k < 4; k++) {
				float c0 = x * s_pal[n0][0][k] + y * s_pal[n0][1][k] + z * s_pal[n0][2][k] + s_pal[n0][3][k];
				if (w0 < 0.9999f && n1 != n0) {
					float c1 = x * s_pal[n1][0][k] + y * s_pal[n1][1][k] + z * s_pal[n1][2][k] + s_pal[n1][3][k];
					c0 = c1 + (c0 - c1) * w0;
				}
				tri[v].c[k] = c0;
			}
		}
		if (!clip_needs(tri, near_z, 0.985f) || clip_outside(tri, near_z)) continue;
		for (v = 0; v < 3; v++) {                              /* phase 2, only for a triangle that needs clipping: colour and uv */
			const int16_t *p = (const int16_t *)(pos + 8u * (3u * t + v)), *qq = (const int16_t *)(uv + 8u * (3u * t + v));
			const int8_t *nn = (const int8_t *)(nrm + 4u * (3u * t + v));
			unsigned s0 = (unsigned)(uint16_t)p[3];
			unsigned n0 = (s0 < b->palette_count && b->palette[s0] < SKEL_MAX_NODES) ? b->palette[s0] : 0;
			float ndl = ((float)nn[0] * s_pal[n0][4][0] + (float)nn[1] * s_pal[n0][4][1] + (float)nn[2] * s_pal[n0][4][2]) / 127.0f, col;
			if (ndl < 0.0f) ndl = 0.0f;
			for (k = 0; k < 3; k++) { col = (float)light->ambient[k] + (float)light->sun[k] * ndl; tri[v].a[4 + k] = col > 255.0f ? 255.0f : col; }
			tri[v].a[7] = 128.0f;
			tri[v].a[0] = (float)qq[0] * su; tri[v].a[1] = (float)qq[1] * sv; tri[v].a[2] = tri[v].a[3] = 0.0f;
		}
		m = clip_polygon(tri, near_z, out);
		if (m < 3 || nv + 3u * (unsigned)(m - 2) > EE_CLIP_MAX_VERTS) continue;
		for (i = 0; i < (unsigned)m; i++) poly[nv + i] = out[i];
		nv += (unsigned)m; cnt[npoly++] = (uint8_t)m;
	}
	o->clip_ms += timer_ms(t_start, timer_cycles());
	if (!npoly) return;
	{
		unsigned verts = 0, j, base = 0;
		for (i = 0; i < npoly; i++) verts += 3u * (cnt[i] - 2u);
		w = chain_cnt(c, 1 + 1 + 3 * verts, rb_take_pending());
		if (!w) return;
		w[0] = VIF_CODE(VIF_NOP, 0, 0); w[1] = VIF_CODE(VIF_NOP, 0, 0);
		w[2] = VIF_CODE(VIF_NOP, 0, 0); w[3] = VIF_CODE(VIF_DIRECT, 0, 1 + 3 * verts);
		q = (u64 *)(w + 4);
		q[0] = GIF_TAG(verts, 1, 1, prim, GIFX_FLG_PACKED, 3); q[1] = 0x412;
		w = (u32 *)(q + 2);
		for (i = 0; i < npoly; i++) {
			const cvert *pv = &poly[base];
			for (j = 1; j + 1 < cnt[i]; j++) {
				clip_pack_vertex(&pv[0], 0, &g, (uint32_t *)w); w += 12;
				clip_pack_vertex(&pv[j], 0, &g, (uint32_t *)w); w += 12;
				clip_pack_vertex(&pv[j + 1], 0, &g, (uint32_t *)w); w += 12;
			}
			base += cnt[i];
		}
	}
}

static unsigned draw_model(obj_render_t *o, const render_ctx_t *rc, int model, const uint16_t *perm, unsigned regions,
                           const object_nodes_t *nodes, unsigned lod, const scen_light *light, unsigned budget, const cc_tints *cc, int pass)
{
	const model_section *ms = o->world.models;
	model_info mi = model_get(ms, (uint32_t)model);
	unsigned i, r, p, bi, emitted = 0;
	int unlit = 0;
	if (nodes->count == 0 || budget == 0) return 0;
	for (i = 0; i < nodes->count; i++) skel_palette_entry(s_pal[i], &nodes->skin[i], rc->clip, light->sun_dir);
	s_pal_serial++;                                   /* new palette contents: the VU1 copy is stale */
	emit_light(rc->chain, light);
	for (r = 0; r < regions; r++) {
		model_part_range pr = model_parts(ms, &mi, perm[r], lod);
		for (p = 0; p < pr.part_count; p++) {
			model_part part = model_get_part(ms, (uint32_t)pr.first_part + p);
			mat_binding_t mb = { -1, 0, 0, { 1.0f, 1.0f }, 0, -1 };
			gs_ad ad[7];
			unsigned n = 0;
			u64 tex0 = 0;
			int blend, atest;
			if (emitted + part.batch_count > budget) { o->st_skipped++; continue; }
			if (o->textures && o->textures->binding && o->textures->binding(o->textures->user, part.shader_id, &mb) != 0) mb.tex = -1;
			blend = (mb.flags & MAT_BLEND) != 0 || (part.flags & 2);
			atest = (mb.flags & MAT_ALPHA_TEST) != 0 || (part.flags & 1);
			if ((pass == 0 && blend && !atest) || (pass == 1 && !(blend && !atest))) continue;
			if (mb.add != unlit && o->unlit_add) {              /* an additive part (a hologram, a glow) is self lit: the Xbox does not light it, so it gets full white vertex light, the lit parts their light back */
				scen_light full = *light;
				if (mb.add) { full.ambient[0] = full.ambient[1] = full.ambient[2] = 128; full.sun[0] = full.sun[1] = full.sun[2] = 0; emit_light(rc->chain, &full); }
				else emit_light(rc->chain, light);
				unlit = mb.add;
			}     /* pass 0: opaque and alpha tested parts, pass 1: blended parts (no Z write, far to near) */
			if (rc->opt->textures && mb.tex >= 0 && o->textures && o->textures->texture) {
				tex_image_t img;
				if (o->textures->texture(o->textures->user, mb.tex, &img) == 0) {
					if (o->want_hi) tex0 = hires_tex0(o, mb.tex);
					if (!tex0) tex0 = rb_object_tex0((unsigned)mb.tex, &img, 0);
				}
			}
			if (tex0) {
				ad[n].data = tex0;                          ad[n++].addr = GS_A_TEX0_1;
				ad[n].data = GS_TEX1(1, 0, 1, 1, 0, 0, 0);  ad[n++].addr = GS_A_TEX1_1;
				ad[n].data = GS_CLAMP(0, 0, 0, 0, 0, 0);    ad[n++].addr = GS_A_CLAMP_1;
			}
			ad[n].data = mb.add ? GS_ALPHA(0, 2, 2, 1, 0x80) : GS_ALPHA(0, 1, 0, 1, 0); ad[n++].addr = GS_A_ALPHA_1;
			ad[n].data = GS_TEST(atest && tex0 ? 1 : 0, 6, 0x40, 0, 0, 0, 1, 2); ad[n++].addr = GS_A_TEST_1;
			ad[n].data = GS_ZBUF(*rb_zbuf_page_ptr(), GS_PSMZ24, blend && !atest ? 1 : 0); ad[n++].addr = GS_A_ZBUF_1;     /* a blended part never writes Z: it would hide what is drawn after it */
			emit_part_state(rc->chain, ad, n, mb.uv_scale[0] / (float)(1u << part.uv_shift), mb.uv_scale[1] / (float)(1u << part.uv_shift),
			                GS_PRIM(GSX_PRIM_TRIANGLE, 1, tex0 ? 1 : 0, rc->opt->fog, blend, 0, 0, 0, 0));
			for (bi = 0; bi < part.batch_count; bi++) {
				model_batch b = model_get_batch(ms, (uint32_t)part.first_batch + bi);
				emit_batch(rc->chain, rc, ms, &b);
				if (o->ee_clip && o->ee_clip_obj && o->clip_ms < o->clip_budget_ms && batch_unsafe(ms, &b, rc->cam->near_z))
					emit_batch_clipped(o, rc->chain, rc, ms, &b, light, mb.uv_scale[0] / (float)(1u << part.uv_shift), mb.uv_scale[1] / (float)(1u << part.uv_shift),
					                   GS_PRIM(GSX_PRIM_TRIANGLE, 1, tex0 ? 1 : 0, rc->opt->fog, blend, 0, 0, 0, 0));
				o->st_triangles += b.triangles;
			}
			emitted += part.batch_count;
			if (mb.cc && mb.tex2 >= 0 && cc && mb.cc <= CCHG_MAX_CHANGE && cc->has[mb.cc - 1] && rc->opt->textures && o->textures && o->textures->texture
			    && emitted + part.batch_count <= budget) {
				/* colour change: the part again, its base map with the mask as alpha, alpha blended over the first pass (same depth) and lit with the
				 * object's change colour: where the mask is 1 the result is base x tint x light, elsewhere the first pass stays */
				tex_image_t img2;
				scen_light tl = *light;
				unsigned k;
				if (o->textures->texture(o->textures->user, mb.tex2, &img2) == 0) {
					for (k = 0; k < 3; k++) {
						tl.ambient[k] = (uint8_t)((unsigned)light->ambient[k] * cc->rgb[mb.cc - 1][k] / 255u);
						tl.sun[k] = (uint8_t)((unsigned)light->sun[k] * cc->rgb[mb.cc - 1][k] / 255u);
					}
					emit_light(rc->chain, &tl);
					n = 0;
					ad[n].data = rb_object_tex0((unsigned)mb.tex2, &img2, 0);   ad[n++].addr = GS_A_TEX0_1;
					ad[n].data = GS_TEX1(1, 0, 1, 1, 0, 0, 0);  ad[n++].addr = GS_A_TEX1_1;
					ad[n].data = GS_CLAMP(0, 0, 0, 0, 0, 0);    ad[n++].addr = GS_A_CLAMP_1;
					ad[n].data = GS_ALPHA(0, 1, 0, 1, 0);       ad[n++].addr = GS_A_ALPHA_1;
					ad[n].data = GS_TEST(0, 6, 0x40, 0, 0, 0, 1, 2); ad[n++].addr = GS_A_TEST_1;
					ad[n].data = GS_ZBUF(*rb_zbuf_page_ptr(), GS_PSMZ24, 0); ad[n++].addr = GS_A_ZBUF_1;
					emit_part_state(rc->chain, ad, n, mb.uv_scale[0] / (float)(1u << part.uv_shift), mb.uv_scale[1] / (float)(1u << part.uv_shift),
					                GS_PRIM(GSX_PRIM_TRIANGLE, 1, 1, rc->opt->fog, 1, 0, 0, 0, 0));
					for (bi = 0; bi < part.batch_count; bi++) {
						model_batch b2 = model_get_batch(ms, (uint32_t)part.first_batch + bi);
						emit_batch(rc->chain, rc, ms, &b2);
						o->st_triangles += b2.triangles;
					}
					emitted += part.batch_count;
					emit_light(rc->chain, light);
				}
			}
		}
	}
	if (unlit) emit_light(rc->chain, light);
	o->st_batches += emitted;
	return emitted;
}

static void light_at(const obj_render_t *o, const float pos[3], scen_light *l)
{
	int cl = o->bsp ? bsp_find_cluster(o->bsp, pos) : -1;
	if (cl >= 0 && o->world.scen) scen_cluster_light(o->world.scen, (uint32_t)o->bsp_index, (uint32_t)cl, l);
	else scen_cluster_light(o->world.scen, (uint32_t)o->bsp_index, 0xFFFFu, l);   /* neutral default */
	{                                                         /* the cluster probe averages a lightmap whose hue is stronger than the Xbox's object lighting: pull toward grey (light_chroma 1 = unchanged), then the gain */
		unsigned k;
		float la = 0.30f * l->ambient[0] + 0.59f * l->ambient[1] + 0.11f * l->ambient[2], ls = 0.30f * l->sun[0] + 0.59f * l->sun[1] + 0.11f * l->sun[2];
		for (k = 0; k < 3; k++) {
			float a = (la + ((float)l->ambient[k] - la) * o->light_chroma) * o->light_gain[k] * o->amb_gain, s = (ls + ((float)l->sun[k] - ls) * o->light_chroma) * o->light_gain[k];
			l->ambient[k] = (uint8_t)(a > 255.0f ? 255.0f : a < 0.0f ? 0.0f : a); l->sun[k] = (uint8_t)(s > 255.0f ? 255.0f : s < 0.0f ? 0.0f : s);
		}
	}
	if (o->fx) {                                              /* dynamic point lights of attached fixtures: the red alert of the bridge colours the people in it, the command display turns Keyes teal */
		const fx_dynlight *dl;
		unsigned n = fx_dynamic_lights(o->fx, &dl), i, k;
		for (i = 0; i < n; i++) {
			float dx = pos[0] - dl[i].pos[0], dy = pos[1] - dl[i].pos[1], dz = pos[2] - dl[i].pos[2], d = sqrtf(dx * dx + dy * dy + dz * dz), a, mx;
			if (d >= dl[i].radius) continue;
			a = 1.0f - d / dl[i].radius;
			a *= a;
			mx = dl[i].rgb[0] > dl[i].rgb[1] ? dl[i].rgb[0] : dl[i].rgb[1];
			if (dl[i].rgb[2] > mx) mx = dl[i].rgb[2];
			if (mx < 0.05f) continue;
			{                                                 /* weight of the light's own (normalised) colour in what lights the object near it: a pulsing alarm lamp averages about 0.4 of its colour, a hologram projector is steady */
				float red = (dl[i].rgb[0] - 0.5f * (dl[i].rgb[1] + dl[i].rgb[2])) / mx;
				if (red < 0.0f) red = 0.0f;
				a *= (0.6f - 0.3f * red) * o->dl_gain; if (a > 1.0f) a = 1.0f;
			}
			for (k = 0; k < 3; k++) {
				float c = dl[i].rgb[k] / mx * 255.0f;
				l->ambient[k] = (uint8_t)((float)l->ambient[k] * (1.0f - a) + c * a * 0.7f);
				l->sun[k] = (uint8_t)((float)l->sun[k] * (1.0f - a * 0.7f) + c * a * 0.5f);
			}
		}
	}
}

void obj_render_pass(void *user, render_ctx_t *rc)
{
	obj_render_t *o = user;
	const camera_t *cam = rc->cam;
	float kx = 1.0f / tanf(cam->hfov * 0.5f);
	float maxd = o->max_distance * (o->lod ? lodctl_distance_scale(o->lod) : 1.0f);
	int bias = o->lod ? lodctl_lod_bias(o->lod) : 0;
	unsigned i, budget = o->max_batches;
	float right[3], up[3], fwd[3];
	cc_tints cc;
	u32 t0 = timer_cycles();
	o->st_visible = o->st_culled = o->st_batches = o->st_triangles = o->st_skipped = 0;
	if (!o->world.models || !o->world.scen) return;
	camera_basis(cam, right, up, fwd);
	emit_model_setup(rc->chain, rc);
	if (o->hires) hires_begin_frame(o->hires);
	o->want_hi = 0;
	o->frame_no++;
	o->clip_ms = 0.0f;

	/* Visible objects are drawn nearest first: the batch budget runs out in busy scenes (the bridge sees 80 objects, the hangar's crowd among them) and the
	 * objects that go missing must be the far ones, not the ones with the highest index (Keyes, index 508, vanished in the hologram shot). */
	{
		static uint16_t cand[OBJ_MAX];
		static float    cand_key[OBJ_MAX];
		unsigned n_cand = 0, k, j;
		int pass;
		if (o->lod) budget = lodctl_batch_cap(o->lod, budget);
		for (i = 0; o->draw_objects && o->table && i < o->table->count && i < OBJ_MAX; i++) {
			const object_t *ob = &o->table->obj[i];
			model_info mi;
			float c[3], d[3], dist, w, diameter;
			if (ob->model < 0 || (ob->flags & (OBJ_F_FREE | OBJ_F_INACTIVE | OBJ_F_HIDDEN))) continue;
			mi = model_get(o->world.models, (uint32_t)ob->model);
			mat4_transform_point(&ob->root, mi.centre, c);
			d[0] = c[0] - cam->pos[0]; d[1] = c[1] - cam->pos[1]; d[2] = c[2] - cam->pos[2];
			dist = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
			w = d[0] * fwd[0] + d[1] * fwd[1] + d[2] * fwd[2];
			if (o->trace_hi > o->trace_lo && o->frame_no >= o->trace_lo && o->frame_no < o->trace_hi && (o->frame_no % o->trace_step) == 0 && dist < 3000.0f) {   /* cfg objtrace "first last step": every live object's cull inputs, to chase pop-in */
				float dm = 2.0f * ob->radius * 320.0f * kx / (w > cam->near_z ? w : cam->near_z);
				printf("[obj] f %u #%u def %d model %d type %d r %.2f dist %.1f w %.1f diam %.1f frustum %d\n", o->frame_no, i, (int)ob->def, (int)ob->model, (int)ob->type, (double)ob->radius, (double)dist, (double)w, (double)dm, frustum_sphere(rc->frustum, c, ob->radius));
			}
			if (o->dbg_obj == (int)i) printf("[dbg] obj %u dist %.2f radius %.2f w %.2f maxd %.1f frustum %d\n", i, (double)dist, (double)ob->radius, (double)w, (double)maxd, frustum_sphere(rc->frustum, c, ob->radius));
			if (!frustum_sphere(rc->frustum, c, ob->radius)) { o->st_culled++; continue; }
			diameter = 2.0f * ob->radius * 320.0f * kx / (w > cam->near_z ? w : cam->near_z);
			/* The draw distance only thins out small far objects: a big one (the Pelicans escorting the Pillar of Autumn: radius 12 at 100 .. 570 units, 20 .. 85 px) must not
			 * blink out when its distance crosses `objdist`, and a cinematic camera has no draw distance at all (ADR-066). */
			if (dist - ob->radius > maxd && diameter < o->far_px && !o->hide_fp) { o->st_culled++; continue; }
			if (diameter < 3.0f && dist > ob->radius * 2.0f) { o->st_culled++; continue; }   /* sub-pixel clutter */
			{
				float key = dist - ob->radius;
				for (j = n_cand; j > 0 && cand_key[j - 1] > key; j--) {              /* insertion sort: a few dozen candidates */
					cand_key[j] = cand_key[j - 1];
					cand[j] = cand[j - 1];
				}
				cand_key[j] = key;
			}
			cand[j] = (uint16_t)i;
			n_cand++;
		}
		for (pass = 0; pass < 2; pass++) {
			for (k = 0; k < n_cand; k++) {
				unsigned kk = pass ? n_cand - 1 - k : k;                                   /* the blended pass runs far to near */
				const object_t *ob = &o->table->obj[cand[kk]];
				model_info mi = model_get(o->world.models, (uint32_t)ob->model);
				float c[3], d[3], w, diameter;
				scen_light light;
				unsigned lod, em;
				mat4_transform_point(&ob->root, mi.centre, c);
				d[0] = c[0] - cam->pos[0]; d[1] = c[1] - cam->pos[1]; d[2] = c[2] - cam->pos[2];
				w = d[0] * fwd[0] + d[1] * fwd[1] + d[2] * fwd[2];
				diameter = 2.0f * ob->radius * 320.0f * kx / (w > cam->near_z ? w : cam->near_z);
				lod = pick_lod(o->world.models, &mi, ob->perm, ob->region_count, diameter, bias);
				o->want_hi = o->hires == NULL ? 0 : diameter >= o->hires_px ? 1 : diameter >= 0.5f * o->hires_px ? 2 : 0;      /* 2: approaching, load and upload the variant now, draw the resident one until it counts */
				if (pass && !model_has_blend(o, ob->model, ob->perm, ob->region_count, lod)) continue;
				if ((ob->flags & (OBJ_F_ANIMATED | OBJ_F_POSED)) ? !objects_pose(ob, &o->world, &s_nodes) : !objects_pose_static(ob, &o->world, &s_nodes)) { if (!pass) o->st_culled++; continue; }
				light_at(o, c, &light);
				if (!pass) o->st_visible++;
				cc_pick(o, ob->model, (uint32_t)cand[kk] * 2654435761u + (uint32_t)ob->def * 40503u, &cc, (int)cand[kk]);
				o->ee_clip_obj = (d[0] * d[0] + d[1] * d[1] + d[2] * d[2]) < (2.0f + ob->radius) * (2.0f + ob->radius);       /* only an object within a couple of units of the lens can have a vertex near the near plane or the band */
				em = draw_model(o, rc, ob->model, ob->perm, ob->region_count, &s_nodes, lod, &light, (pass ? budget + budget / 4u : budget) > o->st_batches ? (pass ? budget + budget / 4u : budget) - o->st_batches : 0, &cc, pass);       /* the blended pass has its own quarter on top: glass and holograms are drawn last and must not starve */
				if (o->dbg_obj == (int)cand[kk]) printf("[dbg] obj %u pass %d lod %u emitted %u budget %u used %u\n", (unsigned)cand[kk], pass, lod, em, budget, o->st_batches);
				if (em == 0 && !pass && o->st_batches >= budget) { o->st_skipped++; }
			}
		}
	}

	o->want_hi = 0;
	if (o->draw_fp && !o->hide_fp && o->fp && o->fp->active) {
		const fpview_t *f = o->fp;
		scen_light light;
		uint16_t perm[OBJ_MAX_REGIONS];
		model_info mi;
		unsigned r;
		light_at(o, cam->pos, &light);
		{                                                  /* the view model is never in the dark: Halo lights it with its own rig */
			unsigned k;
			for (k = 0; k < 3; k++) { if (light.ambient[k] < 80) light.ambient[k] = 80; if (light.sun[k] < 60) light.sun[k] = 60; }
			light.sun_dir[0] = 0.3f; light.sun_dir[1] = 0.2f; light.sun_dir[2] = 0.93f;
		}
		mi = model_get(o->world.models, (uint32_t)f->arms_model);
		model_select_permutations(o->world.models, &mi, NULL, perm);
		for (r = 0; r < mi.region_count && r < OBJ_MAX_REGIONS; r++) if (perm[r] >= mi.permutation_count) perm[r] = 0;
		cc_pick(o, f->arms_model, 1u, &cc, -1);
		draw_model(o, rc, f->arms_model, perm, mi.region_count < OBJ_MAX_REGIONS ? mi.region_count : OBJ_MAX_REGIONS,
		           &f->arms_nodes, 0, &light, 400, &cc, 2);
		if (f->weapon_model >= 0) {
			object_nodes_t wn;
			mi = model_get(o->world.models, (uint32_t)f->weapon_model);
			model_select_permutations(o->world.models, &mi, NULL, perm);
			for (r = 0; r < mi.region_count && r < OBJ_MAX_REGIONS; r++) if (perm[r] >= mi.permutation_count) perm[r] = 0;
			if (objects_pose_model(&o->world, f->weapon_model, f->weapon_graph, f->weapon_anim, f->weapon_frame, &f->weapon_root, &wn))
				draw_model(o, rc, f->weapon_model, perm, mi.region_count < OBJ_MAX_REGIONS ? mi.region_count : OBJ_MAX_REGIONS, &wn, 0, &light, 400, NULL, 2);
		}
	}
	o->st_ms = timer_ms(t0, timer_cycles());
}

/* ---- sky ---------------------------------------------------------------------------------------- */
static void sky_emit_polys(chain_t *c, const cvert *poly, const unsigned *counts, unsigned npoly, u64 prim, const clip_gs *g)
{
	unsigned verts = 0, i, t;
	u32 *w;
	u64 *q;
	const cvert *v = poly;
	for (i = 0; i < npoly; i++) verts += 3 * (counts[i] - 2);
	if (!verts) return;
	w = chain_cnt(c, 1 + 1 + 3 * verts, rb_take_pending());
	if (!w) return;
	w[0] = VIF_CODE(VIF_NOP, 0, 0); w[1] = VIF_CODE(VIF_NOP, 0, 0);
	w[2] = VIF_CODE(VIF_NOP, 0, 0); w[3] = VIF_CODE(VIF_DIRECT, 0, 1 + 3 * verts);
	q = (u64 *)(w + 4);
	q[0] = GIF_TAG(verts, 1, 1, prim, GIFX_FLG_PACKED, 3); q[1] = 0x412;
	w = (u32 *)(q + 2);
	for (i = 0; i < npoly; i++) {
		for (t = 1; t + 1 < counts[i]; t++) {
			clip_pack_vertex(&v[0], 0, g, (uint32_t *)w); w += 12;
			clip_pack_vertex(&v[t], 0, g, (uint32_t *)w); w += 12;
			clip_pack_vertex(&v[t + 1], 0, g, (uint32_t *)w); w += 12;
		}
		v += counts[i];
	}
}

/* Bind-pose inverse of a node: the inverse of its world matrix in the model's default pose (chain of the nodes' own translation / rotation). */
static void node_bind_inverse(const model_section *ms, const model_info *mi, int node, mat4 *out)
{
	int chain[16], n = 0, i;
	mat4 w, l;
	for (; node >= 0 && node < (int)mi->node_count && n < 16; node = model_get_node(ms, mi, (uint32_t)node).parent) chain[n++] = node;
	memset(&w, 0, sizeof w);
	w.m[0][0] = w.m[1][1] = w.m[2][2] = w.m[3][3] = 1.0f;
	for (i = n - 1; i >= 0; i--) {                       /* root first: world = local * parent world */
		model_node nd = model_get_node(ms, mi, (uint32_t)chain[i]);
		mat4_from_trs(&l, nd.rotation, nd.translation, nd.scale > 0.0f ? nd.scale : 1.0f);
		mat4_mul(&w, &l, &w);
	}
	mat4_rigid_inverse(out, &w);
}

void obj_render_sky(void *user, render_ctx_t *rc)
{
	obj_render_t *o = user;
	const model_section *ms = o->world.models;
	model_info mi;
	uint16_t perm[OBJ_MAX_REGIONS];
	clip_gs g;
	unsigned r, p, bi, t, k, j;
	u32 t0 = timer_cycles();
	int sky = o->sky_model;
	float sky_yaw = 0.0f;
	o->sky_ms = 0.0f;
	if (rc->sky_index == 1 && o->sky_model2 >= 0) sky = o->sky_model2;       /* palette entry 1: a10's exterior (the intro's space shots) */
	if (sky == o->sky_model) sky_yaw = o->sky_yaw0;      /* palette entry 0 (cfg skyyaw): a10's windows need a quarter turn, see ADR-063 */
	if (sky < 0 || !ms || !rc->sky_visible) return;                 /* no visible cluster sees the sky: nothing to draw */
	mi = model_get(ms, (uint32_t)sky);
	model_select_permutations(ms, &mi, NULL, perm);
	g.near_z = rc->cam->near_z; g.zmax = 16777215.0f;
	g.sx = (float)VIDEO_WIDTH * 0.5f; g.sy = (float)VIDEO_HEIGHT * 0.5f; g.ox = 2048.0f; g.oy = 2048.0f;
	g.fog_k = 0.0f; g.fog_b = 4080.0f;
	for (r = 0; r < mi.region_count && r < OBJ_MAX_REGIONS; r++) {
		model_part_range pr;
		unsigned lod;
		if (o->sky_hide & (1u << r)) continue;
		if (perm[r] >= mi.permutation_count) perm[r] = 0;
		for (lod = 0; lod < MODEL_LODS && !model_parts(ms, &mi, perm[r], lod).part_count; lod++) {}
		if (lod == MODEL_LODS) continue;
		pr = model_parts(ms, &mi, perm[r], lod);
		for (p = 0; p < pr.part_count; p++) {
			model_part part = model_get_part(ms, (uint32_t)pr.first_part + p);
			mat_binding_t mb = { -1, 0, 0, { 1.0f, 1.0f }, 0, -1 };
			gs_ad ad[7];
			unsigned n = 0;
			u64 tex0 = 0, prim;
			float su, sv;
			int blend, atest;
			mat4 nw;
			/* A sky model has no animation: its nodes are posed with the identity, so the skin matrix world(pose) * inverse(world(default)) leaves
			 * the bind-pose inverse. a10's sky node is yawed by 90 degrees by default, which turns the dome, planet and galaxy by -90: only then
			 * the intro's planet stands left of the Autumn as on the Xbox. */
			node_bind_inverse(ms, &mi, part.rigid_node, &nw);
			if (o->sky_rot[0] != 0.0f || o->sky_rot[1] != 0.0f || o->sky_rot[2] != 0.0f || sky_yaw != 0.0f) {
				static const float zero[3] = { 0.0f, 0.0f, 0.0f };
				mat4 er;
				mat4_from_ypr(&er, o->sky_rot[0] + sky_yaw, o->sky_rot[1], o->sky_rot[2], zero);
				mat4_mul(&nw, &nw, &er);
			}
			if (o->textures && o->textures->binding && o->textures->binding(o->textures->user, part.shader_id, &mb) != 0) mb.tex = -1;
			blend = (mb.flags & MAT_BLEND) != 0 || (part.flags & 2);
			atest = (mb.flags & MAT_ALPHA_TEST) != 0 || (part.flags & 1);
			if (o->sky_hide & 0x40000000u) printf("[sky] region %u part %u shader %08x tex %d flags %x add %d blend %d uv %.2f %.2f batches %u\n", r, p, (unsigned)part.shader_id, mb.tex, (unsigned)mb.flags, mb.add, blend, (double)mb.uv_scale[0], (double)mb.uv_scale[1], (unsigned)part.batch_count);
			if (o->sky_hide & (0x100u << p)) continue;
			if (rc->opt->textures && mb.tex >= 0 && o->textures && o->textures->texture) {
				tex_image_t img;
				if (o->textures->texture(o->textures->user, mb.tex, &img) == 0) tex0 = rb_object_tex0((unsigned)mb.tex, &img, 0);
			}
			if (tex0) {
				ad[n].data = tex0;                          ad[n++].addr = GS_A_TEX0_1;
				ad[n].data = GS_TEX1(1, 0, 1, 1, 0, 0, 0);  ad[n++].addr = GS_A_TEX1_1;
				ad[n].data = GS_CLAMP(0, 0, 0, 0, 0, 0);    ad[n++].addr = GS_A_CLAMP_1;
			}
			ad[n].data = mb.add ? GS_ALPHA(0, 2, 2, 1, 0x80) : GS_ALPHA(0, 1, 0, 1, 0); ad[n++].addr = GS_A_ALPHA_1;
			ad[n].data = GS_TEST(atest && tex0 ? 1 : 0, 6, 0x40, 0, 0, 0, 1, 1); ad[n++].addr = GS_A_TEST_1;     /* Z test always */
			ad[n].data = GS_ZBUF(*rb_zbuf_page_ptr(), GS_PSMZ24, 1); ad[n++].addr = GS_A_ZBUF_1;                  /* no Z write */
			emit_gs_state(rc->chain, ad, n);               /* never emit_part_state here: it would overwrite the BSP clip matrix */
			prim = GS_PRIM(GSX_PRIM_TRIANGLE, 1, tex0 ? 1 : 0, 0, blend, 0, 0, 0, 0);
			su = mb.uv_scale[0] / (float)(1u << part.uv_shift);
			sv = mb.uv_scale[1] / (float)(1u << part.uv_shift);
			for (bi = 0; bi < part.batch_count; bi++) {
				model_batch b = model_get_batch(ms, (uint32_t)part.first_batch + bi);
				const uint8_t *vif = model_batch_vif(ms, &b);
				unsigned nv = 3u * b.triangles;
				const float *hdr = (const float *)(const void *)(vif + 4);
				const int16_t *pos = (const int16_t *)(const void *)(vif + 40);
				const int16_t *uv = (const int16_t *)(const void *)(vif + 44 + 8u * nv);
				static cvert polys[24 * CLIP_MAX_VERTS];
				unsigned counts[24], np = 0, filled = 0;
				for (t = 0; t < b.triangles; t++) {
					cvert tri[3], out[CLIP_MAX_VERTS];
					unsigned cnt;
					for (k = 0; k < 3; k++) {
						float wp[3], lp[3];
						const int16_t *q = pos + 4 * (3 * t + k), *uvk = uv + 4 * (3 * t + k);
						for (j = 0; j < 3; j++) lp[j] = hdr[j] + (float)q[j] * hdr[3];
						mat4_transform_point(&nw, lp, wp);
						for (j = 0; j < 3; j++) wp[j] += rc->cam->pos[j];                                    /* sky follows the camera */
						clip_project(rc->clip, wp, &tri[k]);
						memset(tri[k].a, 0, sizeof tri[k].a);
						tri[k].a[0] = (float)uvk[0] * su; tri[k].a[1] = (float)uvk[1] * sv;
						tri[k].a[4] = tri[k].a[5] = tri[k].a[6] = 128.0f * (mb.add ? 1.0f : o->sky_planet_gain);   /* the planet billboards (double multiply on the Xbox) */
						tri[k].a[7] = 128.0f;
					}
					cnt = (unsigned)clip_polygon(tri, rc->cam->near_z, out);
					if (cnt >= 3 && filled + cnt <= 24 * CLIP_MAX_VERTS) {
						memcpy(&polys[filled], out, cnt * sizeof(cvert));
						counts[np++] = cnt;
						filled += cnt;
					}
				}
				sky_emit_polys(rc->chain, polys, counts, np, prim, &g);
			}
		}
	}
	o->sky_ms = timer_ms(t0, timer_cycles());
}
