/* halo-ps2 — Phase 1: static world at 30 fps (ADR-023; `fps 60` selects the stretch target). Pipelined frame loop: the EE builds frame N+1
 * (visibility, sort, DMA chain) while the GS draws frame N; ticks run every other vblank (ADR-002).
 * Boot options come from the optional host:halo.cfg (see core/config.h and docs/04-roadmap.md). */
#include <kernel.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dma.h>
#include <graph.h>
#include <gs_psm.h>
#include <packet2.h>
#include <draw.h>
#include <math.h>

#include "core/config.h"
#include "core/mem.h"
#include "platform/fbdump.h"
#include "platform/pad.h"
#include "platform/profiler.h"
#include "platform/timer.h"
#include "platform/video.h"
#include "render/bsp_render.h"
#include "render/flycam.h"
#include "render/selftest.h"
#include "world/world.h"

static video_t    s_video;
static cfg_t      s_cfg;
static arena_t    s_arena;
static world_t    s_world;
static camera_t   s_cam;
static cam_path   s_path;
static profiler_t s_prof;
static text_batch_t s_text;
static int        s_vblanks = 2;                /* vblanks per frame: 2 = 30 fps (default), 1 = 60 fps */
#define VBLANK_S (1.0f / 59.94f)

static void load_config(void)
{
	static char text[4096];
	FILE *f = fopen("host:halo.cfg", "rb");
	size_t n;
	if (!f) { printf("[cfg] no host:halo.cfg, defaults\n"); return; }
	n = fread(text, 1, sizeof text, f);
	fclose(f);
	printf("[cfg] host:halo.cfg: %u bytes, %d ignored line(s)\n", (unsigned)n, cfg_parse(&s_cfg, text, n));
}

static int fbdump_wanted(u32 frame)
{
	int i;
	const char *v;
	for (i = 0; (v = cfg_nth(&s_cfg, "fbdump", i)) != NULL; i++)
		if ((u32)strtoul(v, NULL, 0) == frame) return 1;
	return 0;
}

static void apply_options(void)
{
	render_options_t o;
	float f[6];
	bsp_render_options(&o);
	o.textures = (int)cfg_int(&s_cfg, "textures", 1);
	o.lightmaps = (int)cfg_int(&s_cfg, "lightmaps", 1);
	o.debug = (int)cfg_int(&s_cfg, "debug", 0);
	if (cfg_floats(&s_cfg, "onlybatch", f, 2) >= 1) { o.only_batch = (int)f[0]; o.only_batch_hi = cfg_floats(&s_cfg, "onlybatch", f, 2) == 2 ? (int)f[1] : (int)f[0]; }
	if (cfg_floats(&s_cfg, "clipbudget", f, 1) == 1) o.clip_budget_ms = f[0];
	if (cfg_floats(&s_cfg, "fog", f, 6) >= 1 && f[0] != 0.0f) {
		o.fog = 1;
		if (cfg_floats(&s_cfg, "fog", f, 6) >= 3) { o.fog_near = f[1]; o.fog_far = f[2]; }
		if (cfg_floats(&s_cfg, "fog", f, 6) >= 6) { o.fog_rgb[0] = (u8)f[3]; o.fog_rgb[1] = (u8)f[4]; o.fog_rgb[2] = (u8)f[5]; }
	}
	if (cfg_floats(&s_cfg, "sky", f, 3) == 3) {
		o.sky_rgb[0] = o.horizon_rgb[0] = (u8)f[0]; o.sky_rgb[1] = o.horizon_rgb[1] = (u8)f[1]; o.sky_rgb[2] = o.horizon_rgb[2] = (u8)f[2];
	}
	if (cfg_floats(&s_cfg, "horizon", f, 3) == 3) { o.horizon_rgb[0] = (u8)f[0]; o.horizon_rgb[1] = (u8)f[1]; o.horizon_rgb[2] = (u8)f[2]; }
	bsp_render_set_options(&o);
}

static void setup_camera(void)
{
	float f[5];
	const float *b = s_world.bsp.bounds;
	camera_init(&s_cam);
	s_cam.far_z = 4000.0f;
	if (cfg_floats(&s_cfg, "camera", f, 5) >= 3) {
		s_cam.pos[0] = f[0]; s_cam.pos[1] = f[1]; s_cam.pos[2] = f[2];
		s_cam.yaw = f[3] * 3.14159265f / 180.0f;
		s_cam.pitch = f[4] * 3.14159265f / 180.0f;
	} else {
		s_cam.pos[0] = 0.5f * (b[0] + b[3]); s_cam.pos[1] = 0.5f * (b[1] + b[4]); s_cam.pos[2] = 0.5f * (b[2] + b[5]);
	}
	if (campath_parse(&s_path, &s_cfg)) printf("[cam] scripted path: %d keys, %.1f s loop\n", s_path.count, campath_duration(&s_path));
}

/* GS FINISH must be pending before the first flip waits for it. */
static void prime_finish(void)
{
	packet2_t *p = packet2_create(2, P2_TYPE_NORMAL, P2_MODE_NORMAL, 0);
	packet2_update(p, draw_finish(p->next));
	dma_channel_send_packet2(p, DMA_CHANNEL_GIF, 1);
	dma_wait_fast();
	packet2_free(p);
}

