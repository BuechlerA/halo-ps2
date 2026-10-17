#include "sim/fpview.h"
#include <math.h>
#include <string.h>

void fpview_init(fpview_t *f, const object_world_t *w, const scen_player *p)
{
	model_info mi;
	memset(f, 0, sizeof *f);
	f->arms_model = f->arms_graph = f->weapon_model = f->weapon_graph = -1;
	f->hand_marker = -1;
	f->hand_node = -1;
	f->arms_anim = f->weapon_anim = SCEN_NO_ANIM;
	if (!p->fp_arms_model_tag) return;
	f->arms_model = model_find(w->models, p->fp_arms_model_tag);
	if (f->arms_model < 0) return;
	if (p->fp_arms_anim_tag && w->anims) f->arms_graph = anim_find_graph(w->anims, p->fp_arms_anim_tag);
	f->arms_anim = p->arms_idle_anim;
	if (p->weapon_fp_model_tag) f->weapon_model = model_find(w->models, p->weapon_fp_model_tag);
	if (p->weapon_fp_anim_tag && w->anims) f->weapon_graph = anim_find_graph(w->anims, p->weapon_fp_anim_tag);
	f->weapon_anim = p->weapon_idle_anim;
	mi = model_get(w->models, (uint32_t)f->arms_model);
	if (p->weapon_hand_marker[0]) f->hand_marker = model_find_marker(w->models, &mi, p->weapon_hand_marker);
	/* the doc names a node ('frame r wriste'), not a marker: fall back to that node by name */
	if (f->hand_marker < 0 && p->weapon_hand_marker[0]) {
		unsigned k;
		for (k = 0; k < mi.node_count; k++) {
			model_node nd = model_get_node(w->models, &mi, k);
			if (strcmp(nd.name, p->weapon_hand_marker) == 0) { f->hand_node = (int)k; break; }
		}
	}
	memcpy(f->offset, p->fp_offset, sizeof f->offset);
	/* Halo's first-person arms and gun share one animation graph whose node order interleaves both (the engine maps
	 * nodes by name, objects_pose_model); the gun's root node hangs off the hand node named by the data. */
	f->active = 1;
}

static void advance(float *frame, const anim_section *a, int graph, unsigned anim)
{
	anim_graph g;
	anim_info i;
	if (graph < 0 || anim == SCEN_NO_ANIM || !a) return;
	g = anim_get_graph(a, (uint32_t)graph);
	if (anim >= g.anim_count) return;
	i = anim_get(a, &g, anim);
	*frame += i.fps > 0.0f ? i.fps / 30.0f : 1.0f;
	if (i.frame_count > 0 && *frame >= (float)i.frame_count)
		*frame = (i.flags & ANIM_LOOPS) ? fmodf(*frame, (float)i.frame_count) : (float)i.frame_count - 1.0f;
}

void fpview_tick(fpview_t *f, const object_world_t *w)
{
	if (!f->active) return;
	advance(&f->arms_frame, w->anims, f->arms_graph, f->arms_anim);
	advance(&f->weapon_frame, w->anims, f->weapon_graph, f->weapon_anim);
}

void fpview_place(fpview_t *f, const object_world_t *w, const camera_t *cam)
{
	float right[3], up[3], fwd[3], pos[3];
	int k;
	if (!f->active) return;
	camera_basis(cam, right, up, fwd);
	for (k = 0; k < 3; k++) pos[k] = cam->pos[k] + right[k] * f->offset[0] + up[k] * f->offset[1] + fwd[k] * f->offset[2];
	mat4_from_ypr(&f->arms_root, cam->yaw, cam->pitch, 0.0f, pos);     /* model +x forward, +y left, +z up like the world */
	objects_pose_model(w, f->arms_model, f->arms_graph, f->arms_anim, f->arms_frame, &f->arms_root, &f->arms_nodes);
	f->weapon_root = f->arms_root;
	if (f->weapon_model >= 0 && f->hand_node >= 0 && (unsigned)f->hand_node < f->arms_nodes.count && f->hand_marker < 0) {
		f->weapon_root = f->arms_nodes.world[f->hand_node];       /* separate graphs: attach to the hand node */
	} else if (f->weapon_model >= 0 && f->hand_marker >= 0 && f->arms_nodes.count) {
		model_info mi = model_get(w->models, (uint32_t)f->arms_model);
		model_marker mk = model_get_marker(w->models, &mi, (uint32_t)f->hand_marker);
		if (mk.node >= 0 && (unsigned)mk.node < f->arms_nodes.count) {
			mat4 m;
			mat4_from_trs(&m, mk.rotation, mk.translation, 1.0f);
			mat4_mul(&f->weapon_root, &m, &f->arms_nodes.world[mk.node]);
		}
	}
}
