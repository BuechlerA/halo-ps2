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
#include "render/lodctl.h"
#include "render/obj_render.h"
#include "sim/fpview.h"
#include "sim/objects.h"
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
static object_table_t s_objects;
static fpview_t   s_fp;
static lodctl_t   s_lod;
static obj_render_t s_objr;
static int        s_have_objects;
static int        s_vblanks = 2;                /* vblanks per frame: 2 = 30 fps (default), 1 = 60 fps */
#define VBLANK_S (1.0f / 59.94f)
#define VBLANK_MS (1000.0f / 59.94f)

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
	if (cfg_int(&s_cfg, "skyfog", 0) && s_world.sky.valid && s_world.sky.fog_far > s_world.sky.fog_near && s_world.sky.fog_density > 0.01f) {
		/* the sky tag's fog: colour, and a linear ramp that reaches full strength at the opaque distance / density */
		o.fog = 1;
		o.fog_near = s_world.sky.fog_near;
		o.fog_far = s_world.sky.fog_far / s_world.sky.fog_density;
		o.fog_rgb[0] = (u8)(s_world.sky.fog_rgb[0] * 255.0f); o.fog_rgb[1] = (u8)(s_world.sky.fog_rgb[1] * 255.0f); o.fog_rgb[2] = (u8)(s_world.sky.fog_rgb[2] * 255.0f);
	}
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
	{
		const char *obj = cfg_str(&s_cfg, "obj", NULL);
		if (obj && world_load_objects(&s_world, &s_arena, obj) == 0) {
			object_world_t ow;
			ow.scen = &s_world.scen; ow.models = &s_world.models; ow.anims = s_world.anims.graph_count ? &s_world.anims : NULL;
			objects_clear(&s_objects);
			objects_spawn_bsp(&s_objects, &ow, (unsigned)s_world.bsp_index, (int)cfg_int(&s_cfg, "objhidden", 0));
			printf("[obj] BSP %d: %u objects placed (skipped: %u no model, %u hidden, %u other BSP, %u over capacity)\n", s_world.bsp_index,
			       s_objects.count, s_objects.skipped_no_model, s_objects.skipped_hidden, s_objects.skipped_other_bsp, s_objects.skipped_cap);
			{
				scen_player pl = scen_get_player(&s_world.scen);
				fpview_init(&s_fp, &ow, &pl);
				if (!cfg_int(&s_cfg, "fpanim", 1)) s_fp.arms_anim = s_fp.weapon_anim = SCEN_NO_ANIM;
				printf("[obj] first person: %s\n", s_fp.active ? "arms + weapon" : "none");
			}
			if (cfg_int(&s_cfg, "objlist", 0)) {
				unsigned oi;
				for (oi = 0; oi < s_objects.count; oi++) {
					const object_t *ob = &s_objects.obj[oi];
					scen_def d = scen_get_def(ow.scen, (uint32_t)ob->def);
					printf("[obj] #%u type %u %s at %.1f %.1f %.1f anim %u\n", oi, ob->type, d.name, ob->pos[0], ob->pos[1], ob->pos[2], (unsigned)ob->anim);
				}
			}
			{
				float cp[5];
				if (cfg_floats(&s_cfg, "chief", cp, 4) == 4) {            /* third-person Master Chief: x y z yaw(deg) */
					scen_player pl = scen_get_player(&s_world.scen);
					float pos[3] = { cp[0], cp[1], cp[2] };
					int idx = pl.biped_def ? objects_spawn_def(&s_objects, &ow, pl.biped_def - 1, pos, cp[3] * 3.14159265f / 180.0f) : -1;
					printf("[obj] third-person biped: %s\n", idx >= 0 ? "spawned" : "not available");
				}
			}
			memset(&s_objr, 0, sizeof s_objr);
			s_objr.table = &s_objects;
			s_objr.world = ow;
			s_objr.bsp = &s_world.bsp;
			s_objr.bsp_index = s_world.bsp_index;
			s_objr.textures = &s_world.obj_source;
			s_objr.fp = &s_fp;
			s_objr.lod = &s_lod;
			if (!cfg_int(&s_cfg, "objanim", 1)) { unsigned oi; for (oi = 0; oi < s_objects.count; oi++) s_objects.obj[oi].flags &= ~OBJ_F_ANIMATED; }
			s_objr.max_distance = (float)cfg_int(&s_cfg, "objdist", 200);
			s_objr.max_batches = (unsigned)cfg_int(&s_cfg, "objcap", 900);
			s_objr.draw_objects = (int)cfg_int(&s_cfg, "objects", 1);
			s_objr.draw_fp = (int)cfg_int(&s_cfg, "fp", 1);
			s_objr.sky_model = -1;
			if (cfg_int(&s_cfg, "sky3d", 1) && s_world.sky.valid && s_world.sky.model_tag) s_objr.sky_model = model_find(&s_world.models, s_world.sky.model_tag);
			printf("[obj] sky model: %s\n", s_objr.sky_model >= 0 ? "drawn" : "none");
			bsp_render_set_object_pass(obj_render_pass, &s_objr, &s_world.obj_source);
			bsp_render_set_sky_pass(obj_render_sky, &s_objr);
			apply_options();                                     /* sky-derived fog needs the sky data */
			s_have_objects = 1;
		}
	}
	printf("[mem] level arena: %u KB used of %u KB\n", (unsigned)(s_arena.used / 1024), (unsigned)(s_arena.cap / 1024));
	setup_camera();
	if (s_have_objects && cfg_int(&s_cfg, "chiefcam", 0)) {            /* Master Chief 3 units ahead of the start camera, facing it */
		scen_player pl = scen_get_player(&s_world.scen);
		float r[3], u[3], f[3], pos[3];
		camera_basis(&s_cam, r, u, f);
		pos[0] = s_cam.pos[0] + f[0] * 3.0f; pos[1] = s_cam.pos[1] + f[1] * 3.0f; pos[2] = s_cam.pos[2] - 1.4f;
		printf("[obj] third-person biped: %s\n", pl.biped_def && objects_spawn_def(&s_objects, &s_objr.world, pl.biped_def - 1, pos, s_cam.yaw + 3.14159265f) >= 0 ? "spawned" : "not available");
	}
	s_vblanks = cfg_int(&s_cfg, "fps", 30) >= 60 ? 1 : 2;
	printf("[cfg] target %d fps (%d vblank(s) per frame)\n", 60 / s_vblanks, s_vblanks);
	profiler_init(&s_prof, ((float)s_vblanks + 0.5f) * VBLANK_MS);   /* late by more than half a frame time = dropped */
	lodctl_init(&s_lod, 33.3f / (float)(3 - s_vblanks), 60 / s_vblanks);
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

		if (s_have_objects) {
			if (s_vblanks == 2 || (frame & 1) == 0) {         /* the 30 Hz tick: every frame at 30 fps, every other at 60 */
				objects_tick(&s_objects, &s_objr.world);
				fpview_tick(&s_fp, &s_objr.world);
			}
			fpview_place(&s_fp, &s_objr.world, &s_cam);
		}
		if ((frame % 6) == 0) profiler_overlay(&s_prof, &s_text);   /* a few updates a second: float formatting is slow */
		bsp_render_build(frame, &s_cam, &s_text);          /* overlaps the GS drawing frame-1 */
		t1 = timer_cycles();
		bsp_render_wait_gpu();
		video_wait_gpu();                                  /* FINISH seen: burning vsyncs below clears the flag */
		{
			/* Present exactly s_vblanks vblanks after the previous flip (30 fps: 2). flip() itself waits for the next
			 * vsync; if the build finished before the vsync that is already (s_vblanks - 1) vblanks after the last
			 * flip, burn the vsyncs that have not happened yet. A build that ran long just lands on a later vsync. */
			int passed = (int)(timer_ms(s_prof.last_flip, timer_cycles()) / VBLANK_MS), v;
			for (v = passed; v < s_vblanks - 1; v++) graph_wait_vsync();
		}
		video_present(&s_video);                           /* vsync + display */
		t2 = timer_cycles();

		profiler_frame(&s_prof, timer_ms(t0, t1), timer_ms(t1, t2), bsp_render_stats(), t2);
		if (s_have_objects) {
			s_prof.obj_visible = s_objr.st_visible; s_prof.obj_culled = s_objr.st_culled;
			s_prof.obj_batches = s_objr.st_batches; s_prof.obj_tris = s_objr.st_triangles; s_prof.lod_level = (unsigned)s_lod.level;
			s_prof.obj_ms = s_objr.st_ms;
			if (cfg_int(&s_cfg, "lodctl", 1) && frame > 10) {
				float gs = s_prof.gs_ms > 0.0f ? s_prof.gs_ms : 0.0f, ee = timer_ms(t0, t1);
				lodctl_frame(&s_lod, gs > ee ? gs : ee, s_prof.last_interval > s_prof.drop_ms);
			}
		}
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