int main(int argc, char *argv[])
{
	u32 frame = 0, limit;
	void *heap;
	size_t arena_bytes;
	const char *pack, *tex;

	setbuf(stdout, NULL);   /* ps2sdk stdout is not a tty: unbuffered so TTY logs appear immediately */
	printf("halo-ps2 phase 1 boot\n");
	load_config();

	arena_bytes = (size_t)cfg_int(&s_cfg, "arena_mb", 20) * 1024u * 1024u;
	heap = malloc(arena_bytes);                         /* the one boot-time allocation (ADR-006) */
	if (!heap) { printf("[err] cannot allocate %u MB arena\n", (unsigned)(arena_bytes >> 20)); return 1; }
	arena_init(&s_arena, "level", heap, arena_bytes);

	pad_init();
	dma_channel_initialize(DMA_CHANNEL_GIF, NULL, 0);
	dma_channel_fast_waits(DMA_CHANNEL_GIF);
	video_init(&s_video);
	if (bsp_render_init(&s_video)) return 1;

	if (cfg_int(&s_cfg, "selftest", 0)) {
		/* selftest 1: draw, dump the framebuffer over TTY, exit; selftest 2: also hold the picture on screen */
		selftest_draw(&s_video);
		graph_set_framebuffer_filtered(s_video.fb[s_video.back].address, VIDEO_WIDTH, GS_PSM_16S, 0, 0);
		if (cfg_int(&s_cfg, "fbdump_selftest", 1)) fbdump_frame(&s_video.fb[s_video.back], 0);
		printf("[done] selftest\n");
		if (cfg_int(&s_cfg, "selftest", 0) >= 2) for (;;) graph_wait_vsync();
		return 0;
	}
	pack = cfg_str(&s_cfg, "pack", "assets/hp2/a10.hp2");
	tex = cfg_str(&s_cfg, "tex", NULL);
	if (world_load(&s_world, &s_arena, pack, tex, (int)cfg_int(&s_cfg, "bsp", 0))) return 1;
	apply_options();
	if (bsp_render_set_world(&s_arena, &s_world.bsp, &s_world.source)) return 1;
	printf("[mem] level arena: %u KB used of %u KB\n", (unsigned)(s_arena.used / 1024), (unsigned)(s_arena.cap / 1024));
	setup_camera();
	s_vblanks = cfg_int(&s_cfg, "fps", 30) >= 60 ? 1 : 2;
	printf("[cfg] target %d fps (%d vblank(s) per frame)\n", 60 / s_vblanks, s_vblanks);
	profiler_init(&s_prof, 25.0f * (float)s_vblanks);
	s_prof.overlay = (int)cfg_int(&s_cfg, "overlay", 1);
	prime_finish();

	limit = (u32)cfg_int(&s_cfg, "frames", 0);
	for (;;) {
		u32 t0 = timer_cycles(), t1, t2;
		fly_input in;
		unsigned down = pad_poll(&in);

		if (down & PAD_BTN_START) s_prof.overlay ^= 1;
		if (s_path.count) campath_eval(&s_path, (float)frame * (float)s_vblanks * VBLANK_S, &s_cam);
		else flycam_update(&s_cam, &in, (float)s_vblanks * VBLANK_S);

		if ((frame % 6) == 0) profiler_overlay(&s_prof, &s_text);   /* a few updates a second: float formatting is slow */
		bsp_render_build(frame, &s_cam, &s_text);          /* overlaps the GS drawing frame-1 */
		t1 = timer_cycles();
		bsp_render_wait_gpu();
		video_flip(&s_video);                              /* GS finish of frame-1 + vsync + display */
		{ int v; for (v = 1; v < s_vblanks; v++) graph_wait_vsync(); }   /* 30 fps: hold the picture for the extra vblank(s) */
		t2 = timer_cycles();

		profiler_frame(&s_prof, timer_ms(t0, t1), timer_ms(t1, t2), bsp_render_stats(), t2);
		if (frame > 0 && fbdump_wanted(frame - 1)) {
			fbdump_frame(&s_video.fb[s_video.back ^ 1], frame - 1);   /* the buffer that was just displayed */
			s_prof.last_flip = timer_cycles();
		}
		if (limit && frame >= limit) {
			profiler_report(&s_prof);
			{
				const render_stats_t *rs = bsp_render_stats();
				printf("[mem] level arena %u KB of %u KB (high water %u KB); GS texture window: %u pages free of %u cache pages\n",
				       (unsigned)(s_arena.used / 1024), (unsigned)(s_arena.cap / 1024), (unsigned)(s_arena.high / 1024),
				       (unsigned)rs->pages_free, (unsigned)bsp_render_cache_pages());
			}
			printf("[done] frames=%u dropped=%u (%.2f%%)\n", s_prof.frames, s_prof.dropped,
			       100.0f * (float)s_prof.dropped / (float)(s_prof.frames ? s_prof.frames : 1));
			if (cfg_int(&s_cfg, "hold", 0)) {
				/* keep the last frame (with the profiler overlay) on screen: ps2link's TTY does not carry EE printf */
				u32 total_dropped = s_prof.dropped;
				for (;;) { (void)total_dropped; graph_wait_vsync(); }
			}
			return 0;
		}
		bsp_render_kick(&s_video);
		frame++;

		if ((frame % 60) == 0) {
			profiler_report(&s_prof);
			if (s_path.count == 0 && cfg_int(&s_cfg, "printcam", 0))
				printf("[cam] pos %.2f %.2f %.2f yaw %.1f pitch %.1f\n", s_cam.pos[0], s_cam.pos[1], s_cam.pos[2],
				       s_cam.yaw * 57.29578f, s_cam.pitch * 57.29578f);
		}
	}
	return 0;
}
