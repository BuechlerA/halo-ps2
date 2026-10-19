#include "render/bsp_render.h"
#include "render/chain.h"
#include "render/render_internal.h"
#include "render/clip.h"
#include "render/gs.h"
#include "render/upload.h"
#include "platform/timer.h"
#include <kernel.h>
#include <dma.h>
#include <draw.h>
#include <malloc.h>
#include <packet2.h>
#include <packet2_utils.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

extern u32 VU1Bsp_CodeStart __attribute__((section(".vudata")));
extern u32 VU1Bsp_EntryLit  __attribute__((section(".vudata")));
extern u32 VU1Bsp_EntryLm   __attribute__((section(".vudata")));
extern u32 VU1Model_Entry   __attribute__((section(".vudata")));
extern u32 VU1World_CodeEnd __attribute__((section(".vudata")));

#define VU_DBUF_BASE   16
#define VU_DBUF_OFFSET BSP_VU_QWORDS            /* 220 */
#define CHAIN_QW       32768                    /* 512 KB per frame chain */
#define UPLOAD_QW      4096
#define CLUT_POOL_PAGES 16                      /* page 0..15 of the texture window: 128 CLUT slots of 4 blocks */
#define FONT_PAGE      CLUT_POOL_PAGES
#define CACHE_FIRST_PAGE (FONT_PAGE + 1)
#define FONT_CLUT_SLOT 127
#define PAGE_WORDS     2048u
#define ZMAX           16777215.0f
#define GS_CSR         ((volatile u64 *)0x12001000)
#define D1_CHCR        ((volatile u32 *)0x10009000)

/* ---- state ---------------------------------------------------------------------------------- */
static u8       *s_chain_mem[2], *s_upload_mem[2];
static chain_t   s_chain[2], s_upload[2];
static int       s_build;                       /* chain being built; the other one may be in flight */
static u32       s_window_base;                 /* VRAM word address of the texture window */
static u32       s_window_pages;
static u32       s_zbuf_page;
static u64       s_pending;                     /* VIF pair (MSCAL) for the next tag */

static const bsp_section *s_bsp;
static vis_t            s_vis;
static tc_t             s_tc;
static tex_source_t     s_src;
static mat_binding_t   *s_bind;
static u32             *s_mat_count, *s_mat_start, *s_mat_cursor, *s_sorted, *s_sorted2;
static u64             *s_mat_key;
static u16             *s_used;
static u32             *s_page_count, *s_page_start, *s_page_cursor;
static u32              s_page_slots;
/* EE-clipped triangles (see render/clip.h): polygons gathered per frame, emitted by material / lightmap page */
#define CLIP_MAX_POLYS 1024
#define CLIP_MAX_CVERTS 6144
typedef struct { u32 first; u16 material, page; u8 n; } clip_poly_t;
static clip_poly_t *s_cpoly;
static cvert       *s_cvert;
static u32          s_cpoly_count, s_cvert_count;
static float       *s_box;                          /* per batch: min xyz, max xyz (6 floats) */
static u16         *s_cpoly_sorted;                 /* poly indices grouped by material, then by page */
static u32         *s_cgroup_count, *s_cgroup_start;
static render_pass_fn   s_sky_pass;
static void            *s_sky_user;
static render_pass_fn   s_obj_pass;
static void            *s_obj_user;
static tex_source_t     s_obj_src;
static unsigned         s_obj_key_base;
#define KEY_LM_SLOTS 64u                             /* texture residency keys reserved for lightmap pages (a10: 55 at most) */
static unsigned         s_obj_count;                 /* object textures */
static unsigned         s_extra_total;               /* texture keys reserved for HUD / effect sprites */
static render_pass_fn   s_overlay_pass;
static void            *s_overlay_user;
static render_options_t s_opt;
static render_stats_t   s_stats;

static packet2_t *s_env;
static u32 s_kick_cycles, s_vif_cycles, s_gs_cycles;
static int s_vif_seen, s_gs_seen;
static int s_horizon_y;                      /* screen row of the horizon for the frame being built */

static u32 entry_addr(const u32 *e) { return ((u32)e - (u32)&VU1Bsp_CodeStart) / 8; }

/* ---- init ----------------------------------------------------------------------------------- */
static void send_vif_program(void)
{
	u32 n = packet2_utils_get_packet_size_for_program(&VU1Bsp_CodeStart, &VU1World_CodeEnd) + 1;
	packet2_t *p = packet2_create(n, P2_TYPE_NORMAL, P2_MODE_CHAIN, 1);
	packet2_vif_add_micro_program(p, 0, &VU1Bsp_CodeStart, &VU1World_CodeEnd);
	packet2_utils_vu_add_end_tag(p);
	dma_channel_send_packet2(p, DMA_CHANNEL_VIF1, 1);
	dma_channel_wait(DMA_CHANNEL_VIF1, 0);
	packet2_free(p);
	p = packet2_create(2, P2_TYPE_NORMAL, P2_MODE_CHAIN, 1);
	packet2_utils_vu_add_double_buffer(p, VU_DBUF_BASE, VU_DBUF_OFFSET);
	packet2_utils_vu_add_end_tag(p);
	dma_channel_send_packet2(p, DMA_CHANNEL_VIF1, 1);
	dma_channel_wait(DMA_CHANNEL_VIF1, 0);
	packet2_free(p);
	printf("[vu1] bsp program: %u bytes, entries lit=%u lm=%u\n",
	       (u32)&VU1World_CodeEnd - (u32)&VU1Bsp_CodeStart, entry_addr(&VU1Bsp_EntryLit), entry_addr(&VU1Bsp_EntryLm));
}

static u8 *alloc_uncached(size_t bytes)
{
	void *p = memalign(64, bytes);
	if (!p) return NULL;
	memset(p, 0, bytes);
	SyncDCache(p, (u8 *)p + bytes);
	return (u8 *)((u32)p | 0x30000000u);     /* uncached accelerated: no flushes needed for chain writes */
}

