/* Free-fly (noclip) camera controller and scripted camera paths. Pure C11, host-tested.
 * Scripted paths make fly-through benchmarks and screenshots deterministic: halo.cfg lines
 *     pathkey <seconds> <x> <y> <z> <yaw deg> <pitch deg>
 * are interpolated linearly (yaw along the shortest arc) and looped. */
#ifndef RENDER_FLYCAM_H
#define RENDER_FLYCAM_H
#include "core/config.h"
#include "render/camera.h"

#define CAMPATH_MAX_KEYS 24

typedef struct { float t, pos[3], yaw, pitch; } cam_key;
typedef struct { cam_key key[CAMPATH_MAX_KEYS]; int count; } cam_path;

typedef struct {
	float move_x, move_y;      /* left stick: strafe (+right) / forward (+up), -1..1 */
	float look_x, look_y;      /* right stick: yaw (+right) / pitch (+up), -1..1 */
	float rise;                /* L1/R1: -1..1 along world z */
	int   fast;
} fly_input;

#define FLY_SPEED 8.0f         /* world units per second (a Halo biped walks ~2.5), x4 when fast */
#define FLY_TURN  2.2f         /* radians per second at full deflection */
#define FLY_DEADZONE 0.15f

/* Returns the number of keys parsed (0 = no path). Keys must be time-ordered; unordered ones are dropped. */
int  campath_parse(cam_path *p, const cfg_t *cfg);
float campath_duration(const cam_path *p);
void campath_eval(const cam_path *p, float t, camera_t *cam);
void flycam_update(camera_t *cam, const fly_input *in, float dt);
float fly_deadzone(float v);   /* radial-free per-axis deadzone with rescale */

#endif
