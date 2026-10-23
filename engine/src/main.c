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
#include "audio/audio.h"
#include "audio/gameaudio.h"
#include "core/mem.h"
#include "platform/fbdump.h"
#include "platform/memcard.h"
#include "mission_app.h"
#include "audio/stream.h"
#include "stream/aread.h"
#include "stream/file.h"
#include "platform/pad.h"
#include "platform/profiler.h"
#include "platform/timer.h"
#include "platform/video.h"
#include "render/bsp_render.h"
#include "render/fx_render.h"
#include "render/hud_render.h"
#include "render/flycam.h"
#include "render/lodctl.h"
#include "render/obj_render.h"
#include "sim/fpview.h"
#include "sim/fx.h"
#include "sim/game.h"
#include "sim/input.h"
#include "sim/save.h"
#include "world/gamedata.h"
#include "sim/objects.h"
#include "render/selftest.h"
#include "world/world.h"
#include "world/hires.h"

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
static float s_cine_light[3] = { 1.0f, 1.0f, 1.0f }, s_cine_amb = 1.0f;     /* cinematic object lighting gains (cfg objlight / objamb) */
static obj_render_t s_objr;
/* Streamed high resolution textures (docs/37): the object set (<obj>.hires.hp2) and the BSP set (<tex>.thires.hp2) share one staging ring and each reads through its own IO thread. */
static hires_t s_hires, s_bhires;
static hires_ring_t s_hring;
static aread_t s_ar_obj, s_ar_bsp;
static int s_hires_open, s_bhires_open, s_ar_obj_open, s_ar_bsp_open;
static unsigned s_bhires_key, s_bhires_keyed;
static float s_cine_fxadd = 1.0f, s_cine_halo = 0.0f;
static uint32_t s_hires_loads = 1;
static float s_hires_px, s_bhires_ratio = 0.7f;
static char s_hires_path[160], s_bhires_path[160];

static int hires_file_read(void *user, uint32_t offset, void *dst, uint32_t bytes) { return file_read_at((const char *)user, (long)offset, dst, bytes); }
static int hires_ar_start(void *user, uint32_t offset, void *dst, uint32_t bytes) { return aread_start((aread_t *)user, offset, dst, bytes); }
static int hires_ar_poll(void *user) { return aread_poll((aread_t *)user); }
static int        s_have_objects;
static gamedata_t s_gd;
static game_t     s_game;
static input_script s_script;
static int        s_game_on;
static unsigned   s_tick;
static const char *s_pack, *s_tex;
static size_t     s_bsp_mark;
static int        s_gpu_idle;
static int        s_mission_done;
static gameaudio_t s_ga;
static hud_render_t s_hudr;
static fx_t       s_fx;
static checkpoint_t s_ckpt;
static hud_state_t s_hs;
static stream_player s_sp;
static stream_ee  s_see;
static int        s_stream_on;
static int        s_paused;
static uint32_t   s_unit_hud;
static fx_render_t s_fxr;
static float cine_near_floor = CAMERA_NEAR_DEFAULT;     /* cfg cinenear: the lowest near plane of a cinematic camera */

static void overlay_pass(void *u, struct render_ctx *rc)
{
	(void)u;
	fx_render_pass(&s_fxr, rc);          /* particles and decals are depth tested: before the HUD */
	hud_render_pass(&s_hudr, rc);
}
static int        s_vblanks = 2;                /* vblanks per frame: 2 = 30 fps (default), 1 = 60 fps */
#define VBLANK_S (1.0f / 59.94f)
#define VBLANK_MS (1000.0f / 59.94f)