static void upload_font(void)
{
	static u8 atlas[TEXT_ATLAS_W * TEXT_ATLAS_H] __attribute__((aligned(64)));
	static uint32_t clut[256] __attribute__((aligned(64)));
	static u8 mem[8 * 16 * 2 + 64] __attribute__((aligned(64)));
	chain_t c;
	u32 atlas_block = (s_window_base + FONT_PAGE * PAGE_WORDS) / 64;
	u32 clut_block = s_window_base / 64 + FONT_CLUT_SLOT * 4;
	text_build_atlas(atlas);
	text_build_clut(clut);
	SyncDCache(atlas, atlas + sizeof atlas);
	SyncDCache(clut, clut + 256);
	chain_init(&c, mem, sizeof mem / 16);
	gs_upload(&c, atlas, sizeof atlas, atlas_block, TEXT_ATLAS_W / 64, GS_PSMT8, TEXT_ATLAS_W, TEXT_ATLAS_H);
	gs_upload(&c, clut, sizeof clut, clut_block, 1, GS_PSMCT32, 16, 16);
	chain_end(&c, 0);
	SyncDCache(mem, mem + sizeof mem);
	dma_channel_send_chain(DMA_CHANNEL_GIF, mem, 0, 0, 0);
	dma_wait_fast();
}

void bsp_render_options(render_options_t *o)
{
	memset(o, 0, sizeof *o);
	o->textures = 1;
	o->lightmaps = 1;
	o->fog = 0;
	o->clip_budget_ms = 6.0f;
	o->only_batch = -1;
	o->only_batch_hi = 0x7FFFFFFF;
	o->fog_near = 40.0f;
	o->fog_far = 400.0f;
	o->fog_rgb[0] = 150; o->fog_rgb[1] = 170; o->fog_rgb[2] = 190;
	o->sky_rgb[0] = 20; o->sky_rgb[1] = 24; o->sky_rgb[2] = 40;
	o->horizon_rgb[0] = 20; o->horizon_rgb[1] = 24; o->horizon_rgb[2] = 40;
}

void bsp_render_set_options(const render_options_t *o) { s_opt = *o; }

int bsp_render_init(const video_t *v)
{
	int i;
	dma_channel_initialize(DMA_CHANNEL_VIF1, NULL, 0);
	dma_channel_fast_waits(DMA_CHANNEL_VIF1);
	send_vif_program();
	for (i = 0; i < 2; i++) {
		s_chain_mem[i] = alloc_uncached((size_t)CHAIN_QW * 16);
		s_upload_mem[i] = alloc_uncached((size_t)UPLOAD_QW * 16);
		if (!s_chain_mem[i] || !s_upload_mem[i]) { printf("[err] out of memory for chains\n"); return -1; }
	}
	s_window_base = v->tex_base;
	s_window_pages = (VIDEO_VRAM_WORDS - v->tex_base) / PAGE_WORDS;
	s_zbuf_page = v->z.address / PAGE_WORDS;
	s_env = packet2_create(64, P2_TYPE_NORMAL, P2_MODE_NORMAL, 0);
	bsp_render_options(&s_opt);
	upload_font();
	printf("[video] texture window: base word %u, %u pages (%u KB): %u CLUT slots, font page, %u cache pages\n",
	       s_window_base, s_window_pages, s_window_pages * 8, FONT_CLUT_SLOT, s_window_pages - CACHE_FIRST_PAGE);
	return 0;
}

int bsp_render_set_world(arena_t *a, const bsp_section *s, const tex_source_t *src)
{
	unsigned i, pages = s_window_pages - CACHE_FIRST_PAGE;
	s_bsp = s;
	s_src = *src;
	if (vis_init(&s_vis, a, s)) { printf("[err] vis buffers do not fit\n"); return -1; }
	s_bind = arena_alloc(a, sizeof *s_bind * (s->material_count + 1), 16);
	s_mat_count = arena_alloc(a, 4u * (s->material_count + 1), 16);
	s_mat_start = arena_alloc(a, 4u * (s->material_count + 1), 16);
	s_mat_cursor = arena_alloc(a, 4u * (s->material_count + 1), 16);
	s_mat_key = arena_alloc(a, 8u * (s->material_count + 1), 16);
	s_used = arena_alloc(a, 2u * (s->material_count + 1), 16);
	s_sorted = arena_alloc(a, 4u * (s->batch_count + 1), 16);
	s_sorted2 = arena_alloc(a, 4u * (s->batch_count + 1), 16);
	s_page_slots = 0;
	for (i = 0; i < s->material_count; i++) {
		bsp_material m = bsp_get_material(s, i);
		if (m.lightmap_page != BSP_NO_LIGHTMAP && (u32)m.lightmap_page + 1 > s_page_slots) s_page_slots = (u32)m.lightmap_page + 1;
	}
	s_cpoly = arena_alloc(a, sizeof(clip_poly_t) * CLIP_MAX_POLYS, 16);
	s_cvert = arena_alloc(a, sizeof(cvert) * CLIP_MAX_CVERTS, 16);
	s_cpoly_sorted = arena_alloc(a, sizeof(u16) * CLIP_MAX_POLYS, 16);
	s_box = arena_alloc(a, sizeof(float) * 6 * (s->batch_count + 1), 16);
	s_cgroup_count = arena_alloc(a, 4u * (s->material_count + s_page_slots + 2), 16);
	s_cgroup_start = arena_alloc(a, 4u * (s->material_count + s_page_slots + 2), 16);
	s_page_count = arena_alloc(a, 4u * (s_page_slots + 1), 16);
	s_page_start = arena_alloc(a, 4u * (s_page_slots + 1), 16);
	s_page_cursor = arena_alloc(a, 4u * (s_page_slots + 1), 16);
	if (!s_box || !s_cpoly || !s_cvert || !s_cpoly_sorted || !s_cgroup_count || !s_cgroup_start ||
	    !s_bind || !s_mat_count || !s_mat_start || !s_mat_cursor || !s_mat_key || !s_used || !s_sorted || !s_sorted2 ||
	    !s_page_count || !s_page_start || !s_page_cursor) {
		printf("[err] renderer buffers do not fit\n");
		return -1;
	}
	{
		unsigned bound = 0, flat = 0;
		for (i = 0; i < s->material_count; i++) {
			bsp_material m = bsp_get_material(s, i);
			mat_binding_t b = { -1, 0, 0, { 1.0f, 1.0f }, 0, -1 };
			if (src->binding && src->binding(src->user, m.shader_id, &b) == 0 && b.tex >= 0) bound++; else { b.tex = -1; flat++; }
			s_bind[i] = b;
		}
		printf("[tex] materials: %u textured, %u flat; %u lightmap page slots\n", bound, flat, s_page_slots);
	}
	/* exact bounding boxes of the baked vertices (the table only stores a sphere) */
	for (i = 0; i < s->batch_count; i++) {
		bsp_batch b = bsp_get_batch(s, i);
		bsp_batch_raw raw = bsp_batch_streams(s, &b);
		float *bx = s_box + 6 * i;
		unsigned v, k;
		for (k = 0; k < 3; k++) { bx[k] = 1e30f; bx[3 + k] = -1e30f; }
		for (v = 0; v < raw.vertices; v++)
			for (k = 0; k < 3; k++) {
				float p = raw.centre[k] + (float)raw.pos[4 * v + k] * raw.scale;
				if (p < bx[k]) bx[k] = p;
				if (p > bx[3 + k]) bx[3 + k] = p;
			}
	}
	if (s_page_slots > KEY_LM_SLOTS) { printf("[err] %u lightmap pages exceed the %u reserved texture keys\n", (unsigned)s_page_slots, (unsigned)KEY_LM_SLOTS); return -1; }
	tc_init(&s_tc, pages, FONT_CLUT_SLOT, s_obj_pass ? s_obj_key_base + s_obj_count + s_extra_total : src->texture_count + KEY_LM_SLOTS, 1536u * 1024u);
	return 0;
}

