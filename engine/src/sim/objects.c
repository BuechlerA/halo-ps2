#include "sim/objects.h"
#include <math.h>
#include <string.h>

void objects_clear(object_table_t *t)
{
	memset(t, 0, sizeof *t);
}

unsigned objects_spawn_bsp(object_table_t *t, const object_world_t *w, unsigned bsp, int include_hidden)
{
	uint32_t i, n = w->scen->placement_count;
	t->count = 0;
	t->skipped_no_model = t->skipped_hidden = t->skipped_cap = t->skipped_other_bsp = 0;
	for (i = 0; i < n; i++) {
		scen_placement p = scen_get_placement(w->scen, i);
		scen_def d;
		object_t *o;
		int model, graph = -1;
		model_info mi;
		unsigned r;
		if (!(p.bsp_mask & (1u << bsp))) { t->skipped_other_bsp++; continue; }
		if (!include_hidden && !(p.flags & SCEN_P_INITIALLY_VISIBLE)) { t->skipped_hidden++; continue; }
		d = scen_get_def(w->scen, p.def);
		model = d.model_tag ? model_find(w->models, d.model_tag) : -1;
		if (model < 0) { t->skipped_no_model++; continue; }
		if (t->count >= OBJ_MAX) { t->skipped_cap++; continue; }
		o = &t->obj[t->count++];
		memset(o, 0, sizeof *o);
		o->def = (int16_t)p.def;
		o->type = d.type;
		o->model = (int16_t)model;
		if (d.anim_tag && w->anims) graph = anim_find_graph(w->anims, d.anim_tag);
		o->graph = (int16_t)graph;
		o->anim = SCEN_NO_ANIM;
		if (graph >= 0 && d.idle_anim != SCEN_NO_ANIM) {
			anim_graph g = anim_get_graph(w->anims, (uint32_t)graph);
			if (d.idle_anim < g.anim_count) { o->anim = d.idle_anim; o->flags |= OBJ_F_ANIMATED; }
		}
		o->flags |= OBJ_F_VISIBLE;
		o->pos[0] = p.pos[0]; o->pos[1] = p.pos[1]; o->pos[2] = p.pos[2];
		o->radius = d.radius;
		mat4_from_ypr(&o->root, p.rot[0], p.rot[1], p.rot[2], p.pos);
		mi = model_get(w->models, (uint32_t)model);
		if (d.radius <= 0.0f) o->radius = mi.radius;
		o->region_count = mi.region_count > OBJ_MAX_REGIONS ? OBJ_MAX_REGIONS : mi.region_count;
		model_select_permutations(w->models, &mi, p.variant, o->perm);
		for (r = 0; r < o->region_count; r++) if (o->perm[r] >= mi.permutation_count) o->perm[r] = 0;
		/* stagger animation phase by position so identical idles do not move in lockstep */
		o->frame = (float)((unsigned)(fabsf(p.pos[0] * 7.0f + p.pos[1] * 13.0f)) % 64u);
	}
	return t->count;
}

void objects_tick(object_table_t *t, const object_world_t *w)
{
	unsigned i;
	for (i = 0; i < t->count; i++) {
		object_t *o = &t->obj[i];
		if (o->flags & OBJ_F_ANIMATED) {
			anim_graph g = anim_get_graph(w->anims, (uint32_t)o->graph);
			anim_info a = anim_get(w->anims, &g, o->anim);
			float frames = (float)a.frame_count;
			o->frame += a.fps > 0.0f ? a.fps / 30.0f : 1.0f;
			if (frames > 0.0f && o->frame >= frames) o->frame = (a.flags & ANIM_LOOPS) ? fmodf(o->frame, frames) : frames - 1.0f;
		}
	}
}

