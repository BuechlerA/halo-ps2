#include "vu1_tri.h"
#include <string.h>
#include <stdio.h>
#include <math3d.h>
#include <dma.h>
#include <packet2.h>
#include <packet2_utils.h>
#include <graph.h>
#include <draw.h>
#include <draw3d.h>

extern u32 VU1Tri_CodeStart __attribute__((section(".vudata")));
extern u32 VU1Tri_CodeEnd   __attribute__((section(".vudata")));

/* VU1 data memory: matrix at 0..3, double buffers at 8 and 8+496 (see tri.vsm). */
#define VU1_DBUF_BASE   8
#define VU1_DBUF_OFFSET 496

#define TRI_VERTS 3

static VECTOR s_verts[TRI_VERTS] __attribute__((aligned(16))) = {
	{  0.0f,  40.0f, 0.0f, 1.0f },
	{ 40.0f, -30.0f, 0.0f, 1.0f },
	{-40.0f, -30.0f, 0.0f, 1.0f },
};
/* PACKED RGBAQ: one 8-bit component in the low byte of each 32-bit lane. */
static u32 s_colors[TRI_VERTS][4] __attribute__((aligned(16))) = {
	{ 0x80, 0x10, 0x10, 0x80 },
	{ 0x10, 0x80, 0x10, 0x80 },
	{ 0x10, 0x10, 0x80, 0x80 },
};

static packet2_t *s_vif[2];
static packet2_t *s_header;     /* scale + count + GIF tag, constant */
static int        s_ctx;
static MATRIX     s_view_screen;
static VECTOR     s_rot = { 0.0f, 0.0f, 0.0f, 1.0f };
static VECTOR     s_pos = { 0.0f, 0.0f, 0.0f, 1.0f };
static VECTOR     s_cam_pos = { 0.0f, 0.0f, 120.0f, 1.0f };
static VECTOR     s_cam_rot = { 0.0f, 0.0f, 0.0f, 1.0f };

static void upload_microprogram(void)
{
	u32 n = packet2_utils_get_packet_size_for_program(&VU1Tri_CodeStart, &VU1Tri_CodeEnd) + 1;
	packet2_t *p = packet2_create(n, P2_TYPE_NORMAL, P2_MODE_CHAIN, 1);
	packet2_vif_add_micro_program(p, 0, &VU1Tri_CodeStart, &VU1Tri_CodeEnd);
	packet2_utils_vu_add_end_tag(p);
	dma_channel_send_packet2(p, DMA_CHANNEL_VIF1, 1);
	dma_channel_wait(DMA_CHANNEL_VIF1, 0);
	packet2_free(p);
	printf("[vu1] microprogram uploaded: %u bytes (%u instruction pairs), %u MPG packet(s)\n",
	       (u32)&VU1Tri_CodeEnd - (u32)&VU1Tri_CodeStart,
	       ((u32)&VU1Tri_CodeEnd - (u32)&VU1Tri_CodeStart) / 8, n - 1);
}

static void set_double_buffer(void)
{
	packet2_t *p = packet2_create(2, P2_TYPE_NORMAL, P2_MODE_CHAIN, 1);
	packet2_utils_vu_add_double_buffer(p, VU1_DBUF_BASE, VU1_DBUF_OFFSET);
	packet2_utils_vu_add_end_tag(p);
	dma_channel_send_packet2(p, DMA_CHANNEL_VIF1, 1);
	dma_channel_wait(DMA_CHANNEL_VIF1, 0);
	packet2_free(p);
}

void vu1_tri_init(void)
{
	prim_t prim;
	dma_channel_initialize(DMA_CHANNEL_VIF1, NULL, 0);
	dma_channel_fast_waits(DMA_CHANNEL_VIF1);

	upload_microprogram();
	set_double_buffer();

	s_vif[0] = packet2_create(32, P2_TYPE_NORMAL, P2_MODE_CHAIN, 1);
	s_vif[1] = packet2_create(32, P2_TYPE_NORMAL, P2_MODE_CHAIN, 1);
	s_header = packet2_create(4, P2_TYPE_NORMAL, P2_MODE_CHAIN, 1);

	prim.type = PRIM_TRIANGLE;
	prim.shading = PRIM_SHADE_GOURAUD;
	prim.mapping = DRAW_DISABLE;
	prim.fogging = DRAW_DISABLE;
	prim.blending = DRAW_DISABLE;
	prim.antialiasing = DRAW_DISABLE;
	prim.mapping_type = PRIM_MAP_ST;
	prim.colorfix = PRIM_UNFIXED;

	/* qword 0: screen scale xyz + vertex count in w */
	packet2_add_float(s_header, 2048.0f);
	packet2_add_float(s_header, 2048.0f);
	packet2_add_float(s_header, ((float)0xFFFFFF) / 32.0f);   /* Z24 range after ftoi4 */
	packet2_add_s32(s_header, TRI_VERTS);
	/* qword 1: GIF tag, NLOOP = verts, regs RGBAQ + XYZ2 */
	packet2_utils_gs_add_prim_giftag(s_header, &prim, TRI_VERTS, DRAW_RGBAQ_REGLIST, 2, 0);

	create_view_screen(s_view_screen, graph_aspect_ratio(), -3.0f, 3.0f, -3.0f, 3.0f, 1.0f, 2000.0f);
}

void vu1_tri_update(float angle)
{
	s_rot[2] = angle;
}

void vu1_tri_draw(void)
{
	MATRIX local_world, world_view, local_screen;
	packet2_t *p = s_vif[s_ctx];
	u32 top = 0;

	create_local_world(local_world, s_pos, s_rot);
	create_world_view(world_view, s_cam_pos, s_cam_rot);
	create_local_screen(local_screen, local_world, world_view, s_view_screen);

	packet2_reset(p, 0);
	/* matrix → absolute address 0 (not relative to TOP) */
	packet2_utils_vu_add_unpack_data(p, 0, local_screen, 4, 0);
	/* header, vertices, colours → TOP-relative */
	packet2_utils_vu_add_unpack_data(p, top, s_header->base, packet2_get_qw_count(s_header), 1);
	top += packet2_get_qw_count(s_header);
	packet2_utils_vu_add_unpack_data(p, top, s_verts, TRI_VERTS, 1);
	top += TRI_VERTS;
	packet2_utils_vu_add_unpack_data(p, top, s_colors, TRI_VERTS, 1);
	top += TRI_VERTS;
	packet2_utils_vu_add_start_program(p, 0);
	packet2_utils_vu_add_end_tag(p);

	dma_channel_wait(DMA_CHANNEL_VIF1, 0);
	dma_channel_send_packet2(p, DMA_CHANNEL_VIF1, 1);
	s_ctx ^= 1;
}
