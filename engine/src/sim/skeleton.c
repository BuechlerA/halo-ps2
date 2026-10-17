#include "sim/skeleton.h"
#include <string.h>

int skel_world(mat4 *world, const skel_xform *local, const int16_t *parent, unsigned count, const mat4 *root)
{
	uint8_t done[SKEL_MAX_NODES];
	unsigned i, left = count, pass;
	if (count > SKEL_MAX_NODES) return -1;
	memset(done, 0, count);
	for (i = 0; i < count; i++)
		if (parent[i] >= (int)count || parent[i] == (int)i) return -1;
	for (pass = 0; left && pass < count + 1; pass++) {
		for (i = 0; i < count; i++) {
			mat4 l;
			if (done[i]) continue;
			if (parent[i] >= 0 && !done[parent[i]]) continue;
			mat4_from_trs(&l, local[i].rot, local[i].trans, local[i].scale);
			mat4_mul(&world[i], &l, parent[i] >= 0 ? &world[parent[i]] : root);
			done[i] = 1;
			left--;
		}
	}
	return left ? -1 : 0;
}

void skel_palette_entry(float out[5][4], const mat4 *node_world, const float clip[4][4], const float light_world[3])
{
	mat4 c, p;
	float l[3];
	int i, k;
	memcpy(c.m, clip, sizeof c.m);
	mat4_mul(&p, node_world, &c);
	for (i = 0; i < 4; i++) for (k = 0; k < 4; k++) out[i][k] = p.m[i][k];
	mat4_world_dir_to_local(node_world, light_world, l);
	out[4][0] = l[0]; out[4][1] = l[1]; out[4][2] = l[2]; out[4][3] = 0.0f;
}
