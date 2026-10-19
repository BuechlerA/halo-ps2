/* The mission bot (gate harness, docs/23): produces the player's input from the mission's own state so a whole mission can be played headless.
 * It walks to the trigger volume the scripts are currently waiting for (mission.awaited_volume) or to the active navpoint, along the BSP's
 * pathfinding mesh; it turns towards the nearest visible hostile and shoots, reloads, swaps empty weapons, picks up weapons, jumps when stuck.
 * It sets the player's yaw / pitch directly (a harness, not a human) and reports every assist it had to take (teleports over stuck spots).
 * Pure C11 over mission_t; host-tested with the real packs. */
#ifndef SIM_BOT_H
#define SIM_BOT_H
#include "ai/path.h"
#include "sim/mission.h"

#define BOT_WP_MAX 48

typedef struct {
	int       enabled;
	float     goal[3];
	int       has_goal, goal_kind;           /* 0 none, 1 volume, 2 navpoint, 3 pickup, 4 explore */
	uint32_t  goal_id;
	path_waypoint wp[BOT_WP_MAX];
	int       wp_n, wp_i;
	uint32_t  replan_at, stuck_ticks, stuck_total;
	float     last_pos[3];
	uint32_t  last_progress_tick;
	/* statistics */
	uint32_t  assists, replans, shots_ticks, enemies_seen, volumes_reached, ticks_moving, ticks_idle;
	uint32_t  last_volume_tick;
	int       target_unit;                    /* hostile currently engaged (unit index) or -1 */
	uint32_t  explore_phase;
	uint16_t  ignore_vol[4]; uint32_t ignore_until[4];   /* volumes outside the loaded BSP: skipped for a while */
	int16_t   shun_unit[4]; uint32_t shun_until[4];      /* hostile units that did not die (behind glass, invulnerable, unreachable): not engaged for a while */
	int       engage_unit; uint32_t engage_start; float engage_hp;
	uint32_t  progress_tick; float progress_dist;        /* distance to the goal at the last progress check */
	uint32_t  exercise_ticks;                  /* ticks spent demonstrating controls for tutorial waits */
	uint32_t  unreach_ticks;                   /* ticks the goal has been unreachable (partial route) or the goal's height unreachable */
	int       route_status, route_start, route_goal;   /* last path_route result (debug) */
} bot_t;

void bot_init(bot_t *b);
/* One tick: fills `in` (the player's input for this tick). `path` and `scratch` of the current BSP may be NULL (then straight-line walking). */
void bot_tick(bot_t *b, mission_t *m, const path_bsp *path, path_scratch *scratch, game_input_t *in);

#endif
