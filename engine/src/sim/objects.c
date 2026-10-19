#include "sim/objects.h"
#include <math.h>
#include <string.h>

void objects_clear(object_table_t *t)
{
	memset(t, 0, sizeof *t);
	memset(t->by_placement, 0xFF, sizeof t->by_placement);          /* -1: not spawned */
}

static int alloc_slot(object_table_t *t)
{
	unsigned i;
	for (i = 0; i < t->count; i++) if (t->obj[i].flags & OBJ_F_FREE) return (int)i;
	if (t->count >= OBJ_MAX) return -1;
	return (int)t->count++;
}

/* Fills slot `slot` for definition `def` at `pos` with Halo-order rotation `rot` (yaw, pitch, roll). Returns 0 or -1 (no model). */
static int make_object(object_table_t *t, const object_world_t *w, int slot, unsigned def, const float pos[3], const float rot[3], const char *variant,
                       int placement, uint32_t bsp_mask)
{
	scen_def d = scen_get_def(w->scen, def);
	object_t *o;
	int model = d.model_tag ? model_find(w->models, d.model_tag) : -1, graph = -1;
	model_info mi;
	unsigned r;
	if (model < 0) return -1;
	o = &t->obj[slot];
	memset(o, 0, sizeof *o);
	if (++t->next_serial == 0) t->next_serial = 1;
	o->serial = t->next_serial;
	o->def = (int16_t)def;
	o->placement = (int16_t)placement;
	o->bsp_mask = bsp_mask;
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
	o->pos[0] = pos[0]; o->pos[1] = pos[1]; o->pos[2] = pos[2];
	mat4_from_euler_halo(&o->root, rot[0], rot[1], rot[2], pos);
	mi = model_get(w->models, (uint32_t)model);
	o->radius = d.radius > 0.0f ? d.radius : mi.radius;
	o->region_count = mi.region_count > OBJ_MAX_REGIONS ? OBJ_MAX_REGIONS : mi.region_count;
	model_select_permutations(w->models, &mi, variant, o->perm);
	for (r = 0; r < o->region_count; r++) if (o->perm[r] >= mi.permutation_count) o->perm[r] = 0;
	/* stagger animation phase by position so identical idles do not move in lockstep */
	o->frame = (float)((unsigned)(fabsf(pos[0] * 7.0f + pos[1] * 13.0f)) % 64u);
	return 0;
}

int objects_spawn_placement(object_table_t *t, const object_world_t *w, unsigned placement)
{
	scen_placement p;
	int slot;
	if (placement >= w->scen->placement_count || placement >= OBJ_MAX_PLACEMENTS) return -1;
	if (t->by_placement[placement] >= 0 && !(t->obj[t->by_placement[placement]].flags & OBJ_F_FREE)) return t->by_placement[placement];
	p = scen_get_placement(w->scen, placement);
	slot = alloc_slot(t);
	if (slot < 0) { t->skipped_cap++; return -1; }
	if (make_object(t, w, slot, p.def, p.pos, p.rot, p.variant, (int)placement, p.bsp_mask) != 0) {
		t->obj[slot].flags = OBJ_F_FREE; t->obj[slot].model = -1;
		if ((unsigned)slot + 1 == t->count) t->count--;
		t->skipped_no_model++;
		return -1;
	}
	t->by_placement[placement] = (int16_t)slot;
	return slot;
}

unsigned objects_spawn_all(object_table_t *t, const object_world_t *w, int include_hidden)
{
	uint32_t i, n = w->scen->placement_count, made = 0;
	for (i = 0; i < n && i < OBJ_MAX_PLACEMENTS; i++) {
		scen_placement p = scen_get_placement(w->scen, i);
		if (!include_hidden && !(p.flags & SCEN_P_INITIALLY_VISIBLE)) { t->skipped_hidden++; continue; }
		if (t->by_placement[i] >= 0) continue;
		if (objects_spawn_placement(t, w, i) >= 0) made++;
	}
	return made;
}

