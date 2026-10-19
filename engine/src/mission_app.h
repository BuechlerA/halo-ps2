/* Mission wiring for the EE build: owns the mission's static state (units, script machine, devices, cinematics, AI, bot) and connects the pure
 * modules to the platform (audio, stream, HUD, collision/path of the current BSP). main.c calls mapp_init once, mapp_set_bsp after every BSP load,
 * mapp_tick every 30 Hz tick, and serves the requests in `mission_t` (BSP switch, checkpoint save / revert, memory-card save). */
#ifndef MISSION_APP_H
#define MISSION_APP_H
#include "audio/gameaudio.h"
#include "audio/stream.h"
#include "core/config.h"
#include "sim/msave.h"
#include "world/gamedata.h"

typedef struct {
	gamedata_t     *gd;
	game_t         *game;
	object_table_t *objects;
	object_world_t  ow;
	fx_t           *fx;
	hud_state_t    *hud;
	gameaudio_t    *ga;                   /* may have no sounds (ga->snd NULL) */
	stream_player  *sp;                   /* NULL: no streamed audio */
	const cfg_t    *cfg;
	arena_t        *arena;                /* persistent arena (path scratch) */
	int             bsp;
} mapp_env;

/* Allocates what must outlive BSP switches (call before the BSP arena mark). */
void mapp_reserve(arena_t *arena);
/* Builds the mission; returns 0 on success (the data it needs is missing: scripts, devices, AI data). */
int  mapp_init(const mapp_env *env);
int  mapp_active(void);
mission_t *mapp_mission(void);
/* The BSP (and its collision / pathfinding) changed. */
void mapp_set_bsp(int bsp);
/* One 30 Hz tick; `in` is the player's input (the bot replaces it when enabled). */
void mapp_tick(const game_input_t *in);
/* Checkpoints: the snapshot of the whole mission. */
void mapp_checkpoint_save(void);
/* Returns the BSP the snapshot was taken in (the caller loads it if it is not the current one) or -1 without a snapshot; then call mapp_checkpoint_restore. */
int  mapp_checkpoint_bsp(void);
int  mapp_checkpoint_restore(void);
mission_checkpoint_t *mapp_checkpoint(void);
/* Positions the player at the first starting location of the scenario (devdef start 0): pos, yaw, bsp. Returns 1 if there is one. */
int  mapp_start_location(float pos[3], float *yaw, int *bsp);
/* Statistics line for the log. */
void mapp_log_status(void);
const void *mapp_bot(void);
void mapp_dump_threads(void);
void mapp_report(void);                 /* end-of-run summary: unregistered built-ins, AI statistics */

#endif