/* ---- per-frame emission --------------------------------------------------------------------- */
static u64 take_pending(void)
{
	u64 p = s_pending;
	s_pending = 0;
	return p;
}

/* Frame constants (VU memory abs 0..6 and 9), a TEXFLUSH for textures uploaded since last frame. */
static void emit_frame_header(chain_t *c, const float clip[4][4])
{
	u32 *w;
	w = chain_cnt(c, 1, 0);
	if (w) {                                      /* BSP buffer layout (the object pass switches and restores it) */
		w[0] = VIF_CODE(VIF_NOP, 0, 0); w[1] = VIF_CODE(VIF_BASE, 0, 16);
		w[2] = VIF_CODE(VIF_OFFSET, 0, BSP_VU_QWORDS); w[3] = VIF_CODE(VIF_NOP, 0, 0);
	}
	w = chain_cnt(c, 1 + 7 + 1 + 2, 0);
	float *f;
	float fog_k = 0.0f, fog_b = 4080.0f;      /* GS: F = 255 means no fog */
	u64 *q;
	if (!w) return;
	w[0] = VIF_CODE(VIF_NOP, 0, 0); w[1] = VIF_CODE(VIF_NOP, 0, 0);
	w[2] = VIF_CODE(VIF_STCYCL, 0, 0x0101); w[3] = VIF_CODE(VIF_UNPACK_V4_32, 7, 0);
	f = (float *)(w + 4);
	memcpy(f, clip, 16 * sizeof(float));
	f[16] = (float)VIDEO_WIDTH * 0.5f; f[17] = (float)VIDEO_HEIGHT * 0.5f; f[18] = ZMAX; f[19] = 1.0f;   /* abs 4 */
	f[20] = 2048.0f; f[21] = 2048.0f; f[22] = 0.0f; f[23] = 0.0f;                                         /* abs 5 */
	f[24] = 1.0f / CLIP_GUARD; f[25] = 1.0f / CLIP_GUARD; f[26] = 2.0f; f[27] = 0.0f;                    /* abs 6: kS */
	w[32] = VIF_CODE(VIF_NOP, 0, 0); w[33] = VIF_CODE(VIF_NOP, 0, 0);
	w[34] = VIF_CODE(VIF_NOP, 0, 0); w[35] = VIF_CODE(VIF_UNPACK_V4_32, 2, 9);
	if (s_opt.fog && s_opt.fog_far > s_opt.fog_near) {
		fog_k = -4080.0f / (s_opt.fog_far - s_opt.fog_near);
		fog_b = 4080.0f - s_opt.fog_near * fog_k;
	}
	f = (float *)(w + 36);
	f[0] = fog_k; f[1] = fog_b; f[2] = 0.0f; f[3] = 4080.0f;                                              /* abs 9 */
	f[4] = 0.0f; f[5] = 0.0f; f[6] = -1.0f; f[7] = 0.0f;                                                  /* abs 10: kB */
	(void)q;
	/* TEXFLUSH + fog colour through PATH2 */
	w = chain_cnt(c, 1 + 3, take_pending());
	if (!w) return;
	w[0] = VIF_CODE(VIF_NOP, 0, 0); w[1] = VIF_CODE(VIF_NOP, 0, 0);
	w[2] = VIF_CODE(VIF_FLUSHA, 0, 0); w[3] = VIF_CODE(VIF_DIRECT, 0, 3);
	q = (u64 *)(w + 4);
	q[0] = GIF_TAG(2, 1, 0, 0, GIFX_FLG_PACKED, 1); q[1] = GIF_REGS_AD;
	q[2] = GS_FOGCOL(s_opt.fog_rgb[0], s_opt.fog_rgb[1], s_opt.fog_rgb[2]); q[3] = GS_A_FOGCOL;
	q[4] = 0; q[5] = GS_A_TEXFLUSH;
}

/* One state change: A+D registers through PATH2 plus the VU constants that depend on it
 * (abs 7 = uv scale, abs 8 = GIF tag template). */
static void emit_state(chain_t *c, const gs_ad *ad, unsigned n, float su, float sv, u64 prim)
{
	u32 *w = chain_cnt(c, 1 + 1 + n + 1 + 2, take_pending());
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
	w[0] = VIF_CODE(VIF_NOP, 0, 0); w[1] = VIF_CODE(VIF_NOP, 0, 0);
	w[2] = VIF_CODE(VIF_NOP, 0, 0); w[3] = VIF_CODE(VIF_UNPACK_V4_32, 2, 7);
	f = (float *)(w + 4);
	f[0] = su; f[1] = sv; f[2] = 0.0f; f[3] = 0.0f;
	q = (u64 *)(f + 4);
	q[0] = GIF_TAG(0, 0, 1, prim, GIFX_FLG_PACKED, 3);     /* NLOOP/EOP patched by VU1 */
	q[1] = 0x412;                                         /* ST, RGBAQ, XYZF2 */
}

static u32 pages_for(unsigned w, unsigned h, unsigned psm)
{
	unsigned tbw = (w + 63) / 64, page_h = psm == GS_PSMT4 ? 128 : 64;
	if (tbw < 1) tbw = 1;
	return ((tbw + 1) / 2) * ((h + page_h - 1) / page_h);
}

static unsigned ilog2(unsigned v) { unsigned r = 0; while ((1u << (r + 1)) <= v) r++; return r; }

/* Make a texture resident (queueing its upload); returns the TEX0 value or 0 if it cannot be
 * drawn this frame. */