unsigned objects_spawn_bsp(object_table_t *t, const object_world_t *w, unsigned bsp, int include_hidden)
{
	uint32_t i, n = w->scen->placement_count;
	objects_clear(t);
	for (i = 0; i < n && i < OBJ_MAX_PLACEMENTS; i++) {
		scen_placement p = scen_get_placement(w->scen, i);
		if (!(p.bsp_mask & (1u << bsp))) { t->skipped_other_bsp++; continue; }
		if (!include_hidden && !(p.flags & SCEN_P_INITIALLY_VISIBLE)) { t->skipped_hidden++; continue; }
		objects_spawn_placement(t, w, i);
	}
	{ unsigned live = 0, k; for (k = 0; k < t->count; k++) if (!(t->obj[k].flags & OBJ_F_FREE)) live++; return live; }
}

void objects_destroy(object_table_t *t, unsigned index)
{
	object_t *o;
	if (index >= t->count) return;
	o = &t->obj[index];
	if (o->placement >= 0 && o->placement < OBJ_MAX_PLACEMENTS) t->by_placement[o->placement] = -1;
	memset(o, 0, sizeof *o);
	o->model = -1; o->placement = -1; o->flags = OBJ_F_FREE;
	while (t->count && (t->obj[t->count - 1].flags & OBJ_F_FREE)) t->count--;
}

void objects_set_active_bsp(object_table_t *t, unsigned bsp)
{
	unsigned i;
	for (i = 0; i < t->count; i++) {
		object_t *o = &t->obj[i];
		if (o->flags & OBJ_F_FREE) continue;
		if (o->bsp_mask == 0 || (o->bsp_mask & (1u << bsp))) o->flags &= ~OBJ_F_INACTIVE; else o->flags |= OBJ_F_INACTIVE;
	}
}

void objects_tick(object_table_t *t, const object_world_t *w)
{
	unsigned i;
	for (i = 0; i < t->count; i++) {
		object_t *o = &t->obj[i];
		if ((o->flags & (OBJ_F_FREE | OBJ_F_INACTIVE)) == 0 && (o->flags & OBJ_F_ANIMATED)) {
			anim_graph g = anim_get_graph(w->anims, (uint32_t)o->graph);
			anim_info a = anim_get(w->anims, &g, o->anim);
			float frames = (float)a.frame_count;
			o->frame += a.fps > 0.0f ? a.fps / 30.0f : 1.0f;
			if (frames > 0.0f && o->frame >= frames) o->frame = (a.flags & ANIM_LOOPS) ? fmodf(o->frame, frames) : frames - 1.0f;
		}
	}
}

/* model node -> graph node, by name; cached for the last pairs (first person, custom animations, units) */
#define REMAP_CACHE 32
typedef struct { int16_t model, graph; uint8_t used, identity; int8_t map[SKEL_MAX_NODES]; } node_remap;
static node_remap s_remap[REMAP_CACHE];
static unsigned   s_remap_next;

static const node_remap *remap_for(const object_world_t *w, const model_info *mi, int model, int graph, unsigned n)
{
	unsigned k, matched = 0, named = 0;
	node_remap *r;
	anim_graph g;
	for (k = 0; k < REMAP_CACHE; k++) if (s_remap[k].used && s_remap[k].model == model && s_remap[k].graph == graph) return &s_remap[k];
	r = &s_remap[s_remap_next++ % REMAP_CACHE];
	memset(r, 0, sizeof *r);
	r->model = (int16_t)model; r->graph = (int16_t)graph; r->used = 1;
	g = anim_get_graph(w->anims, (uint32_t)graph);
	for (k = 0; k < g.node_count; k++) if (anim_node_name(w->anims, &g, k)[0]) named++;
	for (k = 0; k < n; k++) {
		model_node nd = model_get_node(w->models, mi, k);
		int j = named ? anim_find_node(w->anims, &g, nd.name) : (k < g.node_count ? (int)k : -1);   /* unnamed graph: by index */
		r->map[k] = (int8_t)j;
		if (j == (int)k) matched++;
	}
	r->identity = (matched == n);
	return r;
}

int g_face_compose = 1;

static void quat_mul4(const float a[4], const float b[4], float o[4])     /* Hamilton product a * b, x y z w */
{
	float x = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
	float y = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
	float z = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
	float w = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
	o[0] = x; o[1] = y; o[2] = z; o[3] = w;
}

/* The face overlays are deltas (an eyelid turns by a few degrees, a brow moves a few millimetres): the driven channels of the overlay
 * compose with the posed skeleton's own, every other node is untouched. `weight` scales the overlay (1 = as authored). */
