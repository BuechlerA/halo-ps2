/* Host tests for render/flycam. */
#include "render/flycam.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)
#define NEAR(a, b, e) (fabsf((a) - (b)) <= (e))

int main(void)
{
	static const char text[] =
		"pathkey 0 0 0 0 0 0\n"
		"pathkey 2 10 0 4 90 10\n"
		"pathkey 1.5 99 99 99 0 0\n"          /* out of order: dropped */
		"pathkey 4 10 10 4 170 0\n"
		"pathkey 6 10 10 4 -170 0\n"          /* shortest arc crosses +-180 */
		"pathkey bad\n";
	cfg_t cfg;
	cam_path path;
	camera_t cam;
	fly_input in;
	float before;

	cfg_parse(&cfg, text, sizeof text - 1);
	CHECK(campath_parse(&path, &cfg) == 4);
	CHECK(NEAR(campath_duration(&path), 6.0f, 1e-6f));
	camera_init(&cam);
	campath_eval(&path, 1.0f, &cam);
	CHECK(NEAR(cam.pos[0], 5.0f, 1e-4f) && NEAR(cam.pos[2], 2.0f, 1e-4f));
	CHECK(NEAR(cam.yaw, 45.0f * 3.14159265f / 180.0f, 1e-4f));
	campath_eval(&path, 3.0f, &cam);
	CHECK(NEAR(cam.pos[1], 5.0f, 1e-4f));
	campath_eval(&path, 5.0f, &cam);
	CHECK(fabsf(cam.yaw) > 3.0f);                         /* near +-180, not swung through 0 */
	campath_eval(&path, 6.0f + 1.0f, &cam);               /* loops: t=7 == t=1 */
	CHECK(NEAR(cam.pos[0], 5.0f, 1e-4f));
	campath_eval(&path, -1.0f, &cam);                     /* negative time wraps too */
	CHECK(cam.pos[0] > 0.0f);
	memset(&path, 0, sizeof path);
	campath_eval(&path, 1.0f, &cam);                      /* empty path: no change, no crash */

	/* Stick handling. */
	CHECK(fly_deadzone(0.1f) == 0.0f && fly_deadzone(-0.14f) == 0.0f);
	CHECK(NEAR(fly_deadzone(1.0f), 1.0f, 1e-6f) && NEAR(fly_deadzone(-1.0f), -1.0f, 1e-6f));
	CHECK(fly_deadzone(0.5f) > 0.0f && fly_deadzone(0.5f) < 0.5f);

	camera_init(&cam);
	memset(&in, 0, sizeof in);
	in.move_y = 1.0f;
	flycam_update(&cam, &in, 0.5f);
	CHECK(NEAR(cam.pos[0], FLY_SPEED * 0.5f, 1e-4f));      /* forward is +x at yaw 0 */
	in.fast = 1;
	before = cam.pos[0];
	flycam_update(&cam, &in, 0.5f);
	CHECK(NEAR(cam.pos[0] - before, FLY_SPEED * 4.0f * 0.5f, 1e-3f));
	memset(&in, 0, sizeof in);
	in.look_x = 1.0f;                                      /* stick right turns clockwise: yaw decreases */
	camera_init(&cam);
	flycam_update(&cam, &in, 0.25f);
	CHECK(cam.yaw < 0.0f);
	memset(&in, 0, sizeof in);
	in.look_y = 1.0f;                                      /* stick up looks up */
	camera_init(&cam);
	flycam_update(&cam, &in, 0.25f);
	CHECK(cam.pitch > 0.0f);

	if (failures) { printf("%d failure(s)\n", failures); return 1; }
	printf("ok\n");
	return 0;
}
