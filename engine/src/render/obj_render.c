#include "render/obj_render.h"
#include "render/gs.h"
#include "platform/timer.h"
#include <math.h>
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
static unsigned draw_model(obj_render_t *o, const render_ctx_t *rc, int model, const uint16_t *perm, unsigned regions,
                           const object_nodes_t *nodes, unsigned lod, const scen_light *light, unsigned budget)
{
	const model_section *ms = o->world.models;
	model_info mi = model_get(ms, (uint32_t)model);
	unsigned i, r, p, bi, emitted = 0;
	if (nodes->count == 0 || budget == 0) return 0;
	for (i = 0; i < nodes->count; i++) skel_palette_entry(s_pal[i], &nodes->skin[i], rc->clip, light->sun_dir);
	s_pal_serial++;                                   /* new palette contents: the VU1 copy is stale */
	emit_light(rc->chain, light);
	for (r = 0; r < regions; r++) {
		model_part_range pr = model_parts(ms, &mi, perm[r], lod);
		for (p = 0; p < pr.part_count; p++) {
			model_part part = model_get_part(ms, (uint32_t)pr.first_part + p);
			mat_binding_t mb = { -1, 0, 0, { 1.0f, 1.0f } };
			gs_ad ad[7];
			unsigned n = 0;
			u64 tex0 = 0;
			int blend, atest;
			if (emitted + part.batch_count > budget) { o->st_skipped++; continue; }
			if (o->textures && o->textures->binding && o->textures->binding(o->textures->user, part.shader_id, &mb) != 0) mb.tex = -1;
			blend = (mb.flags & MAT_BLEND) != 0 || (part.flags & 2);
			atest = (mb.flags & MAT_ALPHA_TEST) != 0 || (part.flags & 1);
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
			ad[n].data = GS_TEST(atest && tex0 ? 1 : 0, 6, 0x40, 0, 0, 0, 1, 2); ad[n++].addr = GS_A_TEST_1;
			ad[n].data = GS_ZBUF(*rb_zbuf_page_ptr(), GS_PSMZ24, 0); ad[n++].addr = GS_A_ZBUF_1;
			emit_part_state(rc->chain, ad, n, mb.uv_scale[0] / (float)(1u << part.uv_shift), mb.uv_scale[1] / (float)(1u << part.uv_shift),
			                GS_PRIM(GSX_PRIM_TRIANGLE, 1, tex0 ? 1 : 0, rc->opt->fog, blend, 0, 0, 0, 0));
			for (bi = 0; bi < part.batch_count; bi++) {
				model_batch b = model_get_batch(ms, (uint32_t)part.first_batch + bi);
				emit_batch(rc->chain, rc, ms, &b);
				o->st_triangles += b.triangles;
			}
			emitted += part.batch_count;
		}
	}
	o->st_batches += emitted;
	return emitted;
}