unsigned objects_pose_model(const object_world_t *w, int model, int graph, unsigned anim, float frame, unsigned node_offset,
                            const mat4 *root, object_nodes_t *out)
{
	model_info mi;
	skel_xform local[SKEL_MAX_NODES];
	int16_t parent[SKEL_MAX_NODES];
	unsigned n, i;
	if (model < 0) { out->count = 0; return 0; }
	mi = model_get(w->models, (uint32_t)model);
	n = mi.node_count > SKEL_MAX_NODES ? SKEL_MAX_NODES : mi.node_count;
	for (i = 0; i < n; i++) {
		model_node nd = model_get_node(w->models, &mi, i);
		memcpy(local[i].rot, nd.rotation, sizeof local[i].rot);
		memcpy(local[i].trans, nd.translation, sizeof local[i].trans);
		local[i].scale = nd.scale;
		parent[i] = nd.parent;
	}
	if (graph >= 0 && anim != SCEN_NO_ANIM && w->anims) {
		anim_graph g = anim_get_graph(w->anims, (uint32_t)graph);
		if (anim < g.anim_count) {
			static anim_pose pose;                          /* ~2 KB; the tick/render path is single threaded */
			anim_sample(w->anims, &g, anim, frame, &pose);
			for (i = 0; i < n && node_offset + i < g.node_count; i++) {
				const anim_xform *x = &pose.node[node_offset + i];
				memcpy(local[i].rot, x->rot, sizeof local[i].rot);
				memcpy(local[i].trans, x->trans, sizeof local[i].trans);
				local[i].scale = x->scale;
			}
		}
	}
	if (n == 0) { out->count = 0; return 0; }
	if (skel_world(out->world, local, parent, n, root) != 0) { out->count = 0; return 0; }
	{
		/* skinning matrices: vertices are stored in bind-pose model space (docs/11) */
		skel_xform bind[SKEL_MAX_NODES];
		static mat4 def[SKEL_MAX_NODES];
		mat4 ident, inv;
		for (i = 0; i < n; i++) {
			model_node nd = model_get_node(w->models, &mi, i);
			memcpy(bind[i].rot, nd.rotation, sizeof bind[i].rot);
			memcpy(bind[i].trans, nd.translation, sizeof bind[i].trans);
			bind[i].scale = nd.scale;
		}
		mat4_identity(&ident);
		if (skel_world(def, bind, parent, n, &ident) != 0) { out->count = 0; return 0; }
		for (i = 0; i < n; i++) {
			mat4_rigid_inverse(&inv, &def[i]);
			mat4_mul(&out->skin[i], &inv, &out->world[i]);
		}
	}
	out->count = n;
	return n;
}

unsigned objects_pose(const object_t *o, const object_world_t *w, object_nodes_t *out)
{
	return objects_pose_model(w, o->model, o->graph, o->anim, o->frame, 0, &o->root, out);
}

unsigned objects_pose_static(const object_t *o, const object_world_t *w, object_nodes_t *out)
{
	model_info mi;
	unsigned n, i;
	if (o->model < 0) { out->count = 0; return 0; }
	mi = model_get(w->models, (uint32_t)o->model);
	n = mi.node_count > SKEL_MAX_NODES ? SKEL_MAX_NODES : mi.node_count;
	for (i = 0; i < n; i++) out->skin[i] = o->root;
	out->count = n;
	return n;
}

int objects_spawn_def(object_table_t *t, const object_world_t *w, unsigned def, const float pos[3], float yaw)
{
	scen_def d;
	object_t *o;
	model_info mi;
	int model, graph = -1;
	unsigned r;
	if (def >= w->scen->def_count || t->count >= OBJ_MAX) return -1;
	d = scen_get_def(w->scen, def);
	model = d.model_tag ? model_find(w->models, d.model_tag) : -1;
	if (model < 0) return -1;
	o = &t->obj[t->count];
	memset(o, 0, sizeof *o);
	o->def = (int16_t)def;
	o->type = d.type;
	o->model = (int16_t)model;
	if (d.anim_tag && w->anims) graph = anim_find_graph(w->anims, d.anim_tag);
	o->graph = (int16_t)graph;
	o->anim = SCEN_NO_ANIM;
	if (graph >= 0 && d.idle_anim != SCEN_NO_ANIM) {
		anim_graph g = anim_get_graph(w->anims, (uint32_t)graph);
		if (d.idle_anim < g.anim_count) { o->anim = d.idle_anim; o->flags |= OBJ_F_ANIMATED; }
	}
	o->flags |= OBJ_F_VISIBLE;
	memcpy(o->pos, pos, sizeof o->pos);
	mat4_from_ypr(&o->root, yaw, 0.0f, 0.0f, pos);
	mi = model_get(w->models, (uint32_t)model);
	o->radius = d.radius > 0.0f ? d.radius : mi.radius;
	o->region_count = mi.region_count > OBJ_MAX_REGIONS ? OBJ_MAX_REGIONS : mi.region_count;
	model_select_permutations(w->models, &mi, NULL, o->perm);
	for (r = 0; r < o->region_count; r++) if (o->perm[r] >= mi.permutation_count) o->perm[r] = 0;
	return (int)t->count++;
}