static u64 resident_tex0(unsigned key, const tex_image_t *img, u64 tfx)
{
	const tc_entry *e;
	tc_result r;
	u32 pages, tbw, tbp, cbp, px_bytes, clut_bytes, clut_w, clut_h;
	int t4 = img->psm == GS_PSMT4;
	if (t4 && (s_opt.debug & 8)) return 0;
	if ((img->psm != GS_PSMT8 && !t4) || img->w == 0 || img->h == 0 || (img->w & (img->w - 1)) || (img->h & (img->h - 1))) return 0;
	px_bytes = t4 ? (u32)img->w * img->h / 2 : (u32)img->w * img->h;
	clut_bytes = t4 ? 64 : 1024;                       /* 8x2 or 16x16 PSMCT32 */
	clut_w = t4 ? 8 : 16; clut_h = t4 ? 2 : 16;
	if (t4 && (s_opt.debug & 16)) { clut_w = 16; clut_h = 1; }
	if ((px_bytes & 15) || img->clut_bytes < clut_bytes || img->pixel_bytes < px_bytes) return 0;
	pages = pages_for(img->w, img->h, img->psm);
	r = tc_acquire(&s_tc, key, pages, px_bytes + clut_bytes, &e);
	if (r != TC_HIT && r != TC_UPLOAD) return 0;
	tbw = (img->w + 63) / 64;
	tbp = (s_window_base + (CACHE_FIRST_PAGE + e->first_page) * PAGE_WORDS) / 64;
	cbp = s_window_base / 64 + e->clut_slot * 4;
	if (r == TC_UPLOAD) {
		chain_t *u = &s_upload[s_build];
		if (gs_upload(u, img->pixels, px_bytes, tbp, tbw, img->psm, img->w, img->h) ||
		    gs_upload(u, img->clut, clut_bytes, cbp, 1, GS_PSMCT32, clut_w, clut_h)) {
			s_tc.failed++;           /* upload chain full: the claim stays but draws flat this frame */
			return 0;
		}
		s_stats.uploads++;
		s_stats.upload_bytes += px_bytes + clut_bytes;
	}
	return GS_TEX0(tbp, tbw, img->psm, ilog2(img->w), ilog2(img->h), 1, tfx, cbp, GS_PSMCT32, 0, 0, 1);
}

static int cmp_key(const void *a, const void *b)
{
	u64 x = s_mat_key[*(const u16 *)a], y = s_mat_key[*(const u16 *)b];
	return x < y ? -1 : x > y;
}

static void emit_batches(chain_t *c, const u32 *list, u32 count, u32 entry)
{
	u32 i;
	for (i = 0; i < count; i++) {
		bsp_batch b = bsp_get_batch(s_bsp, list[i]);
		chain_ref(c, bsp_batch_vif(s_bsp, &b), b.vif_qwords, take_pending());
		s_pending = VIF_PAIR(VIF_CODE(VIF_MSCAL, 0, entry), 0);
	}
}


/* ---- EE clipping of the triangles the VU1 drops ------------------------------------------------ */
static void clip_gs_params(clip_gs *g, const camera_t *cam)
{
	g->near_z = cam->near_z; g->zmax = ZMAX;
	g->sx = (float)VIDEO_WIDTH * 0.5f; g->sy = (float)VIDEO_HEIGHT * 0.5f; g->ox = 2048.0f; g->oy = 2048.0f;
	g->fog_k = 0.0f; g->fog_b = 4080.0f;
	if (s_opt.fog && s_opt.fog_far > s_opt.fog_near) {
		g->fog_k = -4080.0f / (s_opt.fog_far - s_opt.fog_near);
		g->fog_b = 4080.0f - s_opt.fog_near * g->fog_k;
	}
}

/* Can any triangle of this batch leave the guard band or cross the near plane? The batch's box
 * is convex, so if all eight corners project inside the band and in front of the near plane, so
 * does everything in it. */
static int batch_unsafe(u32 index, const camera_t *cam, const float clip[4][4])
{
	const float *bx = s_box + 6 * index;
	float lim = CLIP_GUARD * 0.97f;
	unsigned any = 0, all = 0x1F;
	int c;
	for (c = 0; c < 8; c++) {
		float x = bx[(c & 1) ? 3 : 0], y = bx[(c & 2) ? 4 : 1], z = bx[(c & 4) ? 5 : 2];
		float w = clip[0][3] * x + clip[1][3] * y + clip[2][3] * z + clip[3][3];
		float cx = clip[0][0] * x + clip[1][0] * y + clip[2][0] * z + clip[3][0];
		float cy = clip[0][1] * x + clip[1][1] * y + clip[2][1] * z + clip[3][1];
		unsigned out = (w < cam->near_z ? 1u : 0u) | (cx > CLIP_GUARD * w ? 2u : 0u) | (cx < -CLIP_GUARD * w ? 4u : 0u)
		             | (cy > CLIP_GUARD * w ? 8u : 0u) | (cy < -CLIP_GUARD * w ? 16u : 0u);
		any |= out;
		all &= out;
		if (w < cam->near_z * 1.05f || cx > lim * w || cx < -lim * w || cy > lim * w || cy < -lim * w) any |= 0x80u;
	}
	if (all & 0x1F) return 2;                       /* the whole box is outside one plane of the band: nothing of it can show */
	return (any & 0x80u) ? 1 : 0;
}