static void light_at(const obj_render_t *o, const float pos[3], scen_light *l)
{
	int cl = o->bsp ? bsp_find_cluster(o->bsp, pos) : -1;
	if (cl >= 0 && o->world.scen) scen_cluster_light(o->world.scen, (uint32_t)o->bsp_index, (uint32_t)cl, l);
	else scen_cluster_light(o->world.scen, (uint32_t)o->bsp_index, 0xFFFFu, l);   /* neutral default */
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
	u32 t0 = timer_cycles();
	o->st_visible = o->st_culled = o->st_batches = o->st_triangles = o->st_skipped = 0;
	if (!o->world.models || !o->world.scen) return;
	camera_basis(cam, right, up, fwd);
	emit_model_setup(rc->chain, rc);

	for (i = 0; o->draw_objects && o->table && i < o->table->count; i++) {
		const object_t *ob = &o->table->obj[i];
		model_info mi;
		if (ob->model < 0 || (ob->flags & (OBJ_F_FREE | OBJ_F_INACTIVE | OBJ_F_HIDDEN))) continue;
		mi = model_get(o->world.models, (uint32_t)ob->model);
		float c[3], d[3], dist, w, diameter;
		scen_light light;
		unsigned lod;
		mat4_transform_point(&ob->root, mi.centre, c);
		d[0] = c[0] - cam->pos[0]; d[1] = c[1] - cam->pos[1]; d[2] = c[2] - cam->pos[2];
		dist = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
		w = d[0] * fwd[0] + d[1] * fwd[1] + d[2] * fwd[2];
		if (dist - ob->radius > maxd || !frustum_sphere(rc->frustum, c, ob->radius)) { o->st_culled++; continue; }
		diameter = 2.0f * ob->radius * 320.0f * kx / (w > cam->near_z ? w : cam->near_z);
		if (diameter < 3.0f && dist > ob->radius * 2.0f) { o->st_culled++; continue; }   /* sub-pixel clutter */
		lod = pick_lod(o->world.models, &mi, ob->perm, ob->region_count, diameter, bias);
		if ((ob->flags & (OBJ_F_ANIMATED | OBJ_F_POSED)) ? !objects_pose(ob, &o->world, &s_nodes) : !objects_pose_static(ob, &o->world, &s_nodes)) { o->st_culled++; continue; }
		light_at(o, c, &light);
		o->st_visible++;
		if (draw_model(o, rc, ob->model, ob->perm, ob->region_count, &s_nodes, lod, &light, budget > o->st_batches ? budget - o->st_batches : 0) == 0
		    && o->st_batches >= budget) { o->st_skipped++; }
	}

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
		draw_model(o, rc, f->arms_model, perm, mi.region_count < OBJ_MAX_REGIONS ? mi.region_count : OBJ_MAX_REGIONS,
		           &f->arms_nodes, 0, &light, 400);
		if (f->weapon_model >= 0) {
			object_nodes_t wn;
			mi = model_get(o->world.models, (uint32_t)f->weapon_model);
			model_select_permutations(o->world.models, &mi, NULL, perm);
			for (r = 0; r < mi.region_count && r < OBJ_MAX_REGIONS; r++) if (perm[r] >= mi.permutation_count) perm[r] = 0;
			if (objects_pose_model(&o->world, f->weapon_model, f->weapon_graph, f->weapon_anim, f->weapon_frame, &f->weapon_root, &wn))
				draw_model(o, rc, f->weapon_model, perm, mi.region_count < OBJ_MAX_REGIONS ? mi.region_count : OBJ_MAX_REGIONS, &wn, 0, &light, 400);
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
	o->sky_ms = 0.0f;
	if (rc->sky_index == 1 && o->sky_model2 >= 0) sky = o->sky_model2;       /* palette entry 1: a10's exterior (the intro's space shots) */
	if (sky < 0 || !ms || !rc->sky_visible) return;                 /* no visible cluster sees the sky: nothing to draw */
	mi = model_get(ms, (uint32_t)sky);
	model_select_permutations(ms, &mi, NULL, perm);
	g.near_z = rc->cam->near_z; g.zmax = 16777215.0f;
	g.sx = (float)VIDEO_WIDTH * 0.5f; g.sy = (float)VIDEO_HEIGHT * 0.5f; g.ox = 2048.0f; g.oy = 2048.0f;
	g.fog_k = 0.0f; g.fog_b = 4080.0f;
	for (r = 0; r < mi.region_count && r < OBJ_MAX_REGIONS; r++) {
		model_part_range pr;
		unsigned lod;
		if (perm[r] >= mi.permutation_count) perm[r] = 0;
		for (lod = 0; lod < MODEL_LODS && !model_parts(ms, &mi, perm[r], lod).part_count; lod++) {}
		if (lod == MODEL_LODS) continue;
		pr = model_parts(ms, &mi, perm[r], lod);
		for (p = 0; p < pr.part_count; p++) {
			model_part part = model_get_part(ms, (uint32_t)pr.first_part + p);
			mat_binding_t mb = { -1, 0, 0, { 1.0f, 1.0f } };
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
			if (o->textures && o->textures->binding && o->textures->binding(o->textures->user, part.shader_id, &mb) != 0) mb.tex = -1;
			blend = (mb.flags & MAT_BLEND) != 0 || (part.flags & 2);
			atest = (mb.flags & MAT_ALPHA_TEST) != 0 || (part.flags & 1);
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
						tri[k].a[4] = tri[k].a[5] = tri[k].a[6] = tri[k].a[7] = 128.0f;
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
