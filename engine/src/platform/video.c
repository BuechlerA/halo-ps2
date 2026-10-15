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

	/* Bytes consumed by colour + Z buffers; the rest of the 4 MB is the texture window. */
	s_vram_used = 2 * (VIDEO_WIDTH * VIDEO_HEIGHT * 2) + (VIDEO_WIDTH * VIDEO_HEIGHT * 4);

	/* Interlaced NTSC, full-height buffer, CRTC reads alternate lines each field. */
	graph_set_mode(GRAPH_MODE_INTERLACED, GRAPH_MODE_NTSC, GRAPH_MODE_FIELD, GRAPH_ENABLE);
	graph_set_screen(0, 0, VIDEO_WIDTH, VIDEO_HEIGHT);
	graph_set_bgcolor(0, 0, 0);
	graph_set_framebuffer_filtered(v->fb[1].address, VIDEO_WIDTH, GS_PSM_16S, 0, 0);
	graph_enable_output();

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
}

void video_add_clear(video_t *v, packet2_t *p, u8 r, u8 g, u8 b)
{
	packet2_update(p, draw_disable_tests(p->next, 0, &v->z));
	packet2_update(p, draw_clear(p->next, 0, 2048.0f - VIDEO_WIDTH / 2, 2048.0f - VIDEO_HEIGHT / 2,
	                             VIDEO_WIDTH, VIDEO_HEIGHT, r, g, b));
	packet2_update(p, draw_enable_tests(p->next, 0, &v->z));
}

void video_flip(video_t *v)
{
	draw_wait_finish();
	graph_wait_vsync();
	graph_set_framebuffer_filtered(v->fb[v->back].address, VIDEO_WIDTH, GS_PSM_16S, 0, 0);
	v->back ^= 1;
	v->frames++;
}