#define MAX_UNSAFE 256
static void gather_clipped(const camera_t *cam, const float clip[4][4])
{
	u32 order[MAX_UNSAFE], n_unsafe = 0, oi, t, t_start = timer_cycles(), i;
	float dist2[MAX_UNSAFE];
	s_cpoly_count = s_cvert_count = 0;
	/* near batches first (by the distance to the nearest point of their box): a frame that runs out of budget loses the farthest clipped geometry */
	for (i = 0; i < s_vis.batch_count; i++) {
		u32 bi = s_vis.batches[i], j;
		float d, dx, dy, dz;
		const float *bx = s_box + 6 * bi;
		{ int u = batch_unsafe(bi, cam, clip); if (u != 1) { if (u == 2) s_stats.clip_culled_batches++; continue; } }
		dx = 0.5f * (bx[0] + bx[3]) - cam->pos[0]; dy = 0.5f * (bx[1] + bx[4]) - cam->pos[1]; dz = 0.5f * (bx[2] + bx[5]) - cam->pos[2];
		d = sqrtf(dx * dx + dy * dy + dz * dz) - 0.5f * sqrtf((bx[3] - bx[0]) * (bx[3] - bx[0]) + (bx[4] - bx[1]) * (bx[4] - bx[1]) + (bx[5] - bx[2]) * (bx[5] - bx[2]));
		if (d < 0.0f) d = 0.0f;                       /* distance to the nearest point of the box: a big floor triangle under the camera comes first */
		if (n_unsafe >= MAX_UNSAFE) { s_stats.clip_skipped_batches++; continue; }
		for (j = n_unsafe; j > 0 && dist2[j - 1] > d; j--) { dist2[j] = dist2[j - 1]; order[j] = order[j - 1]; }
		dist2[j] = d; order[j] = bi;
		n_unsafe++;
	}
	s_stats.clip_batches = n_unsafe;
	s_stats.clip_scan_ms = timer_ms(t_start, timer_cycles());
	for (oi = 0; oi < n_unsafe; oi++) {
		bsp_batch b = bsp_get_batch(s_bsp, order[oi]);
		bsp_material bm;
		float su, sv;
		if (timer_ms(t_start, timer_cycles()) > s_opt.clip_budget_ms) { s_stats.clip_skipped_batches += n_unsafe - oi; break; }
		bm = bsp_get_material(s_bsp, b.material);
		su = s_bind[b.material].uv_scale[0] / (float)(1u << bm.uv_shift);
		sv = s_bind[b.material].uv_scale[1] / (float)(1u << bm.uv_shift);
		{
			/* per-batch transform: clip = T + s0*x + s1*y + s2*z with q the raw s16 position */
			bsp_batch_raw raw = bsp_batch_streams(s_bsp, &b);
			float tv[4], sv0[4], sv1[4], sv2[4];
			unsigned k;
			for (k = 0; k < 4; k++) {
				tv[k] = clip[0][k] * raw.centre[0] + clip[1][k] * raw.centre[1] + clip[2][k] * raw.centre[2] + clip[3][k];
				sv0[k] = clip[0][k] * raw.scale; sv1[k] = clip[1][k] * raw.scale; sv2[k] = clip[2][k] * raw.scale;
			}
			for (t = 0; t < b.triangles; t++) {
				cvert tri[3], poly[CLIP_MAX_VERTS];
				unsigned j, n;
				int outside = 0;
				for (k = 0; k < 3; k++) {
					const int16_t *q = raw.pos + 4 * (3 * t + k);
					float x = (float)q[0], y = (float)q[1], z = (float)q[2];
					tri[k].c[3] = tv[3] + sv0[3] * x + sv1[3] * y + sv2[3] * z;
					tri[k].c[0] = tv[0] + sv0[0] * x + sv1[0] * y + sv2[0] * z;
					tri[k].c[1] = tv[1] + sv0[1] * x + sv1[1] * y + sv2[1] * z;
				}
				outside = clip_needs(tri, cam->near_z, 0.985f);      /* x, y and w only: z is needed once the triangle is clipped */
				if (!outside) continue;
				s_stats.clip_tris++;
				if (clip_outside(tri, cam->near_z)) continue;
				for (k = 0; k < 3; k++) {
					const int16_t *q = raw.pos + 4 * (3 * t + k);
					tri[k].c[2] = tv[2] + sv0[2] * (float)q[0] + sv1[2] * (float)q[1] + sv2[2] * (float)q[2];
				}
				for (k = 0; k < 3; k++) {
					const int16_t *uv = raw.uv + 4 * (3 * t + k);
					const uint8_t *col = raw.col + 4 * (3 * t + k);
					tri[k].a[0] = (float)uv[0] * su; tri[k].a[1] = (float)uv[1] * sv;
					tri[k].a[2] = (float)uv[2] * (1.0f / 32767.0f); tri[k].a[3] = (float)uv[3] * (1.0f / 32767.0f);
					for (j = 0; j < 4; j++) tri[k].a[4 + j] = (float)col[j];
				}
				{ u32 cc0 = timer_cycles(); n = (unsigned)clip_polygon(tri, cam->near_z, poly); s_stats.emit_clip_ms += timer_ms(cc0, timer_cycles()); }
				if (n < 3) continue;
				if (s_cpoly_count >= CLIP_MAX_POLYS || s_cvert_count + n > CLIP_MAX_CVERTS) { s_stats.clip_dropped++; continue; }
				s_cpoly[s_cpoly_count].first = s_cvert_count;
				s_cpoly[s_cpoly_count].n = (u8)n;
				s_cpoly[s_cpoly_count].material = b.material;
				s_cpoly[s_cpoly_count].page = bm.lightmap_page;
				memcpy(&s_cvert[s_cvert_count], poly, n * sizeof(cvert));
				s_cvert_count += n;
				s_cpoly_count++;
			}
		}
	}
	s_stats.clip_polys = s_cpoly_count;
	s_stats.clip_gather_ms = timer_ms(t_start, timer_cycles());
}

/* Group the clipped polygons by `key` (material or lightmap page). Fills s_cgroup_start/count. */
static void group_clipped(int by_page)
{
	u32 keys = by_page ? s_page_slots : s_bsp->material_count, i, pos = 0;
	memset(s_cgroup_count, 0, 4u * (keys + 1));
	for (i = 0; i < s_cpoly_count; i++) {
		u32 k = by_page ? s_cpoly[i].page : s_cpoly[i].material;
		if (by_page && (s_cpoly[i].page == BSP_NO_LIGHTMAP || (s_bind[s_cpoly[i].material].flags & MAT_BLEND))) continue;
		s_cgroup_count[k]++;
	}
	for (i = 0; i < keys; i++) { s_cgroup_start[i] = pos; pos += s_cgroup_count[i]; }
	for (i = 0; i < keys; i++) s_cgroup_count[i] = 0;
	for (i = 0; i < s_cpoly_count; i++) {
		u32 k = by_page ? s_cpoly[i].page : s_cpoly[i].material;
		if (by_page && (s_cpoly[i].page == BSP_NO_LIGHTMAP || (s_bind[s_cpoly[i].material].flags & MAT_BLEND))) continue;
		s_cpoly_sorted[s_cgroup_start[k] + s_cgroup_count[k]++] = (u16)i;
	}
}

