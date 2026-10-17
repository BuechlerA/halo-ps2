/* Player biped controller: Halo-style capsule movement (walk, run, crouch, jump, stairs, slopes) against phys_world.
 * Fixed 30 Hz step. Pure C11, host-tested with analytic worlds (tests/test_player.c). Numbers come from the biped and
 * globals tags (player_params_t is filled from `unitdef`); the defaults only exist so tests and tag-less runs behave. */
#ifndef SIM_PLAYER_H
#define SIM_PLAYER_H
#include "sim/phys.h"

typedef struct {
	float radius;               /* collision radius (world units) */
	float height, crouch_height;
	float eye_height, crouch_eye_height;
	float walk_speed;           /* units per second */
	float run_speed;
	float strafe_speed, back_speed;
	float crouch_speed;
	float accel;                /* ground acceleration, units/s^2 */
	float air_accel;
	float jump_speed;           /* initial vertical speed, units/s */
	float gravity;              /* units/s^2 (positive) */
	float terminal_speed;
	float step_height;          /* maximum climbable step */
	float max_slope_cos;        /* steepest walkable slope: normal.z must exceed this */
	float turn_rate;            /* radians per second at full stick deflection */
	float pitch_limit;          /* radians */
} player_params_t;

typedef struct {
	float move_x, move_y;       /* -1..1: strafe right / forward */
	float look_x, look_y;       /* -1..1: turn right / look up (already deadzoned) */
	int   jump, crouch, walk;   /* held */
} player_cmd_t;

typedef struct {
	float pos[3];               /* feet centre */
	float vel[3];
	float yaw, pitch;
	float crouch;               /* 0 standing .. 1 crouched (smoothed) */
	int   on_ground;
	float ground_normal[3];
	int   jump_held;            /* edge detection: jumping needs a fresh press */
	float landed_speed;         /* downward speed at the last landing (fall damage, camera dip), reset by the reader */
	float turn_accel;           /* 0..1 look acceleration state */
} player_t;

void player_params_default(player_params_t *p);
void player_init(player_t *pl, const float pos[3], float yaw);
void player_tick(player_t *pl, const player_params_t *par, const phys_world *w, const player_cmd_t *cmd, float dt);
void player_eye(const player_t *pl, const player_params_t *par, float eye[3]);

#endif
