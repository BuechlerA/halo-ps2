/* Host test: the camera observer (sim/cine.c) against the Xbox executable's own (XBE 0x11dd30, run under an x86 emulator by tools/xbe/observer_oracle.py; ADR-083).
 * Each script cuts to a pose and flies through camera_set moves, retargets in mid flight included; the golden file holds the Xbox's position, forward and up
 * every 5th tick. The poses are Halo euler angles; the comparison is on the vectors, so the euler convention is covered too. */
#include "sim/cine.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cine_observer_golden.h"

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static void make_pose(cine_pose *p, const float v[6])
{
	cine_halo_euler_to_ypr(v[0], v[1], v[2], &p->yaw, &p->pitch, &p->roll);
	p->pos[0] = v[3]; p->pos[1] = v[4]; p->pos[2] = v[5];
	p->fov = 1.222f;
}

int main(void)
{
	size_t si, gi = 0;
	for (si = 0; si < sizeof OBSERVER_SCRIPTS / sizeof OBSERVER_SCRIPTS[0]; si++) {
		cine_camera c;
		const char *p = OBSERVER_SCRIPTS[si];
		int tick = 0, samples = 0;
		float worst_p = 0.0f, worst_o = 0.0f;
		cine_cam_init(&c);
		cine_cam_control(&c, 1);
		while (*p) {
			char w[16];
			float v[6];
			int n, ticks = 0, used;
			cine_pose pose;
			if (sscanf(p, "%15s%n", w, &used) != 1) break;
			p += used;
			if (!strcmp(w, "run")) {
				int t;
				sscanf(p, "%d%n", &n, &used); p += used;
				for (t = 0; t < n; t++) {
					cine_pose q;
					float cy, sy, cp, sp, cr, sr, f[3], r0[3], u0[3], u[3];
					int i;
					cine_cam_tick(&c);
					tick++;
					q = cine_cam_pose(&c);
					cy = cosf(q.yaw); sy = sinf(q.yaw); cp = cosf(q.pitch); sp = sinf(q.pitch); cr = cosf(q.roll); sr = sinf(q.roll);
					f[0] = cp * cy; f[1] = cp * sy; f[2] = sp; r0[0] = sy; r0[1] = -cy; r0[2] = 0.0f; u0[0] = -sp * cy; u0[1] = -sp * sy; u0[2] = cp;
					for (i = 0; i < 3; i++) u[i] = sr * r0[i] + cr * u0[i];
					if (tick % 5 == 0) {
						const float *g = OBSERVER_GOLDEN[gi++];
						float ep = 0.0f, eo = 0.0f;
						CHECK((int)g[0] == tick);
						for (i = 0; i < 3; i++) { ep = fmaxf(ep, fabsf(q.pos[i] - g[1 + i])); eo = fmaxf(eo, fmaxf(fabsf(f[i] - g[4 + i]), fabsf(u[i] - g[7 + i]))); }
						worst_p = fmaxf(worst_p, ep); worst_o = fmaxf(worst_o, eo);
						samples++;
					}
				}
			} else {
				int k;
				for (k = 0; k < 6; k++) { sscanf(p, "%f%n", &v[k], &used); p += used; }
				if (!strcmp(w, "move")) { sscanf(p, "%d%n", &ticks, &used); p += used; }
				make_pose(&pose, v);
				cine_cam_set(&c, &pose, (uint32_t)ticks, 0);
			}
			while (*p == '\n' || *p == ' ') p++;
		}
		CHECK(OBSERVER_GOLDEN[gi][0] == -1.0f);
		gi++;
		printf("  observer script %zu: %d samples against the Xbox, worst position error %.5f, worst vector error %.5f\n", si, samples, (double)worst_p, (double)worst_o);
		CHECK(samples > 10 && worst_p < 0.02f && worst_o < 2e-3f);
	}
	if (failures) { printf("%d failure(s)\n", failures); return 1; }
	printf("ok\n");
	return 0;
}