/* Draw one group of clipped polygons through PATH2 (fans -> triangle lists). */
static void emit_clipped(chain_t *c, u32 key, int pass, u64 prim, const clip_gs *g)
{
	u32 count = s_cgroup_count[key], start = s_cgroup_start[key], i = 0;
	while (i < count) {
		u32 verts = 0, j = i;
		u32 *w;
		u64 *q;
		while (j < count && verts + 3 * (s_cpoly[s_cpoly_sorted[start + j]].n - 2u) <= 4000u) {
			verts += 3 * (s_cpoly[s_cpoly_sorted[start + j]].n - 2u);
			j++;
		}
		if (j == i) return;                                /* cannot happen: a polygon has at most 10 triangles' worth */
		w = chain_cnt(c, 1 + 1 + 3 * verts, take_pending());
		if (!w) return;
		w[0] = VIF_CODE(VIF_NOP, 0, 0); w[1] = VIF_CODE(VIF_NOP, 0, 0);
		w[2] = VIF_CODE(VIF_NOP, 0, 0); w[3] = VIF_CODE(VIF_DIRECT, 0, 1 + 3 * verts);
		q = (u64 *)(w + 4);
		q[0] = GIF_TAG(verts, 1, 1, prim, GIFX_FLG_PACKED, 3); q[1] = 0x412;
		w = (u32 *)(q + 2);
		for (; i < j; i++) {
			const clip_poly_t *p = &s_cpoly[s_cpoly_sorted[start + i]];
			const cvert *v = &s_cvert[p->first];
			u32 t;
			for (t = 1; t + 1 < p->n; t++) {
				clip_pack_vertex(&v[0], pass, g, (uint32_t *)w); w += 12;
				clip_pack_vertex(&v[t], pass, g, (uint32_t *)w); w += 12;
				clip_pack_vertex(&v[t + 1], pass, g, (uint32_t *)w); w += 12;
			}
		}
	}
}