static void load_config(void)
{
	static char text[16384];
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
	o.glow = (int)cfg_int(&s_cfg, "glow", 1);
	o.debug = (int)cfg_int(&s_cfg, "debug", 0);
	if (cfg_floats(&s_cfg, "onlybatch", f, 2) >= 1) { o.only_batch = (int)f[0]; o.only_batch_hi = cfg_floats(&s_cfg, "onlybatch", f, 2) == 2 ? (int)f[1] : (int)f[0]; }
	if (cfg_floats(&s_cfg, "clipbudget", f, 1) == 1) o.clip_budget_ms = f[0];
	o.clip_debug = (int)cfg_int(&s_cfg, "clipdebug", 0);
	{ float nf; cine_near_floor = cfg_floats(&s_cfg, "cinenear", &nf, 1) == 1 ? nf : CAMERA_NEAR_DEFAULT; }
	o.cull_sign = (int)cfg_int(&s_cfg, "cullsign", 1);      // single-sided surfaces are not drawn from behind (the camera inside a beam saw its inside)
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

#define FALL_KILL_MARGIN 30.0f      /* wu below the BSP's lowest vertex at which an airborne player counts as out of the world */

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

/* Writes the current checkpoint to the memory card slot and reads it back (debug round trip when `verify`). Returns 0 on success. */
static int card_save(int slot, int verify)
{
	size_t mark = arena_mark(&s_arena), cap = mapp_active() ? msave_pack_max() : checkpoint_pack_max(), n;
	uint8_t *buf = arena_alloc(&s_arena, cap, 64);
	int rc = -1;
	if (!buf) return -1;
	n = mapp_active() ? msave_pack(mapp_checkpoint(), buf, cap) : checkpoint_pack(&s_ckpt, buf, cap);
	if (n && memcard_save(slot, buf, n) == 0) {
		rc = 0;
		printf("[mc] saved slot %d: %u bytes\n", slot, (unsigned)n);
		if (verify) {
			long got = memcard_load(slot, buf, cap);
			static checkpoint_t back;
			rc = (got == (long)n && (mapp_active() ? 1 : checkpoint_unpack(&back, buf, (size_t)got)) && (mapp_active() ? 1 : back.tick == s_ckpt.tick)) ? 0 : -2;
			printf("[mc] read back %ld bytes: %s\n", got, rc == 0 ? "ok" : "MISMATCH");
		}
	}
	arena_release(&s_arena, mark);
	return rc;
}

static int load_bsp(int idx);

/* ---- checkpoints and mission requests ---- */
static void do_checkpoint_save(void)
{
	if (mapp_active()) mapp_checkpoint_save(); else checkpoint_save(&s_ckpt, &s_game, &s_fp, s_fx.def ? &s_fx : NULL);
	hud_message(&s_hs, "Checkpoint saved", 90);
	printf("[game] checkpoint saved at tick %u\n", s_game.tick);
}

static int do_revert(void)
{
	int ok;
	if (mapp_active()) {
		int b = mapp_checkpoint_bsp();
		if (b < 0) return 0;
		if (b != s_world.bsp_index) {
			if (!s_gpu_idle) { bsp_render_wait_gpu(); video_wait_gpu(); }
			s_gpu_idle = 1;
			if (load_bsp(b)) return 0;
			mapp_set_bsp(b);
		}
		ok = mapp_checkpoint_restore();
	} else ok = checkpoint_revert(&s_ckpt, &s_game, &s_objects, &s_fp, s_fx.def ? &s_fx : NULL);
	if (ok) { hud_message(&s_hs, "Reverted to the last checkpoint", 90); printf("[game] reverted to the checkpoint of tick %u\n", s_game.tick); }
	return ok;
}

/* `prof2 1`: ms spent in each part of the 30 Hz tick, summed per 60-frame window (the zones are exclusive; P2_N must match the names in the prof2 print) */
enum { P2_MAPP, P2_SERVE, P2_BSPSW, P2_STREAM, P2_FX, P2_AUDIO, P2_HUD, P2_N };
static float s_p2_ms[P2_N], s_p2_peak[P2_N];
static void p2_add(int zone, u32 t0)
{
	float ms = timer_ms(t0, timer_cycles());
	s_p2_ms[zone] += ms;
	if (ms > s_p2_peak[zone]) s_p2_peak[zone] = ms;
}

/* the mission asks for things the platform must do between ticks */
static void serve_mission(void)
{
	mission_t *mm = mapp_mission();
	if (mm->want_bsp >= 0) {
		int want = mm->want_bsp;
		mm->want_bsp = -1;
		if (want != s_world.bsp_index) {
			u32 t0 = timer_cycles();
			if (!s_gpu_idle) { bsp_render_wait_gpu(); video_wait_gpu(); }
			s_gpu_idle = 1;
			if (load_bsp(want) == 0) {
				mapp_set_bsp(want);
				if (mm->want_teleport) game_teleport(&s_game, mm->want_pos, mm->want_yaw);
			}
			printf("[mission] switched to BSP %d in %.0f ms\n", want, timer_ms(t0, timer_cycles()));
		}
	}
	if (mm->save_requested) { mm->save_requested = 0; do_checkpoint_save(); }
	if (mm->misc.revert_requested) { mm->misc.revert_requested = 0; do_revert(); }
	if (mm->won && !s_mission_done) { s_mission_done = 1; printf("[mission] WON at tick %u (%.1f minutes)\n", mm->tick, (double)mm->tick / 1800.0); mapp_log_status(); }
	if (mm->lost && !s_mission_done) { s_mission_done = 2; printf("[mission] LOST at tick %u\n", mm->tick); mapp_log_status(); }
}

/* (Re)loads BSP `idx` after the arena mark: render geometry, lightmap, collision; rebinds everything that points at BSP data. */
/* Debug (nearlog): the name of the animation an object is posed with. */
static const char *object_anim_label(const object_t *ob)
{
	anim_graph g;
	if (ob->graph < 0 || ob->anim == SCEN_NO_ANIM || !s_world.anims.graph_count || (uint32_t)ob->graph >= s_world.anims.graph_count) return "";
	g = anim_get_graph(&s_world.anims, (uint32_t)ob->graph);
	return ob->anim < g.anim_count ? anim_get(&s_world.anims, &g, ob->anim).name : "";
}

static int load_bsp(int idx);

/* Debug: `bspswitch <seconds> <bsp> <x> <y> <z> <yaw deg>` lines switch the loaded BSP and teleport the player (what the scripts' switch_bsp does). */
static void check_bsp_switch(void)
{
	int i;
	const char *v;
	for (i = 0; (v = cfg_nth(&s_cfg, "bspswitch", i)) != NULL; i++) {
		float f[6];
		char buf[CFG_ARG_LEN];
		strncpy(buf, v, sizeof buf - 1); buf[sizeof buf - 1] = 0;
		{ char *e = buf; int n; for (n = 0; n < 6; n++) { f[n] = strtof(e, &e); } }
		if ((unsigned)(f[0] * 30.0f) == s_tick && s_tick > 0) {
			u32 t0 = timer_cycles();
			float pos[3] = { f[2], f[3], f[4] };
			if (!s_gpu_idle) { bsp_render_wait_gpu(); video_wait_gpu(); }      /* the GS must be done with the old BSP's memory */
			s_gpu_idle = 1;                                                    /* the FINISH of the in-flight frame is consumed: the loop must not wait for it again */
			if (load_bsp((int)f[1]) == 0 && s_game_on) game_teleport(&s_game, pos, f[5] * 3.14159265f / 180.0f);
			printf("[game] switched to BSP %d in %.0f ms; arena %u KB\n", (int)f[1], timer_ms(t0, timer_cycles()), (unsigned)(s_arena.used / 1024));
		}
	}
}

/* Streamed high resolution textures (docs/37, ADR-074 / ADR-082): the staging ring lives above the BSP mark (a BSP switch frees it and this reopens it) and is kept only when
 * the arena still has 1 MB after it: b30 (the largest map) has no room for it, a10 does. The object set sits next to the object pack, the BSP set next to the texture pack. */
#define HIRES_MARGIN (1024u * 1024u)
static void close_hires(void)
{
	if (s_ar_obj_open) aread_close(&s_ar_obj);
	if (s_ar_bsp_open) aread_close(&s_ar_bsp);
	s_ar_obj_open = s_ar_bsp_open = 0;
	s_hires_open = s_bhires_open = 0;
	s_objr.hires = NULL;
	bsp_render_set_hires(NULL, 0, 0.0f);
}

static int derive_path(char *out, size_t cap, const char *src, const char *from, const char *to)
{
	const char *dot = strstr(src, from);
	size_t head, tail = strlen(to);
	if (!dot) return 0;
	head = (size_t)(dot - src);
	if (head + tail + 1u > cap) return 0;
	memcpy(out, src, head); strcpy(out + head, to);
	return 1;
}

static int open_set(hires_t *h, const char *path, const char *what)
{
	long fsz = file_size_of(path);
	if (fsz <= 0) { printf("[hires] %s: none (%s)\n", what, path); return -1; }
	if (hires_open(h, &s_arena, hires_file_read, (void *)path, (size_t)fsz) != 0) { printf("[hires] %s: unusable (%s)\n", what, path); return -1; }
	return 0;
}

static void open_hires(void)
{
	const char *obj = cfg_str(&s_cfg, "obj", "");
	size_t mark = arena_mark(&s_arena);
	int have_obj = 0, have_bsp = 0;
	uint32_t slot_bytes = 0;
	close_hires();
	if (!s_have_objects) return;
	if (cfg_int(&s_cfg, "hires", 1) && derive_path(s_hires_path, sizeof s_hires_path, obj, ".obj.hp2", ".hires.hp2") && open_set(&s_hires, s_hires_path, "objects") == 0) { have_obj = 1; slot_bytes = s_hires.max_bytes; }
	if (cfg_int(&s_cfg, "bsphires", 1) && s_bhires_keyed && s_tex && derive_path(s_bhires_path, sizeof s_bhires_path, s_tex, ".tex.hp2", ".thires.hp2") && open_set(&s_bhires, s_bhires_path, "bsp") == 0) {
		have_bsp = 1;
		if (s_bhires.max_bytes > slot_bytes) slot_bytes = s_bhires.max_bytes;
	}
	if (!have_obj && !have_bsp) { arena_release(&s_arena, mark); return; }
	if (hires_ring_init(&s_hring, &s_arena, (unsigned)cfg_int(&s_cfg, "hiresslots", 3), slot_bytes) != 0 || arena_free(&s_arena) < HIRES_MARGIN) {
		printf("[hires] dropped: %u KB left after the staging ring (needs %u KB)\n", (unsigned)(arena_free(&s_arena) / 1024u), (unsigned)(HIRES_MARGIN / 1024u));
		arena_release(&s_arena, mark); return;
	}
	s_hires_loads = (uint32_t)cfg_int(&s_cfg, "hiresloads", 1);
	s_hring.max_loads = s_hires_loads;
	s_hires_px = 200.0f; cfg_floats(&s_cfg, "hirespx", &s_hires_px, 1);
	s_bhires_ratio = 0.7f; cfg_floats(&s_cfg, "bsphiresratio", &s_bhires_ratio, 1);
	if (have_obj) {
		if (aread_open(&s_ar_obj, s_hires_path) == 0) {
			s_ar_obj_open = 1;
			hires_attach(&s_hires, &s_hring, hires_ar_start, hires_ar_poll, &s_ar_obj);
			s_hires_open = 1;
			s_objr.hires = &s_hires; s_objr.hires_px = s_hires_px;
			printf("[hires] %s: %u variants of %u textures, from %.0f px\n", s_hires_path, (unsigned)s_hires.tex.texture_count, (unsigned)s_hires.map_count, (double)s_hires_px);
		} else printf("[hires] cannot open %s for streaming\n", s_hires_path);
	}
	if (have_bsp) {
		if (aread_open(&s_ar_bsp, s_bhires_path) == 0) {
			s_ar_bsp_open = 1;
			hires_attach(&s_bhires, &s_hring, hires_ar_start, hires_ar_poll, &s_ar_bsp);
			s_bhires_open = 1;
			bsp_render_set_hires(&s_bhires, s_bhires_key, s_bhires_ratio);
			printf("[hires] %s: %u variants of %u textures, from %.2f px per texel\n", s_bhires_path, (unsigned)s_bhires.tex.texture_count, (unsigned)s_bhires.map_count, (double)s_bhires_ratio);
		} else printf("[hires] cannot open %s for streaming\n", s_bhires_path);
	}
	printf("[hires] ring: %u slots of %u KB, %u loads a frame\n", (unsigned)s_hring.slots, (unsigned)(s_hring.slot_bytes / 1024u), (unsigned)s_hring.max_loads);
}

static int load_bsp(int idx)
{
	u32 t0 = timer_cycles(), t1;
	float ms_world, ms_render, ms_game = 0.0f;
	close_hires();
	arena_release(&s_arena, s_bsp_mark);
	if (world_load_bsp(&s_world, &s_arena, s_pack, s_tex, idx)) return -1;
	t1 = timer_cycles(); ms_world = timer_ms(t0, t1); t0 = t1;
	if (bsp_render_set_world(&s_arena, &s_world.bsp, &s_world.source)) return -1;
	t1 = timer_cycles(); ms_render = timer_ms(t0, t1); t0 = t1;
	if (s_have_objects) { s_objr.bsp = &s_world.bsp; s_objr.bsp_index = idx; objects_set_active_bsp(&s_objects, (unsigned)idx); }
	if (cfg_str(&s_cfg, "game", NULL)) {
		gamedata_load_bsp(&s_gd, &s_arena, cfg_str(&s_cfg, "game", NULL), idx);
		if (s_game_on) game_set_collision(&s_game, s_gd.has_coll ? &s_gd.coll : NULL);
	}
	t1 = timer_cycles(); ms_game = timer_ms(t0, t1); t0 = t1;
	open_hires();
	printf("[bsp] load %d: world %.0f ms, render setup %.0f ms, game data %.0f ms, hires %.0f ms\n", idx, (double)ms_world, (double)ms_render, (double)ms_game, (double)timer_ms(t0, timer_cycles()));
	return 0;
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

	arena_bytes = (size_t)cfg_int(&s_cfg, "arena_mb", 25) * 1024u * 1024u;
	heap = malloc(arena_bytes);                         /* the one boot-time allocation (ADR-006) */
	if (!heap) { printf("[err] cannot allocate %u MB arena\n", (unsigned)(arena_bytes >> 20)); return 1; }
	arena_init(&s_arena, "level", heap, arena_bytes);

	pad_init();
	if (cfg_int(&s_cfg, "memcard", 1)) memcard_init((int)cfg_int(&s_cfg, "mcformat", 0));
	if (cfg_int(&s_cfg, "audio", 1)) audio_init();
	{
		const char *at = cfg_str(&s_cfg, "audiotest", NULL);     /* an APCM file: load and play it once (bring-up check) */
		if (at && audio_ready()) {
			FILE *af = fopen(at, "rb");
			if (af) {
				static unsigned char abuf[65536] __attribute__((aligned(64)));
				int n = (int)fread(abuf, 1, sizeof abuf, af);
				audio_sample smp;
				fclose(af);
				printf("[audio] test file %d bytes: load %d, ", n, audio_load(&smp, abuf, n));
				printf("play ch %d\n", audio_play(&smp, 1.0f, 0.0f));
			} else printf("[audio] cannot open %s\n", at);
		}
	}
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
	s_pack = pack; s_tex = tex;
	memset(&s_world, 0, sizeof s_world);
	world_load_tex(&s_world, &s_arena, tex);                 /* map-wide textures stay resident; BSPs are loaded after the arena mark */
	{
		const char *obj = cfg_str(&s_cfg, "obj", NULL);
		if (obj && world_load_objects(&s_world, &s_arena, obj) == 0) {
			object_world_t ow;
			ow.scen = &s_world.scen; ow.models = &s_world.models; ow.anims = s_world.anims.graph_count ? &s_world.anims : NULL; ow.mouth = s_world.mouth.count ? &s_world.mouth : NULL;
			objects_clear(&s_objects);
			objects_spawn_all(&s_objects, &ow, (int)cfg_int(&s_cfg, "objhidden", 0));
			printf("[obj] %u objects placed in all BSPs (skipped: %u no model, %u hidden, %u over capacity)\n",
			       s_objects.count, s_objects.skipped_no_model, s_objects.skipped_hidden, s_objects.skipped_cap);
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
			s_objr.bsp_index = 0;
			s_objr.textures = &s_world.obj_source;
			s_objr.fp = &s_fp;
			s_objr.lod = &s_lod;
			if (!cfg_int(&s_cfg, "objanim", 1)) { unsigned oi; for (oi = 0; oi < s_objects.count; oi++) s_objects.obj[oi].flags &= ~OBJ_F_ANIMATED; }
			s_objr.max_distance = (float)cfg_int(&s_cfg, "objdist", 200);
			s_objr.far_px = (float)cfg_int(&s_cfg, "objfarpx", 20);
			s_objr.ee_clip = (int)cfg_int(&s_cfg, "objclip", 1);
			s_objr.clip_budget_ms = (float)cfg_int(&s_cfg, "objclipms", 3);
			s_objr.max_batches = (unsigned)cfg_int(&s_cfg, "objcap", 900);
			s_objr.draw_objects = (int)cfg_int(&s_cfg, "objects", 1);
			s_objr.draw_fp = (int)cfg_int(&s_cfg, "fp", 1);
			s_objr.cchg = &s_world.cchg;
			{ float sr[3]; if (cfg_floats(&s_cfg, "skyrot", sr, 3) == 3) { s_objr.sky_rot[0] = sr[0] * 0.01745329f; s_objr.sky_rot[1] = sr[1] * 0.01745329f; s_objr.sky_rot[2] = sr[2] * 0.01745329f; } }
			s_objr.sky_model = -1;
			s_objr.sky_hide = (unsigned)cfg_int(&s_cfg, "skyhide", 0);
			s_objr.dbg_obj = (int)cfg_int(&s_cfg, "objdbg", -1);
			{ float tr[3]; s_objr.trace_lo = s_objr.trace_hi = 0; s_objr.trace_step = 1; if (cfg_floats(&s_cfg, "objtrace", tr, 3) == 3 && tr[2] >= 1.0f) { s_objr.trace_lo = (unsigned)tr[0]; s_objr.trace_hi = (unsigned)tr[1]; s_objr.trace_step = (unsigned)tr[2]; } }
			s_objr.unlit_add = (int)cfg_int(&s_cfg, "unlitadd", 1);
			{
				float v[3] = { 1.0f, 1.0f, 1.0f };
				s_objr.dl_gain = 1.0f; cfg_floats(&s_cfg, "dlobj", &s_objr.dl_gain, 1);
				/* cinematic object lighting, measured against the video (ADR-073): the cluster probes alone leave objects 1.2 .. 2.4 x darker than the Xbox shows them; gameplay keeps the probes as they are */
				s_cine_light[0] = 2.5f; s_cine_light[1] = 2.3f; s_cine_light[2] = 2.6f; s_cine_amb = 1.3f; s_objr.light_chroma = 1.0f;
				cfg_floats(&s_cfg, "objamb", &s_cine_amb, 1); cfg_floats(&s_cfg, "objchroma", &s_objr.light_chroma, 1);
				if (cfg_floats(&s_cfg, "objlight", v, 3) == 3) { s_cine_light[0] = v[0]; s_cine_light[1] = v[1]; s_cine_light[2] = v[2]; }
				s_objr.light_gain[0] = s_objr.light_gain[1] = s_objr.light_gain[2] = 1.0f; s_objr.amb_gain = 1.0f;
				{ float g = 0.33f; if (cfg_floats(&s_cfg, "dlwall", &g, 1) == 1) bsp_render_set_dl_gain(g); }
			}
			g_face_compose = (int)cfg_int(&s_cfg, "facecompose", g_face_compose);
			memset(s_objr.cc_perm, 0xFF, sizeof s_objr.cc_perm);
			{                                                      /* cfg ccperm "508:3 223:5": colour change permutations read off the Xbox video */
				const char *cp = cfg_str(&s_cfg, "ccperm", "");
				while (*cp) {
					char *end;
					long oi = strtol(cp, &end, 10), pi;
					if (end == cp || *end != ':') break;
					pi = strtol(end + 1, &end, 10);
					if (oi >= 0 && oi < 1280 && pi >= 0 && pi < 8) s_objr.cc_perm[oi] = (uint8_t)pi;
					cp = end;
					while (*cp == ' ' || *cp == ',') cp++;
				}
			}
			{ float pg; s_objr.sky_planet_gain = cfg_floats(&s_cfg, "skyplanet", &pg, 1) == 1 ? pg : 1.0f; }
			{ float yv; s_objr.sky_yaw0 = cfg_floats(&s_cfg, "skyyaw", &yv, 1) == 1 ? yv * 0.01745329f : 0.0f; }
			if (cfg_int(&s_cfg, "sky3d", 1) && s_world.sky.valid && s_world.sky.model_tag) s_objr.sky_model = model_find(&s_world.models, s_world.sky.model_tag);
			s_objr.sky_model2 = -1;
			if (cfg_int(&s_cfg, "sky3d", 1) && s_world.sky.valid && s_world.sky.model_tag2) s_objr.sky_model2 = model_find(&s_world.models, s_world.sky.model_tag2);
			printf("[obj] sky model: %s, second %s\n", s_objr.sky_model >= 0 ? "drawn" : "none", s_objr.sky_model2 >= 0 ? "drawn" : "none");
			s_have_objects = 1;
		}
	}
	if (s_have_objects && cfg_str(&s_cfg, "game", NULL)) gamedata_load(&s_gd, &s_arena, cfg_str(&s_cfg, "game", NULL), -1);   /* persistent packs; -1: no BSP yet */
	if (s_have_objects && cfg_str(&s_cfg, "game", NULL) && cfg_int(&s_cfg, "stream", 1) && audio_ready()) {
		char sp[160];
		snprintf(sp, sizeof sp, "%s.strm.hp2", cfg_str(&s_cfg, "game", NULL));
		if (file_size_of(sp) >= 0 && stream_ee_open(&s_see, &s_sp, &s_arena, sp, (int)cfg_int(&s_cfg, "streamlog", 0)) == 0) { s_stream_on = 1; printf("[stream] %s open\n", sp); }
	}
	mapp_reserve(&s_arena);                                   /* persistent mission memory (path scratch) before the BSP mark */
	s_bsp_mark = arena_mark(&s_arena);
	{
		int first_bsp = (int)cfg_int(&s_cfg, "bsp", 0);
		if (cfg_int(&s_cfg, "mission", 0) && s_gd.has_dev && s_gd.dev.start_count && !cfg_str(&s_cfg, "bsp", NULL)) {   /* the scenario's first start decides */
			devdef_start st = devdef_get_start(&s_gd.dev, 0);
			unsigned b;
			for (b = 0; b < 32; b++) if (st.bsp_mask & (1u << b)) { first_bsp = (int)b; break; }
		}
		if (load_bsp(first_bsp)) return 1;
	}
	apply_options();   /* every config, with or without an object pack (ADR-059); `skyfog` reads s_world.sky, loaded with the obj pack above */
	if (s_have_objects) {
		bsp_render_set_object_pass(obj_render_pass, &s_objr, &s_world.obj_source);   /* after the first set_world: key layout needs it */
		s_objr.hires_key_base = bsp_render_reserve_keys(s_world.obj_tex.has ? s_world.obj_tex.tex.texture_count : 0);       /* the key range is reserved whether or not the staging ring fits */
		s_bhires_key = bsp_render_reserve_keys((unsigned)s_world.source.texture_count); s_bhires_keyed = 1;     /* the BSP set's range (docs/37) */
		bsp_render_set_sky_pass(obj_render_sky, &s_objr);
		if (load_bsp(s_world.bsp_index)) return 1;           /* re-run so the texture cache is sized with the object key range */
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
	if (s_have_objects && cfg_str(&s_cfg, "game", NULL)) {
		/* Phase 3: gameplay packs (docs/14-phase3-plan.md): `game <prefix>` loads <prefix>.{coll,cmod,unit,weap,ansm,hud,snd,fx}.hp2 */
		int n = (s_gd.has_unit + s_gd.has_weap + s_gd.has_cmod + s_gd.has_ansm + s_gd.has_hud + s_gd.has_snd + s_gd.has_fx + s_gd.has_coll);
		if (n > 0) {
			game_init_t gi;
			float pp[5];
			memset(&gi, 0, sizeof gi);
			gi.unit = s_gd.has_unit ? &s_gd.unit : NULL; gi.weap = s_gd.has_weap ? &s_gd.weap : NULL; gi.cmod = s_gd.has_cmod ? &s_gd.cmod : NULL;
			gi.ansm = s_gd.has_ansm ? &s_gd.ansm : NULL; gi.fx = s_gd.has_fx ? &s_gd.fx : NULL; gi.coll = s_gd.has_coll ? &s_gd.coll : NULL;
			gi.ow = s_objr.world; gi.objects = &s_objects; gi.fp = &s_fp; gi.seed = (uint32_t)cfg_int(&s_cfg, "seed", 1);
			if (cfg_floats(&s_cfg, "player", pp, 4) < 3 && cfg_int(&s_cfg, "mission", 0) && s_gd.has_dev && s_gd.dev.start_count) {
				devdef_start st = devdef_get_start(&s_gd.dev, 0);
				pp[0] = st.pos[0]; pp[1] = st.pos[1]; pp[2] = st.pos[2]; pp[3] = st.facing * 57.29578f;
			} else if (cfg_floats(&s_cfg, "player", pp, 4) < 3) { pp[0] = s_cam.pos[0]; pp[1] = s_cam.pos[1]; pp[2] = s_cam.pos[2] - 0.62f; pp[3] = s_cam.yaw * 57.29578f; }
			game_init(&s_game, &gi, pp, pp[3] * 3.14159265f / 180.0f);
			s_game.all_weapons = (int)cfg_int(&s_cfg, "allweapons", 0);
			s_game.difficulty = (int)cfg_int(&s_cfg, "difficulty", 1);
			s_game_on = 1;
			checkpoint_save(&s_ckpt, &s_game, &s_fp, NULL);          /* the level-start checkpoint (fx are saved from the first Start press) */
			printf("[game] fp: arms model %d graph %d anim %u, weapon model %d graph %d anim %u hand marker %d node %d\n", s_fp.arms_model, s_fp.arms_graph, s_fp.arms_anim,
			       s_fp.weapon_model, s_fp.weapon_graph, s_fp.weapon_anim, s_fp.hand_marker, s_fp.hand_node);
			{
				int sl, k;
				for (sl = 0; sl < GAME_SLOTS; sl++) {
					const game_slot_t *gs = &s_game.slot[sl];
					anim_graph ag;
					if (gs->weapon < 0 || gs->fp_graph < 0) continue;
					ag = anim_get_graph(s_objr.world.anims, (uint32_t)gs->fp_graph);
					printf("[game] slot %d %s: fp model %d graph %d idle %d (%s), nodes %u\n", sl, gs->def.name, gs->fp_model, gs->fp_graph, gs->fp_anim_idle,
					       gs->fp_anim_idle >= 0 ? anim_get(s_objr.world.anims, &ag, (uint32_t)gs->fp_anim_idle).name : "-", (unsigned)ag.node_count);
					for (k = 0; k < (int)ag.anim_count && k < 16; k++) printf("[game]     anim %d: %s\n", k, anim_get(s_objr.world.anims, &ag, (uint32_t)k).name);
				}
			}
			printf("[game] player at %.2f %.2f %.2f; health %.0f shield %.0f; weapon slots %d %d; %u objects\n", pp[0], pp[1], pp[2], s_game.vitals.health,
			       s_game.vitals.shield, s_game.slot[0].weapon, s_game.slot[1].weapon, s_objects.count);
		}
		if (s_game_on && s_gd.has_snd && audio_ready()) gameaudio_init(&s_ga, &s_gd.snd, s_gd.sndd, s_gd.sndd_size, s_gd.has_fx ? &s_gd.fx : NULL, (int)cfg_int(&s_cfg, "audiolog", 0));
		if (s_ga.snd && cfg_int(&s_cfg, "audiopreload", 1)) { u32 pt = timer_cycles(); unsigned pn = gameaudio_preload(&s_ga); printf("[audio] preloaded %u samples in %.0f ms (%u failed)\n", pn, timer_ms(pt, timer_cycles()), s_ga.failed); }
		if (s_game_on) {
			s_hudr.game = &s_game;
			if (s_gd.has_hud && s_gd.hud_tex.has) {
				s_hudr.hud = &s_gd.hud; s_hudr.tex = &s_gd.hud_source;
				s_hudr.fonts = s_gd.has_fonts ? &s_gd.fonts : NULL; s_hudr.state = &s_hs; hud_state_init(&s_hs);
				if (s_game.has_biped) huddef_find_link(&s_gd.hud, HUDDEF_L_UNIT, s_game.biped.tag_id, 0, &s_unit_hud);
				s_hudr.key_base = bsp_render_reserve_keys(s_gd.hud_tex.tex.texture_count);
				s_hudr.enabled = (int)cfg_int(&s_cfg, "hud", 1);
			}
			if (s_gd.has_fx && s_gd.fx_tex.has) {
				fx_init(&s_fx, &s_gd.fx, s_game.globals.gravity > 0.0f ? s_game.globals.gravity * 900.0f : 3.2f, (uint32_t)cfg_int(&s_cfg, "seed", 1) + 99u);
				fx_set_attachments(&s_fx, s_gd.fxat, s_gd.fxat_size);
				s_objr.fx = &s_fx;
				s_fx.dbg = (int)cfg_int(&s_cfg, "fxdbg", 0);
				s_fxr.fx = &s_fx; s_fxr.tex = &s_gd.fx_source;
				s_fxr.key_base = bsp_render_reserve_keys(s_gd.fx_tex.tex.texture_count);
				s_cine_fxadd = 0.55f; cfg_floats(&s_cfg, "fxadd", &s_cine_fxadd, 1); s_cine_halo = 0.5f; cfg_floats(&s_cfg, "fxhalo", &s_cine_halo, 1);          /* additive particles in a cinematic: the engine bells saturate to white at 1.0 where the video keeps a blue tint (ADR-074) */
				s_fxr.add_gain = 1.0f; s_fxr.add_size = 1.0f; cfg_floats(&s_cfg, "fxaddsize", &s_fxr.add_size, 1);
				s_fxr.enabled = (int)cfg_int(&s_cfg, "fx", 1); s_fxr.decals = (int)cfg_int(&s_cfg, "fxdecals", 1); s_fxr.particles = (int)cfg_int(&s_cfg, "fxparticles", 1);
			}
			bsp_render_set_overlay_pass(overlay_pass, NULL);
		}
		if (s_game_on && cfg_int(&s_cfg, "mission", 0)) {
			mapp_env me;
			memset(&me, 0, sizeof me);
			me.gd = &s_gd; me.game = &s_game; me.objects = &s_objects; me.ow = s_objr.world; me.fx = s_fx.def ? &s_fx : NULL; me.hud = &s_hs;
			me.ga = &s_ga; me.sp = s_stream_on ? &s_sp : NULL; me.cfg = &s_cfg; me.arena = &s_arena; me.bsp = s_world.bsp_index;
			if (mapp_init(&me) == 0) { s_hudr.cine = mapp_mission()->cine; mapp_set_bsp(s_world.bsp_index); mapp_checkpoint_save(); }
		}
		input_script_parse(&s_script, &s_cfg);
		if (s_script.count) printf("[game] scripted input: %d keys\n", s_script.count);
	}
	s_vblanks = cfg_int(&s_cfg, "fps", 30) >= 60 ? 1 : 2;
	printf("[cfg] target %d fps (%d vblank(s) per frame)\n", 60 / s_vblanks, s_vblanks);
	profiler_init(&s_prof, ((float)s_vblanks + 0.5f) * VBLANK_MS);   /* late by more than half a frame time = dropped */
	lodctl_init(&s_lod, 33.3f / (float)(3 - s_vblanks), 60 / s_vblanks);
	s_prof.overlay = (int)cfg_int(&s_cfg, "overlay", 1);
	prime_finish();

	limit = (u32)cfg_int(&s_cfg, "frames", 0);
	for (;;) {
		u32 t0 = timer_cycles(), t1, t2, tsim, tobj, tp, tw1, tw2;
		fly_input in;
		unsigned down = pad_poll(&in);

		if (down & PAD_BTN_START) s_prof.overlay ^= 1;
		if (s_game_on) {
			if (s_vblanks == 2 || (frame & 1) == 0) {            /* the 30 Hz simulation tick */
				raw_input_t raw;
				game_input_t gin;
				if (s_script.count) input_script_eval(&s_script, (float)s_tick * (1.0f / 30.0f), &raw);
				else if (!pad_read_raw(&raw)) memset(&raw, 0, sizeof raw);
				input_map(&gin, &raw, s_game.prev_held);
				if (gin.pressed & IN_PAUSE) { s_paused ^= 1; s_hs.paused = s_paused; }
				if (gin.pressed & IN_SAVE) { do_checkpoint_save(); if (cfg_int(&s_cfg, "savetest", 0)) card_save(0, 1); }
				if ((gin.pressed & IN_REVERT) || s_game.dead_ticks > 90) {
					if (s_game.dead_ticks > 90) printf("[game] dead for 3 s at %.1f %.1f %.1f (bsp %d, hp %.0f/%.0f): reverting\n", (double)s_game.player.pos[0], (double)s_game.player.pos[1], (double)s_game.player.pos[2], s_world.bsp_index, (double)s_game.vitals.health, (double)s_game.vitals.shield);
					if (do_revert()) { gin.held = gin.pressed = 0; } else s_game.dead_ticks = 0;
				}
				if (!vitals_dead(&s_game.vitals) && !s_game.player.on_ground && s_game.player.pos[2] < s_world.bsp.bounds[2] - FALL_KILL_MARGIN) {
					/* out of the world (ADR-062): the loaded BSP has no floor down here, and with no cluster around the camera the renderer draws everything in view */
					printf("[game] fell out of the world at %.1f %.1f %.1f (bsp %d, floor of the map %.1f): killed\n", (double)s_game.player.pos[0], (double)s_game.player.pos[1], (double)s_game.player.pos[2], s_world.bsp_index, (double)s_world.bsp.bounds[2]);
					vitals_damage(&s_game.vitals, 1.0e6f, 1.0f, 1.0f, 1.0f);
				}
				if (s_paused) { s_game.ev_count = 0; s_game.prev_held = gin.held; game_camera(&s_game, &s_cam); goto sim_done; }
				tp = timer_cycles();
				if (mapp_active()) { mapp_tick(&gin); p2_add(P2_MAPP, tp); tp = timer_cycles(); serve_mission(); p2_add(P2_SERVE, tp); } else { game_tick(&s_game, &gin); p2_add(P2_MAPP, tp); }
				tp = timer_cycles();
				check_bsp_switch();
				s_tick++;
				s_hudr.tick = s_tick;
				p2_add(P2_BSPSW, tp);
				tp = timer_cycles();
				if (s_stream_on) stream_ee_tick(&s_see);
				p2_add(P2_STREAM, tp);
				tp = timer_cycles();
				if (s_fx.def) {
					unsigned ei, ec;
					const game_event_t *ev = game_events(&s_game, &ec);
					for (ei = 0; ei < ec; ei++) {
						const game_event_t *e = &ev[ei];
						if (!e->tag) continue;
						switch (e->type) {
						case GE_IMPACT: fx_spawn(&s_fx, e->tag, e->pos, e->normal, 1); break;
						case GE_DETONATE: fx_spawn(&s_fx, e->tag, e->pos, e->normal, 1); break;
						case GE_FIRE: {                               /* muzzle effects: out in front of the view model, right hand side */
							float d[3] = { e->normal[0], e->normal[1], e->normal[2] }, r[3] = { sinf(s_game.player.yaw), -cosf(s_game.player.yaw), 0.0f }, mp[3];
							mp[0] = e->pos[0] + d[0] * 0.75f + r[0] * 0.12f; mp[1] = e->pos[1] + d[1] * 0.75f + r[1] * 0.12f; mp[2] = e->pos[2] + d[2] * 0.75f - 0.08f;
							fx_spawn(&s_fx, e->tag, mp, d, 0);
						} break;
						case GE_THROW: case GE_RELOAD: case GE_READY: case GE_EMPTY: case GE_OVERHEAT: {
							float eye[3], fwd[3];
							game_eye(&s_game, eye, fwd);
							eye[0] += fwd[0] * 0.75f; eye[1] += fwd[1] * 0.75f; eye[2] += fwd[2] * 0.75f - 0.08f;
							fx_spawn(&s_fx, e->tag, eye, fwd, 0);
						} break;
						default: break;
						}
					}
					{                                              /* dynamic lights of attached fixtures: objects take them in obj_render, the BSP's vertex colours in bsp_render */
						static float rows[16][7];
						const fx_dynlight *dl;
						unsigned dn = fx_dynamic_lights(&s_fx, &dl), di, dq;
						for (di = 0; di < dn && di < 16; di++) { for (dq = 0; dq < 3; dq++) { rows[di][dq] = dl[di].pos[dq]; rows[di][3 + dq] = dl[di].rgb[dq]; } rows[di][6] = dl[di].radius; }
						bsp_render_set_dynamic_lights((const float (*)[7])rows, dn < 8 ? dn : 8);
					}
					s_fx.cam[0] = s_cam.pos[0]; s_fx.cam[1] = s_cam.pos[1]; s_fx.cam[2] = s_cam.pos[2];
					fx_tick(&s_fx);
				}
				p2_add(P2_FX, tp);
				tp = timer_cycles();
				if (s_ga.snd) { camera_t ac; game_camera(&s_game, &ac); gameaudio_events(&s_ga, &s_game, &ac); }
				p2_add(P2_AUDIO, tp);
				tp = timer_cycles();
				{
					unsigned ei, ec;
					const game_event_t *ev = game_events(&s_game, &ec);
					hud_tick(&s_hs);
					for (ei = 0; ei < ec; ei++) {
						const game_event_t *e = &ev[ei];
						if (e->type == GE_PICKUP && s_gd.has_strings && s_gd.has_hud && s_gd.has_weap) {      /* "Picked up a pistol" from the item strings */
							int wi = weap_find_weapon(&s_gd.weap, e->tag), li = huddef_find_list(&s_gd.strings, huddef_get_globals(&s_gd.hud).item_strings);
							weap_weapon wd;
							if (wi >= 0 && li >= 0 && weap_get_weapon(&s_gd.weap, (uint32_t)wi, &wd)) {
								uint16_t u[HUD_MSG_LEN];
								char txt[HUD_MSG_LEN];
								uint32_t l = huddef_get_string(&s_gd.strings, (uint32_t)li, wd.hud_message, u, HUD_MSG_LEN), k;
								if (l != HUDDEF_STR_NONE) { for (k = 0; k < l && k < HUD_MSG_LEN - 1; k++) txt[k] = u[k] < 128 ? (char)u[k] : '?'; txt[k] = 0; hud_message(&s_hs, txt, 90); }
							}
						}
						if (s_ga.snd && s_unit_hud) {
							if (e->type == GE_PLAYER_HURT) gameaudio_hud_sound(&s_ga, &s_gd.hud, s_unit_hud, s_game.vitals.shield > 0.0f ? 2u : 64u);
							else if (e->type == GE_SHIELD_DOWN) gameaudio_hud_sound(&s_ga, &s_gd.hud, s_unit_hud, 8u);
							else if (e->type == GE_SHIELD_UP) gameaudio_hud_sound(&s_ga, &s_gd.hud, s_unit_hud, 1u);
						}
					}
				}
				p2_add(P2_HUD, tp);
				if (cfg_int(&s_cfg, "gamelog", 0)) {
					unsigned ei, ec;
					const game_event_t *ev = game_events(&s_game, &ec);
					for (ei = 0; ei < ec; ei++)
						printf("[ev] t%u type %u tag %08x arg %u obj %d val %.1f\n", s_tick, ev[ei].type, (unsigned)ev[ei].tag, ev[ei].arg, ev[ei].obj, ev[ei].value);
				}
				if ((s_tick % 15) == 0 && mapp_active() && cfg_int(&s_cfg, "cinelog", 0)) {
					const cine_state *cs = mapp_mission()->cine;
					cine_pose cp = cine_cam_pose(&cs->cam);
					printf("[cin] t%u bsp %d cin %d ctl %d mode %d cam %.2f %.2f %.2f yaw %.0f pit %.0f roll %.0f fov %.0f nc %.2f lb %.2f fade %.2f (%.2f %.2f %.2f)\n", s_tick, s_world.bsp_index, cs->screen.cinematic,
					       cs->cam.control, cs->cam.mode, (double)cp.pos[0], (double)cp.pos[1], (double)cp.pos[2], (double)(cp.yaw * 57.29578f), (double)(cp.pitch * 57.29578f),
					       (double)(cp.roll * 57.29578f), (double)(cp.fov * 57.29578f), (double)cs->cam.near_clip, (double)cs->screen.letterbox, (double)cs->screen.fade_alpha,
					       (double)cs->screen.fade_rgb[0], (double)cs->screen.fade_rgb[1], (double)cs->screen.fade_rgb[2]);
					if (s_have_objects && cfg_str(&s_cfg, "cinetrack", NULL)) {                 /* position of the objects whose definition name contains the string */
						unsigned oi;
						for (oi = 0; oi < s_objects.count; oi++) {
							const object_t *ob = &s_objects.obj[oi];
							scen_def d;
							if (ob->model < 0) continue;
							d = scen_get_def(s_objr.world.scen, (uint32_t)ob->def);
							if (strstr(d.name, cfg_str(&s_cfg, "cinetrack", ""))) printf("[cin]   #%u %s at %.2f %.2f %.2f anim %u frame %.1f flags %02x\n", oi, d.name, (double)ob->pos[0], (double)ob->pos[1], (double)ob->pos[2], (unsigned)ob->anim, (double)ob->frame, ob->flags);
						}
					}
				}
				if (s_have_objects && mapp_active() && cfg_str(&s_cfg, "spawnobj", NULL) && s_tick == (uint32_t)cfg_int(&s_cfg, "spawnat", 3000)) {   /* debug: an object whose definition name contains the string, placed 2.5 wu in front of the cinematic camera */
					const cine_state *cs = mapp_mission()->cine;
					cine_pose cp = cine_cam_pose(&cs->cam);
					unsigned di, nd = s_objr.world.scen ? (unsigned)s_objr.world.scen->def_count : 0;
					for (di = 0; di < nd; di++) {
						scen_def d = scen_get_def(s_objr.world.scen, di);
						if (strcmp(d.name, cfg_str(&s_cfg, "spawnobj", "")) == 0) {
							float p[3] = { cp.pos[0] + 1.6f * cosf(cp.yaw) - 0.7f * sinf(cp.yaw), cp.pos[1] + 1.6f * sinf(cp.yaw) + 0.7f * cosf(cp.yaw), cp.pos[2] - 0.8f };
							float fixp[3];
							if (cfg_floats(&s_cfg, "spawnpos", fixp, 3) == 3) { p[0] = fixp[0]; p[1] = fixp[1]; p[2] = fixp[2]; }
							int slot = objects_spawn_def(&s_objects, &s_objr.world, di, p, cp.yaw + 3.1416f);
							printf("[obj] spawned %s as %d\n", d.name, slot);
							break;
						}
					}
				}
				if (s_have_objects && mapp_active() && cfg_int(&s_cfg, "nearlog", 0) && s_tick == (uint32_t)cfg_int(&s_cfg, "nearlog", 0)) {   /* objects around the cinematic camera */
					const cine_state *cs = mapp_mission()->cine;
					cine_pose cp = cine_cam_pose(&cs->cam);
					unsigned oi;
					for (oi = 0; oi < s_objects.count; oi++) {
						const object_t *ob = &s_objects.obj[oi];
						float dx = ob->pos[0] - cp.pos[0], dy = ob->pos[1] - cp.pos[1], dz = ob->pos[2] - cp.pos[2];
						scen_def d;
						if (ob->model < 0 || dx * dx + dy * dy + dz * dz > (float)cfg_int(&s_cfg, "nearrad", 10) * (float)cfg_int(&s_cfg, "nearrad", 10)) continue;
						d = scen_get_def(s_objr.world.scen, (uint32_t)ob->def);
						printf("[near] #%u %s type %u model %d graph %d anim %u \"%s\" frame %.1f flags %02x perm %u/%u/%u/%u at %.2f %.2f %.2f (d %.2f)\n", oi, d.name, ob->type, ob->model, ob->graph, (unsigned)ob->anim,
						       						       object_anim_label(ob),
						       (double)ob->frame, ob->flags, (unsigned)ob->perm[0], (unsigned)ob->perm[1], (unsigned)ob->perm[2], (unsigned)ob->perm[3],
						       (double)ob->pos[0], (double)ob->pos[1], (double)ob->pos[2], (double)sqrtf(dx * dx + dy * dy + dz * dz));
						{                                                   /* where it lands on the screen (0..1 from the left, from the top), to name an object in a frame */
							float cyw = cosf(cp.yaw), syw = sinf(cp.yaw), cpt = cosf(cp.pitch), spt = sinf(cp.pitch);
							float fwd = dx * cyw * cpt + dy * syw * cpt + dz * spt, lft = -dx * syw + dy * cyw, up = -dx * cyw * spt - dy * syw * spt + dz * cpt;
							if (fwd > 0.05f) {
								float th = tanf(cp.fov * 0.5f);
								printf("[near]    screen x %.2f y %.2f (depth %.2f)\n", (double)(0.5f - 0.5f * lft / fwd / th), (double)(0.5f - 0.5f * up / fwd / th), (double)fwd);
							}
						}
						if (cfg_int(&s_cfg, "nearaxes", 0))
							printf("[near]    root fwd %.2f %.2f %.2f  left %.2f %.2f %.2f  up %.2f %.2f %.2f\n", (double)ob->root.m[0][0], (double)ob->root.m[0][1], (double)ob->root.m[0][2],
							       (double)ob->root.m[1][0], (double)ob->root.m[1][1], (double)ob->root.m[1][2], (double)ob->root.m[2][0], (double)ob->root.m[2][1], (double)ob->root.m[2][2]);
						if (cfg_int(&s_cfg, "nearnodes", 0) && ob->type == 1) {
							static object_nodes_t nn;
							unsigned ni, nc = objects_pose(ob, &s_objr.world, &nn);
							for (ni = 0; ni < nc && ni < 40; ni++) printf("[near]    node %u world %.2f %.2f %.2f\n", ni, (double)nn.world[ni].m[3][0], (double)nn.world[ni].m[3][1], (double)nn.world[ni].m[3][2]);
						}
					}
				}
				if ((s_tick % 30) == 0 && cfg_int(&s_cfg, "gamestat", 1))
					printf("[game] t=%us pos %.1f %.1f %.1f hp %.0f sh %.0f shots %u proj %u hits %u kills %u\n", s_tick / 30, s_game.player.pos[0], s_game.player.pos[1],
					       s_game.player.pos[2], s_game.vitals.health, s_game.vitals.shield, (unsigned)s_game.st_shots, (unsigned)s_game.st_projectiles,
					       (unsigned)s_game.st_hits, (unsigned)s_game.st_kills);
			}
			{                                                  /* debug (cfg forceanim "object anim frame"): pose one object with this animation of its own graph every frame, to compare a candidate with the Xbox video */
				float fa[3];
				if (s_have_objects && cfg_floats(&s_cfg, "forceanim", fa, 3) == 3 && fa[0] >= 0.0f && (unsigned)fa[0] < s_objects.count) {
					object_t *fo = &s_objects.obj[(unsigned)fa[0]];
					fo->anim = (uint16_t)fa[1]; fo->frame = fa[2]; fo->flags |= OBJ_F_ANIMATED;
				}
			}
			if (s_have_objects && (s_tick % 30) == 0 && cfg_int(&s_cfg, "trackperm", -1) >= 0) {   /* debug (cfg trackperm N): every 30 ticks, the live objects whose first region's permutation is N (Sergeant Johnson: 1 on the marine model) */
				unsigned ti;
				for (ti = 0; ti < s_objects.count; ti++) {
					const object_t *to = &s_objects.obj[ti];
					if (to->model < 0 || (to->flags & (OBJ_F_FREE | OBJ_F_INACTIVE)) || (int)to->perm[0] != (int)cfg_int(&s_cfg, "trackperm", -1) || to->type != 1) continue;
					printf("[track] t%u #%u at %.2f %.2f %.2f yaw %.2f anim %u frame %.1f flags %02x\n", (unsigned)s_tick, ti, (double)to->pos[0], (double)to->pos[1], (double)to->pos[2], (double)atan2f(to->root.m[0][1], to->root.m[0][0]), (unsigned)to->anim, (double)to->frame, to->flags);
				}
			}
			{                                                  /* debug (cfg forcemodel "model anim frame"): pose every object of this model with that animation of its graph every frame (compare candidates for a whole crowd) */
				float fm[3];
				if (s_have_objects && cfg_floats(&s_cfg, "forcemodel", fm, 3) == 3 && fm[0] >= 0.0f) {
					unsigned fi;
					for (fi = 0; fi < s_objects.count; fi++) {
						object_t *fo = &s_objects.obj[fi];
						if (fo->model != (int)fm[0] || (fo->flags & (OBJ_F_FREE | OBJ_F_INACTIVE)) || fo->graph < 0) continue;
						fo->anim = (uint16_t)fm[1]; fo->frame = fm[2]; fo->flags |= OBJ_F_ANIMATED;
					}
				}
			}
			if (s_have_objects && cfg_int(&s_cfg, "hideobj", -1) >= 0 && (unsigned)cfg_int(&s_cfg, "hideobj", -1) < s_objects.count)       /* debug: never draw this object */
				s_objects.obj[cfg_int(&s_cfg, "hideobj", -1)].flags |= OBJ_F_HIDDEN;
			sim_done:
			game_camera(&s_game, &s_cam);
			s_cam.near_z = CAMERA_NEAR_DEFAULT;
			s_objr.hide_fp = 0;
			if (mapp_active()) {                                  /* a script owns the camera (camera_control / camera_set): render its pose */
				const cine_state *cs = mapp_mission()->cine;
				if (cs && (cs->screen.cinematic || (cs->cam.control && cs->cam.mode != CINE_CAM_FIRST_PERSON))) s_objr.hide_fp = 1;
				if (cs && cs->cam.control && cs->cam.mode == CINE_CAM_POSE) {
					cine_pose cp = cine_cam_pose(&cs->cam);
					cine_pose_to_camera(&cp, &s_cam);
					s_cam.pitch += (float)cfg_int(&s_cfg, "campitch", 0) * 0.01745329f;                  /* debug */
					{ float cl[5]; if (cfg_floats(&s_cfg, "camlock", cl, 5) == 5) {      /* debug (cfg camlock x y z yaw pitch, degrees): the cinematic camera stays where this says, to look at a scene from outside the shot */
						s_cam.pos[0] = cl[0]; s_cam.pos[1] = cl[1]; s_cam.pos[2] = cl[2]; s_cam.yaw = cl[3] * 0.01745329f; s_cam.pitch = cl[4] * 0.01745329f; s_cam.roll = 0.0f; } }
					{ int cperm = (int)cfg_int(&s_cfg, "camperm", 0);                       /* debug: try other angle conventions for cutscene cameras */
					  if (cperm) { float y = s_cam.yaw, pi = s_cam.pitch, ro = s_cam.roll;
						if (cperm & 4) { float t = pi; pi = ro; ro = t; }
						if (cperm & 1) pi = -pi; if (cperm & 2) ro = -ro; if (cperm & 8) y = -y;
						s_cam.yaw = y; s_cam.pitch = pi; s_cam.roll = ro; } }
					/* The intro asks for near 0.01 (and 0.0625 -> 0.01 in the bridge); the VU1 object program drops every object below about 0.04 (a
					 * captain 0.27 away vanishes, found against the Xbox intro), so the script's value is raised to the engine's default. */
					if (cs->cam.near_clip > 0.0f) s_cam.near_z = cs->cam.near_clip > cine_near_floor ? cs->cam.near_clip : cine_near_floor;
				}
			}
		} else if (s_path.count) campath_eval(&s_path, (float)frame * (float)s_vblanks * VBLANK_S, &s_cam);
		else flycam_update(&s_cam, &in, (float)s_vblanks * VBLANK_S);

		tsim = timer_cycles();
		if (s_have_objects) {
			if ((s_vblanks == 2 || (frame & 1) == 0) && !s_paused) {         /* the 30 Hz tick: every frame at 30 fps, every other at 60 */
				objects_tick(&s_objects, &s_objr.world);
				fpview_tick(&s_fp, &s_objr.world);
			}
			fpview_place(&s_fp, &s_objr.world, &s_cam);
		}
		tobj = timer_cycles();
		if ((frame % 6) == 0) profiler_overlay(&s_prof, &s_text);   /* a few updates a second: float formatting is slow */
		{                                                  /* the player is in the air with no floor under them: out of the world, draw no world around that camera (ADR-062) */
			int in_void = 0;
			if (s_game_on && !s_game.player.on_ground && s_game.has_phys) {
				phys_hit ph;
				float dn[3] = { s_game.player.pos[0], s_game.player.pos[1], s_game.player.pos[2] - 80.0f };
				in_void = !s_game.phys.ray(s_game.phys.user, s_game.player.pos, dn, &ph);
			}
			bsp_render_set_void_view(in_void);
		}
		{
			camera_t rcam = s_cam;
			if (mapp_active()) {                                       /* cinematic object lighting vs gameplay */
				int cin = mapp_mission()->cine && mapp_mission()->cine->screen.cinematic;
				unsigned lk;
				for (lk = 0; lk < 3; lk++) s_objr.light_gain[lk] = cin ? s_cine_light[lk] : 1.0f;
				s_objr.amb_gain = cin ? s_cine_amb : 1.0f;
				s_fxr.add_gain = cin ? s_cine_fxadd : 1.0f;
				s_fxr.halo = cin ? s_cine_halo : 0.0f;
			}
			{                                                          /* cfg widescreen: 0 the Xbox's projection, 1 16:9 with the 4:3 vertical field of view kept, 2 the horizontal field of view kept across 16:9, 3 the old 4:3 buffer with the point's fov as the horizontal one */
				int ws = (int)cfg_int(&s_cfg, "widescreen", 0);
				if (ws == 0) {                                         /* XBE 0xbc260: the view's vertical tangent is 0.6375 tan(fov / 2) and 0x5c5e0 scales x by the window's 4:3 (544 x 408 of 640 x 480): the horizontal tangent is 0.85 tan(fov / 2) (62 degrees for a 70 degree point) */
					rcam.hfov = 2.0f * atanf(0.85f * tanf(s_cam.hfov * 0.5f));
					rcam.aspect = 4.0f / 3.0f;
				} else if (ws == 2) {
					rcam.aspect = 16.0f / 9.0f;
				} else if (ws == 1) {
					rcam.hfov = 2.0f * atanf(tanf(s_cam.hfov * 0.5f) * ((16.0f / 9.0f) / s_cam.aspect));
					rcam.aspect = 16.0f / 9.0f;
				}
			}
			if (s_have_objects && cfg_int(&s_cfg, "objlabel", 0)) {   /* debug: the index of every unit (type 1, 5, 2) at its projected position, to read colour-change picks off a frame */
				float m[4][4];
				unsigned oi;
				text_batch_clear(&s_text);
				camera_clip_matrix(&rcam, m);
				for (oi = 0; oi < s_objects.count; oi++) {
					const object_t *ob = &s_objects.obj[oi];
					float c[4], x = ob->pos[0], y = ob->pos[1], z = ob->pos[2] + 0.8f;
					char buf[12];
					int k, sx, sy;
					if (ob->model < 0 || ob->type != 1 || (ob->flags & (OBJ_F_FREE | OBJ_F_HIDDEN | OBJ_F_INACTIVE))) continue;
					for (k = 0; k < 4; k++) c[k] = m[0][k] * x + m[1][k] * y + m[2][k] * z + m[3][k];
					if (c[3] < 0.1f) continue;
					sx = 256 + (int)(256.0f * c[0] / c[3]); sy = 224 - (int)(224.0f * c[1] / c[3]);
					if (sx < 0 || sx > 500 || sy < 0 || sy > 440) continue;
					snprintf(buf, sizeof buf, "%u", oi);
					text_batch_print(&s_text, sx, sy, 0xFFFF00, buf);
				}
			}
			if (s_hires_open || s_bhires_open) hires_ring_begin_frame(&s_hring);
			bsp_render_build(frame, &rcam, &s_text);           /* overlaps the GS drawing frame-1 */
		}
		t1 = timer_cycles();
		{                                                  /* `prof2 1`: where the EE prep time goes (sim tick / object animation / render build), averaged per 60 frames */
			static float acc[3]; static unsigned n; static int on = -1;
			if (on < 0) on = (int)cfg_int(&s_cfg, "prof2", 0);
			if (on) {
				acc[0] += timer_ms(t0, tsim); acc[1] += timer_ms(tsim, tobj); acc[2] += timer_ms(tobj, t1);
				if (++n == 60) { {
					unsigned z;
					static const char *const zn[P2_N] = { "mapp", "serve", "bspsw", "stream", "fx", "audio", "hud" };
					printf("[prof2] frame %u sim %.2f objects %.2f build %.2f ms; sim parts avg/peak:", (unsigned)frame, acc[0] / 60.0f, acc[1] / 60.0f, acc[2] / 60.0f);
					for (z = 0; z < P2_N; z++) { printf(" %s %.2f/%.1f", zn[z], s_p2_ms[z] / 60.0f, s_p2_peak[z]); s_p2_ms[z] = s_p2_peak[z] = 0.0f; }
					printf("\n");
				}
				acc[0] = acc[1] = acc[2] = 0.0f; n = 0; }
			}
		}
		if (!s_gpu_idle) { bsp_render_wait_gpu(); video_wait_gpu(); }      /* FINISH seen: burning vsyncs below clears the flag */
		s_gpu_idle = 0;
		tw1 = timer_cycles();
		{
			/* Present exactly s_vblanks vblanks after the previous flip (30 fps: 2). flip() itself waits for the next
			 * vsync; if the build finished before the vsync that is already (s_vblanks - 1) vblanks after the last
			 * flip, burn the vsyncs that have not happened yet. A build that ran long just lands on a later vsync. */
			int passed = (int)(timer_ms(s_prof.last_flip, timer_cycles()) / VBLANK_MS);
			while (passed < s_vblanks - 1) {
				int now_passed;
				if (s_stream_on) stream_ee_pump(&s_see);
				/* the pump takes time: when it ran across the vsync we were about to wait for, that vsync is gone and waiting for "the next" one would
				 * burn a whole extra frame (ADR-063: this alone made every frame whose EE work ended within a pump of the vblank 3 vblanks long) */
				now_passed = (int)(timer_ms(s_prof.last_flip, timer_cycles()) / VBLANK_MS);
				if (now_passed > passed) { passed = now_passed; continue; }
				graph_wait_vsync();
				passed++;
			}
		}
		tw2 = timer_cycles();
		video_present(&s_video);                           /* vsync + display */
		t2 = timer_cycles();

		profiler_frame(&s_prof, timer_ms(t0, t1), timer_ms(t1, t2), bsp_render_stats(), t2);
		if (frame > 0 && s_prof.last_interval > s_prof.drop_ms) printf("[prof] DROP frame %u interval %.1f ms (ee %.1f ms; gpu wait %.1f, vsync burn %.1f, present %.1f)\n", (unsigned)frame, s_prof.last_interval, timer_ms(t0, t1), timer_ms(t1, tw1), timer_ms(tw1, tw2), timer_ms(tw2, t2));
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
		if (s_mission_done && cfg_int(&s_cfg, "exitonend", 1)) limit = frame;
		if (limit && frame >= limit) {
			if (mapp_active()) { if (cfg_int(&s_cfg, "threaddump", 0)) mapp_dump_threads(); mapp_report(); }
			profiler_report(&s_prof);
			{
				const render_stats_t *rs = bsp_render_stats();
				printf("[mem] level arena %u KB of %u KB (high water %u KB); GS texture window: %u pages free of %u cache pages\n",
				       (unsigned)(s_arena.used / 1024), (unsigned)(s_arena.cap / 1024), (unsigned)(s_arena.high / 1024),
				       (unsigned)rs->pages_free, (unsigned)bsp_render_cache_pages());
			}
			if (s_hires_open || s_bhires_open) printf("[hires] %u loads, %u KB read, %u refused (budget / busy / no slot), %u failed, %u unclaimed\n", (unsigned)s_hring.loads, (unsigned)(s_hring.bytes_read / 1024u), (unsigned)s_hring.refused, (unsigned)s_hring.failed, (unsigned)s_hring.stale);
			if (s_stream_on) {
				const stream_stats *ss = stream_get_stats(&s_sp);
				printf("[stream] %u plays, %u steals, %u finished, %u loop passes; %u reads %u KB, %u read errors; underruns voice %u (%u frames) output %u; ee ms avg %.2f max %.2f per service call\n",
				       (unsigned)ss->plays, (unsigned)ss->steals, (unsigned)ss->finished, (unsigned)ss->loop_passes, (unsigned)ss->reads, (unsigned)(ss->read_bytes / 1024u),
				       (unsigned)ss->read_errors, (unsigned)ss->voice_underruns, (unsigned)ss->voice_underrun_frames, (unsigned)ss->out_underruns,
				       (double)(ss->service_calls ? ss->ms_sum / (float)ss->service_calls : 0.0f), (double)ss->ms_max);
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
				printf("[cam] pos %.2f %.2f %.2f yaw %.1f pitch %.1f roll %.1f fov %.1f near %.3f\n", s_cam.pos[0], s_cam.pos[1], s_cam.pos[2],
				       s_cam.yaw * 57.29578f, s_cam.pitch * 57.29578f, s_cam.roll * 57.29578f, s_cam.hfov * 57.29578f, s_cam.near_z);
		}
	}
	return 0;
}