static void apply_overlay(const object_world_t *w, const model_info *mi, int model, int gi, const anim_graph *g, unsigned k, float frame,
                          float weight, skel_xform *local, unsigned n)
{
	static anim_pose ov;
	uint8_t mode[ANIM_MAX_NODES];
	const node_remap *rm;
	unsigned i;
	if (k >= g->anim_count || !anim_overlay_modes(w->anims, g, k, mode)) return;
	anim_sample(w->anims, g, k, frame, &ov);
	rm = remap_for(w, mi, model, gi, n);
	for (i = 0; i < n; i++) {
		int j = rm->identity ? (int)i : rm->map[i];
		float q[4], d[4];
		if (j < 0 || (unsigned)j >= g->node_count || (unsigned)j >= ANIM_MAX_NODES) continue;
		if (mode[j] & ANIM_OV_ROT) {
			memcpy(q, ov.node[j].rot, sizeof q);
			if (weight < 1.0f) {                                   /* identity -> q along the short arc */
				float s = q[3] < 0.0f ? -1.0f : 1.0f;
				d[0] = s * q[0] * weight; d[1] = s * q[1] * weight; d[2] = s * q[2] * weight; d[3] = 1.0f + (s * q[3] - 1.0f) * weight;
				{ float l = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2] + d[3] * d[3]); if (l > 1e-12f) { q[0] = d[0] / l; q[1] = d[1] / l; q[2] = d[2] / l; q[3] = d[3] / l; } }
			}
			if (g_face_compose) quat_mul4(local[i].rot, q, d); else quat_mul4(q, local[i].rot, d);
			memcpy(local[i].rot, d, sizeof d);
		}
		if (mode[j] & ANIM_OV_TRANS) {
			local[i].trans[0] += ov.node[j].trans[0] * weight;
			local[i].trans[1] += ov.node[j].trans[1] * weight;
			local[i].trans[2] += ov.node[j].trans[2] * weight;
		}
	}
}

static void apply_face(const object_world_t *w, const model_info *mi, int model, skel_xform *local, unsigned n, const object_face *f)
{
	anim_graph g;
	if (!f || f->graph <= 0 || !w->anims || (unsigned)(f->graph - 1) >= w->anims->graph_count) return;
	g = anim_get_graph(w->anims, (uint32_t)(f->graph - 1));
	if (f->emo_anim > 0) {
		float b = (float)f->emo_blend * (1.0f / 255.0f);          /* easing between two emotion frames: the previous one fades out as the new one fades in */
		if (f->emo_from && f->emo_from != f->emotion && b < 1.0f) apply_overlay(w, mi, model, f->graph - 1, &g, (unsigned)f->emo_anim - 1u, (float)(f->emo_from - 1), 1.0f - b, local, n);
		if (f->emotion) apply_overlay(w, mi, model, f->graph - 1, &g, (unsigned)f->emo_anim - 1u, (float)(f->emotion - 1), (f->emo_from && f->emo_from != f->emotion) || f->emo_blend < 255 ? b : 1.0f, local, n);
	}
	if (f->talk && f->talk_anim > 0) apply_overlay(w, mi, model, f->graph - 1, &g, (unsigned)f->talk_anim - 1u, 0.0f, (float)f->talk * (1.0f / 255.0f), local, n);
}

void objects_set_face_graph(object_t *o, const object_world_t *w, int graph)
{
	anim_graph g;
	int emo, talk;
	o->face.graph = o->face.emo_anim = o->face.talk_anim = 0; o->face.emo_blend = 255; o->face.emo_from = 0;
	if (graph < 0 || !w->anims || (unsigned)graph >= w->anims->graph_count) return;
	g = anim_get_graph(w->anims, (uint32_t)graph);
	emo = anim_find_name(w->anims, &g, "stand emotions");
	talk = anim_find_name(w->anims, &g, "stand talk");
	if (emo < 0 && talk < 0) return;
	o->face.graph = (int16_t)(graph + 1);
	o->face.emo_anim = (int16_t)(emo + 1);
	o->face.talk_anim = (int16_t)(talk + 1);
}

unsigned objects_pose_model(const object_world_t *w, int model, int graph, unsigned anim, float frame,
                            const mat4 *root, object_nodes_t *out)
{
	return objects_pose_model_face(w, model, graph, anim, frame, root, out, NULL);
}

/* inverse(bind-pose world) of every node of a model, direct mapped by model index (BIND_CACHE entries, 64 nodes each): recomputing the bind chain and one inverse per node for every posed
 * object was most of the cost of a pose. An entry is valid for one opening of the model section (its generation) and one model index; a colliding model replaces it. */