void bsp_render_build(u32 frame, const camera_t *cam, const text_batch_t *overlay)
{
	u32 t0 = timer_cycles(), t1;
	chain_t *c;
	frustum_t fr;
	float clip[4][4];
	u32 i, nused = 0, total = s_bsp->material_count, pos;
	u32 entry_lit = entry_addr(&VU1Bsp_EntryLit), entry_lm = entry_addr(&VU1Bsp_EntryLm);
	unsigned fog = s_opt.fog;
	gs_ad ad[8];
	clip_gs cgs;

	s_build ^= 1;
	c = &s_chain[s_build];
	chain_init(c, s_chain_mem[s_build], CHAIN_QW);
	chain_init(&s_upload[s_build], s_upload_mem[s_build], UPLOAD_QW);
	s_pending = 0;
	memset(&s_stats, 0, sizeof s_stats);
	tc_begin_frame(&s_tc, frame);

	camera_frustum(cam, &fr);
	camera_clip_matrix(cam, clip);
	{
		/* horizon row: direction (cos yaw, sin yaw, 0) projects to y = H/2 + H/2 * ky * tan(pitch) */
		float ky = cam->aspect / tanf(cam->hfov * 0.5f);
		float hy = (float)VIDEO_HEIGHT * 0.5f * (1.0f + ky * tanf(cam->pitch));
		s_horizon_y = hy < 0.0f ? 0 : hy > (float)VIDEO_HEIGHT ? VIDEO_HEIGHT : (int)hy;
	}
	s_vis.force_all = (s_opt.debug & 64) != 0;
	vis_run(&s_vis, s_bsp, cam, &fr, clip);
	if (s_opt.only_batch >= 0) {
		u32 k, keep = 0, n = s_vis.batch_count;
		for (k = 0; k < n; k++) if ((int)s_vis.batches[k] >= s_opt.only_batch && (int)s_vis.batches[k] <= s_opt.only_batch_hi) s_vis.batches[keep++] = s_vis.batches[k];
		s_vis.batch_count = keep;
	}
	t1 = timer_cycles();
	s_stats.vis_ms = timer_ms(t0, t1);
	bsp_render_poll();

	clip_gs_params(&cgs, cam);
	{
		u32 tc0 = timer_cycles();
		if (!(s_opt.debug & 32)) gather_clipped(cam, (const float (*)[4])clip);
		else s_cpoly_count = 0;
		s_stats.clip_ms = timer_ms(tc0, timer_cycles());
	}
	group_clipped(0);

	/* group the visible batches by material (counting sort) */
	memset(s_mat_count, 0, 4u * total);
	for (i = 0; i < s_vis.batch_count; i++) s_mat_count[bsp_get_batch(s_bsp, s_vis.batches[i]).material]++;
	for (i = 0, pos = 0; i < total; i++) { s_mat_start[i] = s_mat_cursor[i] = pos; pos += s_mat_count[i]; }
	for (i = 0; i < s_vis.batch_count; i++) {
		u32 bi = s_vis.batches[i];
		s_sorted[s_mat_cursor[bsp_get_batch(s_bsp, bi).material]++] = bi;
	}
	for (i = 0; i < total; i++) {
		if (!s_mat_count[i]) continue;
		s_mat_key[i] = (u64)((s_bind[i].flags & MAT_BLEND) ? 1 : 0) << 48 | (u64)(s_bind[i].tex + 1) << 32 | i;
		s_used[nused++] = (u16)i;
	}
	qsort(s_used, nused, sizeof s_used[0], cmp_key);

	emit_frame_header(c, (const float (*)[4])clip);

	if (s_sky_pass) {
		render_ctx_t rc;
		rc.chain = c; rc.cam = cam; rc.clip = (const float (*)[4])clip; rc.frustum = &fr; rc.frame = frame; rc.opt = &s_opt; rc.sky_visible = s_vis.sky_visible; rc.sky_index = s_vis.sky_index;
		s_sky_pass(s_sky_user, &rc);
	}

	/* pass 1: base texture x vertex colour */
	for (i = 0; i < nused; i++) {
		unsigned m = s_used[i], n = 0;
		bsp_material bm = bsp_get_material(s_bsp, m);
		const mat_binding_t *mb = &s_bind[m];
		u64 tex0 = 0;
		float scale = 1.0f / (float)(1u << bm.uv_shift);
		int blend = (mb->flags & MAT_BLEND) != 0, atest = (mb->flags & MAT_ALPHA_TEST) != 0;
		if (s_opt.textures && mb->tex >= 0) {
			tex_image_t img;
			if (s_src.texture && s_src.texture(s_src.user, mb->tex, &img) == 0) tex0 = resident_tex0((unsigned)mb->tex, &img, 0);
		}
		if (tex0) {
			ad[n].data = tex0;                     ad[n++].addr = GS_A_TEX0_1;
			ad[n].data = GS_TEX1(1, 0, 1, 1, 0, 0, 0); ad[n++].addr = GS_A_TEX1_1;
			ad[n].data = GS_CLAMP(0, 0, 0, 0, 0, 0);   ad[n++].addr = GS_A_CLAMP_1;
		} else {
			s_stats.untextured_batches += s_mat_count[m];
			/* An untextured blended material (glass and lights whose base map is a cube map, ADR-057) would add or blend a
			 * flat grey over what is behind it; drawing nothing is closer to the reference than the wash. */
			if (blend) continue;
		}
		ad[n].data = mb->add ? GS_ALPHA(0, 2, 2, 1, 0xFF) : GS_ALPHA(0, 1, 0, 1, 0); ad[n++].addr = GS_A_ALPHA_1;   /* glow materials add, doubled: the Xbox exhaust is white-hot, its map a 104 grey */
		ad[n].data = GS_TEST(atest && tex0 ? 1 : 0, 6, 0x40, 0, 0, 0, 1, 2); ad[n++].addr = GS_A_TEST_1;
		ad[n].data = GS_ZBUF(s_zbuf_page, GS_PSMZ24, 0); ad[n++].addr = GS_A_ZBUF_1;
		emit_state(c, ad, n, scale * mb->uv_scale[0], scale * mb->uv_scale[1],
		           GS_PRIM(GSX_PRIM_TRIANGLE, 1, tex0 ? 1 : 0, fog, blend, 0, 0, 0, 0));
		if (!(s_opt.debug & 1)) emit_batches(c, s_sorted + s_mat_start[m], s_mat_count[m], entry_lit);
		emit_clipped(c, m, 0, GS_PRIM(GSX_PRIM_TRIANGLE, 1, tex0 ? 1 : 0, fog, blend, 0, 0, 0, 0), &cgs);
		s_stats.materials_drawn++;
		if ((i & 31) == 31) bsp_render_poll();
	}

	/* pass 2: lightmap multiply, grouped by lightmap page */
	if (s_opt.lightmaps && s_src.lightmap && s_page_slots && !(s_opt.debug & 2)) {
		u32 np = s_page_slots, p;
		memset(s_page_count, 0, 4u * np);
		for (i = 0; i < nused; i++) {
			unsigned m = s_used[i];
			bsp_material bm = bsp_get_material(s_bsp, m);
			if (bm.lightmap_page != BSP_NO_LIGHTMAP && !(s_bind[m].flags & MAT_BLEND)) s_page_count[bm.lightmap_page] += s_mat_count[m];
		}
		for (p = 0, pos = 0; p < np; p++) { s_page_start[p] = s_page_cursor[p] = pos; pos += s_page_count[p]; }
		group_clipped(1);
		for (i = 0; i < nused; i++) {
			unsigned m = s_used[i];
			bsp_material bm = bsp_get_material(s_bsp, m);
			u32 k;
			if (bm.lightmap_page == BSP_NO_LIGHTMAP || (s_bind[m].flags & MAT_BLEND)) continue;
			for (k = 0; k < s_mat_count[m]; k++) s_sorted2[s_page_cursor[bm.lightmap_page]++] = s_sorted[s_mat_start[m] + k];
		}
		for (p = 0; p < np; p++) {
			tex_image_t img;
			u64 tex0 = 0;
			if (!s_page_count[p]) continue;
			if (s_src.lightmap(s_src.user, p, &img) == 0) tex0 = resident_tex0(s_src.texture_count + p, &img, 1);
			if (!tex0) continue;
			ad[0].data = tex0;                            ad[0].addr = GS_A_TEX0_1;
			ad[1].data = GS_TEX1(1, 0, 1, 1, 0, 0, 0);    ad[1].addr = GS_A_TEX1_1;
			ad[2].data = GS_CLAMP(1, 1, 0, 0, 0, 0);      ad[2].addr = GS_A_CLAMP_1;
			ad[3].data = GS_ALPHA(1, 2, 0, 2, 0);         ad[3].addr = GS_A_ALPHA_1;      /* dest * lightmap alpha */
			ad[4].data = GS_TEST(0, 1, 0, 0, 0, 0, 1, 2); ad[4].addr = GS_A_TEST_1;
			ad[5].data = GS_ZBUF(s_zbuf_page, GS_PSMZ24, 1); ad[5].addr = GS_A_ZBUF_1;    /* Z read-only */
			emit_state(c, ad, 6, 1.0f / 32767.0f, 1.0f / 32767.0f, GS_PRIM(GSX_PRIM_TRIANGLE, 1, 1, 0, 1, 0, 0, 0, 0));
			emit_batches(c, s_sorted2 + s_page_start[p], s_page_count[p], entry_lm);
			emit_clipped(c, p, 1, GS_PRIM(GSX_PRIM_TRIANGLE, 1, 1, 0, 1, 0, 0, 0, 0), &cgs);
			if ((p & 7) == 7) bsp_render_poll();
		}
	}

	if (s_obj_pass) {
		render_ctx_t rc;
		rc.chain = c; rc.cam = cam; rc.clip = (const float (*)[4])clip; rc.frustum = &fr; rc.frame = frame; rc.opt = &s_opt; rc.sky_visible = s_vis.sky_visible; rc.sky_index = s_vis.sky_index;
		s_obj_pass(s_obj_user, &rc);
	}
	if (s_overlay_pass) {
		render_ctx_t rc;
		rc.chain = c; rc.cam = cam; rc.clip = (const float (*)[4])clip; rc.frustum = &fr; rc.frame = frame; rc.opt = &s_opt; rc.sky_visible = s_vis.sky_visible; rc.sky_index = s_vis.sky_index;
		s_overlay_pass(s_overlay_user, &rc);
	}

	/* overlay + FINISH */
	{
		u32 *w;
		u64 *q;
		if (overlay && !(s_opt.debug & 4)) {
			u32 atlas_block = (s_window_base + FONT_PAGE * PAGE_WORDS) / 64, clut_block = s_window_base / 64 + FONT_CLUT_SLOT * 4;
			/* the text emitter takes the pending MSCAL through the first tag it writes */
			u64 pend = take_pending();
			if (pend) { w = chain_cnt(c, 1, pend); if (w) memset(w, 0, 16); }
			text_batch_emit(overlay, c, atlas_block, clut_block, VIDEO_WIDTH, VIDEO_HEIGHT);
		}
		w = chain_cnt(c, 1 + 2, take_pending());
		if (w) {
			w[0] = VIF_CODE(VIF_NOP, 0, 0); w[1] = VIF_CODE(VIF_NOP, 0, 0);
			w[2] = VIF_CODE(VIF_FLUSHA, 0, 0); w[3] = VIF_CODE(VIF_DIRECT, 0, 2);
			q = (u64 *)(w + 4);
			q[0] = GIF_TAG(1, 1, 0, 0, GIFX_FLG_PACKED, 1); q[1] = GIF_REGS_AD;
			q[2] = 0; q[3] = GS_A_FINISH;
		}
		chain_end(c, 0);
	}
	chain_end(&s_upload[s_build], 0);

	s_stats.batches = s_vis.batch_count;
	s_stats.triangles = s_vis.triangles;
	s_stats.clusters_visible = s_vis.cluster_count;
	s_stats.clusters_total = s_bsp->cluster_count;
	s_stats.portals_tested = s_vis.portals_tested;
	s_stats.portals_passed = s_vis.portals_passed;
	s_stats.camera_cluster_valid = s_vis.camera_cluster >= 0;
	s_stats.tex_hits = s_tc.hits; s_stats.tex_misses = s_tc.misses;
	s_stats.tex_deferred = s_tc.deferred; s_stats.tex_failed = s_tc.failed;
	s_stats.pages_free = tc_pages_free(&s_tc);
	s_stats.chain_qw = chain_qwords(c);
	s_stats.chain_overflow = c->overflow || s_upload[s_build].overflow;
	s_stats.build_ms = timer_ms(t0, timer_cycles());
}

