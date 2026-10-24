#include "video.h"
#include <graph.h>
#include <gs_psm.h>
#include <dma.h>
#include <stdio.h>

static int s_vram_used;

void video_init(video_t *v)
{
	int i;
	for (i = 0; i < 2; i++) {
		v->fb[i].width  = VIDEO_WIDTH;
		v->fb[i].height = VIDEO_HEIGHT;
		v->fb[i].mask   = 0;
		v->fb[i].psm    = GS_PSM_16S;
		v->fb[i].address = graph_vram_allocate(VIDEO_WIDTH, VIDEO_HEIGHT, GS_PSM_16S, GRAPH_ALIGN_PAGE);
	}
	v->z.enable  = DRAW_ENABLE;
	v->z.mask    = 0;
	v->z.method  = ZTEST_METHOD_GREATER_EQUAL;
	v->z.zsm     = GS_ZBUF_24;
	v->z.address = graph_vram_allocate(VIDEO_WIDTH, VIDEO_HEIGHT, GS_ZBUF_24, GRAPH_ALIGN_PAGE);
	v->back   = 0;
	v->frames = 0;
	v->tex_base = graph_vram_allocate(64, 32, GS_PSM_32, GRAPH_ALIGN_PAGE);   /* first free page */

	/* Bytes consumed by colour + Z buffers; the rest of the 4 MB is the texture window. */
	s_vram_used = 2 * (VIDEO_WIDTH * VIDEO_HEIGHT * 2) + (VIDEO_WIDTH * VIDEO_HEIGHT * 4);

	/* Interlaced NTSC, full-height buffer, CRTC reads alternate lines each field. */
	graph_set_mode(GRAPH_MODE_INTERLACED, GRAPH_MODE_NTSC, GRAPH_MODE_FIELD, GRAPH_ENABLE);
	graph_set_screen(0, 0, VIDEO_WIDTH, VIDEO_HEIGHT);
	graph_set_bgcolor(0, 0, 0);
	graph_set_framebuffer_filtered(v->fb[1].address, VIDEO_WIDTH, GS_PSM_16S, 0, 0);
	graph_enable_output();

	printf("[video] fb0=%u fb1=%u z=%u tex_base=%u (units as returned by graph_vram_allocate)\n", (unsigned)v->fb[0].address, (unsigned)v->fb[1].address, (unsigned)v->z.address, (unsigned)v->tex_base);
	printf("[video] %dx%d CT16S x2 + Z24, VRAM used %d KB, texture window %u KB\n",
	       VIDEO_WIDTH, VIDEO_HEIGHT, s_vram_used / 1024, video_texture_window_bytes() / 1024);
}

u32 video_texture_window_bytes(void)
{
	return (4u * 1024u * 1024u) - (u32)s_vram_used;
}

void video_add_setup_env(video_t *v, packet2_t *p)
{
	packet2_update(p, draw_setup_environment(p->next, 0, &v->fb[v->back], &v->z));
	packet2_update(p, draw_primitive_xyoffset(p->next, 0, 2048 - VIDEO_WIDTH / 2, 2048 - VIDEO_HEIGHT / 2));
	/* The colour buffer is 16 bit (5-5-5): ordered dithering (DTHE + the 4x4 matrix) turns the steps of a dark gradient (the space sky, fog, lightmaps) into fine noise.
	 * Free on the GS: the dither is applied in the pixel pipeline when the 8-bit result is truncated. */
	{
		static const char matrix[16] = { -4, 3, -2, 1,   2, -1, 0, -3,   -1, 0, -4, 3,   1, -3, 2, -2 };
		packet2_update(p, draw_dither_matrix(p->next, (char *)matrix));
		packet2_update(p, draw_dithering(p->next, 1));
	}
}

void video_add_clear(video_t *v, packet2_t *p, u8 r, u8 g, u8 b)
{
	packet2_update(p, draw_disable_tests(p->next, 0, &v->z));
	packet2_update(p, draw_clear(p->next, 0, 2048.0f - VIDEO_WIDTH / 2, 2048.0f - VIDEO_HEIGHT / 2,
	                             VIDEO_WIDTH, VIDEO_HEIGHT, r, g, b));
	packet2_update(p, draw_enable_tests(p->next, 0, &v->z));
}

void video_add_sky(video_t *v, packet2_t *p, const u8 top[3], const u8 horizon[3], int horizon_y)
{
	/* two Gouraud quads as a triangle strip each; z = 0 with the test off clears the depth buffer too */
	int ox = 2048 - VIDEO_WIDTH / 2, oy = 2048 - VIDEO_HEIGHT / 2, q, k;
	int ys[3];
	const u8 *cols[3];
	if (horizon_y < 0) horizon_y = 0;
	if (horizon_y > VIDEO_HEIGHT) horizon_y = VIDEO_HEIGHT;
	ys[0] = 0; ys[1] = horizon_y; ys[2] = VIDEO_HEIGHT;
	cols[0] = top; cols[1] = horizon; cols[2] = horizon;
	packet2_update(p, draw_disable_tests(p->next, 0, &v->z));
	for (q = 0; q < 2; q++) {
		if (ys[q + 1] <= ys[q]) continue;
		/* GIF tag: PRIM = triangle strip (4), IIP; NLOOP 4; regs RGBAQ, XYZ2; EOP = 0 */
		packet2_add_u64(p, (u64)4 | (u64)1 << 46 | (u64)(4 | 8) << 47 | (u64)2 << 60);
		packet2_add_u64(p, 0x51);
		for (k = 0; k < 4; k++) {
			int y = ys[q + (k >> 1)], x = (k & 1) ? VIDEO_WIDTH : 0;
			const u8 *c = cols[q + (k >> 1)];
			packet2_add_u32(p, c[0]); packet2_add_u32(p, c[1]); packet2_add_u32(p, c[2]); packet2_add_u32(p, 0x80);
			packet2_add_u32(p, (u32)((ox + x) * 16) & 0xFFFF); packet2_add_u32(p, (u32)((oy + y) * 16) & 0xFFFF);
			packet2_add_u32(p, 0); packet2_add_u32(p, 0);
		}
	}
	packet2_update(p, draw_enable_tests(p->next, 0, &v->z));
}

void video_wait_gpu(void)
{
	draw_wait_finish();
}

void video_present(video_t *v)
{
	graph_wait_vsync();
	graph_set_framebuffer_filtered(v->fb[v->back].address, VIDEO_WIDTH, GS_PSM_16S, 0, 0);
	v->back ^= 1;
	v->frames++;
}

void video_flip(video_t *v)
{
	video_wait_gpu();
	video_present(v);
}