#define BIND_CACHE 24
static struct { uint32_t generation; int model; unsigned n; mat4 inv[SKEL_MAX_NODES]; } s_bind[BIND_CACHE];

static const mat4 *bind_inverse(const object_world_t *w, int model, const model_info *mi, const int16_t *parent, unsigned n)
{
	unsigned i, e = (unsigned)model % BIND_CACHE;
	if (s_bind[e].generation == w->models->generation && s_bind[e].model == model && s_bind[e].n == n) return s_bind[e].inv;
	{
		skel_xform bind[SKEL_MAX_NODES];
		static mat4 def[SKEL_MAX_NODES];
		mat4 ident;
		for (i = 0; i < n; i++) {
			model_node nd = model_get_node(w->models, mi, i);
			memcpy(bind[i].rot, nd.rotation, sizeof bind[i].rot);
			memcpy(bind[i].trans, nd.translation, sizeof bind[i].trans);
			bind[i].scale = nd.scale;
		}
		mat4_identity(&ident);
		if (skel_world(def, bind, parent, n, &ident) != 0) return NULL;
		for (i = 0; i < n; i++) mat4_rigid_inverse(&s_bind[e].inv[i], &def[i]);
		s_bind[e].generation = w->models->generation; s_bind[e].model = model; s_bind[e].n = n;
	}
	return s_bind[e].inv;
}

static unsigned pose_impl(const object_world_t *w, int model, int graph, unsigned anim, float frame,
                          const mat4 *root, object_nodes_t *out, const object_face *face, int skin);

unsigned objects_pose_model_face(const object_world_t *w, int model, int graph, unsigned anim, float frame,
                                 const mat4 *root, object_nodes_t *out, const object_face *face)
{
	return pose_impl(w, model, graph, anim, frame, root, out, face, 1);
}

static unsigned pose_impl(const object_world_t *w, int model, int graph, unsigned anim, float frame,
                          const mat4 *root, object_nodes_t *out, const object_face *face, int skin)
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
			const node_remap *rm = remap_for(w, &mi, model, graph, n);
			anim_sample(w->anims, &g, anim, frame, &pose);
			for (i = 0; i < n; i++) {
				int j = rm->identity ? (int)i : rm->map[i];
				const anim_xform *x;
				if (j < 0 || (unsigned)j >= g.node_count) continue;
				x = &pose.node[j];
				memcpy(local[i].rot, x->rot, sizeof local[i].rot);
				memcpy(local[i].trans, x->trans, sizeof local[i].trans);
				local[i].scale = x->scale;
			}
		}
	}
	if (face && (face->emotion || face->talk)) apply_face(w, &mi, model, local, n, face);
	if (n == 0) { out->count = 0; return 0; }
	if (skel_world(out->world, local, parent, n, root) != 0) { out->count = 0; return 0; }
	if (skin) {
		/* skinning matrices: vertices are stored in bind-pose model space (docs/11); inverse(bind world) is constant per model, so it is kept (bind_inverse) */
		const mat4 *inv = bind_inverse(w, model, &mi, parent, n);
		if (!inv) { out->count = 0; return 0; }
		for (i = 0; i < n; i++) mat4_mul(&out->skin[i], &inv[i], &out->world[i]);
	}
	out->count = n;
	return n;
}

unsigned objects_pose(const object_t *o, const object_world_t *w, object_nodes_t *out)
{
	return objects_pose_model_face(w, o->model, o->graph, o->anim, o->frame, &o->root, out, &o->face);
}

unsigned objects_pose_world(const object_t *o, const object_world_t *w, object_nodes_t *out)
{
	return pose_impl(w, o->model, o->graph, o->anim, o->frame, &o->root, out, &o->face, 0);
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
	int slot;
	float rot[3] = { yaw, 0.0f, 0.0f };
	if (def >= w->scen->def_count) return -1;
	slot = alloc_slot(t);
	if (slot < 0) return -1;
	if (make_object(t, w, slot, def, pos, rot, NULL, -1, 0) != 0) {
		t->obj[slot].flags = OBJ_F_FREE; t->obj[slot].model = -1;
		if ((unsigned)slot + 1 == t->count) t->count--;
		return -1;
	}
	return slot;
}