void bsp_render_kick(video_t *v)
{
	chain_t *c = &s_chain[s_build];
	*GS_CSR = 2;                                         /* clear FINISH */
	packet2_reset(s_env, 0);
	video_add_setup_env(v, s_env);
	video_add_sky(v, s_env, s_opt.sky_rgb, s_opt.horizon_rgb, s_horizon_y);
	/* The draw_* helpers leave the last GIF tag open (EOP = 0); PATH2 (VIF1 DIRECT) cannot start
	 * until the PATH3 packet ends, so close it with an A+D NOP. */
	packet2_add_u64(s_env, GIF_TAG(1, 1, 0, 0, GIFX_FLG_PACKED, 1));
	packet2_add_u64(s_env, GIF_REGS_AD);
	packet2_add_u64(s_env, 0);
	packet2_add_u64(s_env, 0x7F);
	dma_channel_send_packet2(s_env, DMA_CHANNEL_GIF, 1);
	dma_wait_fast();
	if (chain_qwords(&s_upload[s_build]) > 1) {
		dma_channel_send_chain_ucab(DMA_CHANNEL_GIF, s_upload_mem[s_build], 0, 0);
		dma_wait_fast();
	}
	s_kick_cycles = timer_cycles();
	s_vif_seen = s_gs_seen = 0;
	dma_channel_send_chain_ucab(DMA_CHANNEL_VIF1, c->base, 0, DMA_FLAG_TRANSFERTAG);
}

void bsp_render_poll(void)
{
	if (!s_vif_seen && !(*D1_CHCR & 0x100)) { s_vif_seen = 1; s_vif_cycles = timer_cycles(); }
	if (!s_gs_seen && (*GS_CSR & 2)) { s_gs_seen = 1; s_gs_cycles = timer_cycles(); }
}

void bsp_render_timing(float *vif_ms, float *gs_ms)
{
	*vif_ms = s_vif_seen ? timer_ms(s_kick_cycles, s_vif_cycles) : -1.0f;
	*gs_ms = s_gs_seen ? timer_ms(s_kick_cycles, s_gs_cycles) : -1.0f;
}

const render_stats_t *bsp_render_stats(void) { return &s_stats; }

u32 bsp_render_cache_pages(void) { return s_window_pages - CACHE_FIRST_PAGE; }

void bsp_render_wait_gpu(void)
{
	u32 t0 = timer_cycles();
	while (!s_gs_seen && timer_ms(t0, timer_cycles()) < 40.0f) bsp_render_poll();
}

/* ---- object pass hooks --------------------------------------------------------------------------- */
u64  rb_take_pending(void) { return take_pending(); }
void rb_set_pending(u64 pair) { s_pending = pair; }
u32  rb_model_entry(void) { return entry_addr(&VU1Model_Entry); }
const u32 *rb_zbuf_page_ptr(void) { return &s_zbuf_page; }

void rb_fog_params(float *k, float *b)
{
	*k = 0.0f; *b = 4080.0f;
	if (s_opt.fog && s_opt.fog_far > s_opt.fog_near) {
		*k = -4080.0f / (s_opt.fog_far - s_opt.fog_near);
		*b = 4080.0f - s_opt.fog_near * *k;
	}
}

u64 rb_object_tex0(unsigned index, const tex_image_t *img, u64 tfx)
{
	return resident_tex0(s_obj_key_base + index, img, tfx);
}

void bsp_render_set_object_pass(render_pass_fn fn, void *user, const tex_source_t *obj_src)
{
	s_obj_pass = fn;
	s_obj_user = user;
	if (obj_src) {
		s_obj_src = *obj_src;
		s_obj_key_base = s_src.texture_count + KEY_LM_SLOTS;          /* stable across BSP switches (lightmap keys come first) */
		s_obj_count = obj_src->texture_count;
		/* the residency cache keys are textures, lightmap pages, then object textures */
		tc_init(&s_tc, s_window_pages - CACHE_FIRST_PAGE, FONT_CLUT_SLOT, s_obj_key_base + obj_src->texture_count + s_extra_total, 1536u * 1024u);
	}
}

unsigned bsp_render_reserve_keys(unsigned count)
{
	unsigned base = s_obj_key_base + s_obj_count + s_extra_total;
	s_extra_total += count;
	tc_init(&s_tc, s_window_pages - CACHE_FIRST_PAGE, FONT_CLUT_SLOT, s_obj_key_base + s_obj_count + s_extra_total, 1536u * 1024u);
	return base;
}

u64 rb_extra_tex0(unsigned key_base, unsigned index, const tex_image_t *img, u64 tfx)
{
	return resident_tex0(key_base + index, img, tfx);
}

void bsp_render_set_overlay_pass(render_pass_fn fn, void *user)
{
	s_overlay_pass = fn;
	s_overlay_user = user;
}

void bsp_render_set_sky_pass(render_pass_fn fn, void *user)
{
	s_sky_pass = fn;
	s_sky_user = user;
}
