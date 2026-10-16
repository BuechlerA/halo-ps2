/* Object table (docs/03 §6): the placed objects of the current BSP with their animation state.
 * Fixed pool, no heap (ADR-006). Rendering reads it through render/obj_render.c; the simulation of
 * later phases (physics, AI, damage) will write the same records. */
#ifndef SIM_OBJECTS_H
#define SIM_OBJECTS_H
#include <stdint.h>
#include "core/mat.h"
#include "sim/skeleton.h"
#include "world/anim.h"
#include "world/model.h"
#include "world/scen.h"

#define OBJ_MAX 1024
#define OBJ_MAX_REGIONS 16

#define OBJ_F_ANIMATED 1u       /* has a graph and an animation to play */
#define OBJ_F_VISIBLE 2u        /* initially visible placement */

typedef struct {
	int16_t  def;                         /* scen definition */
	uint8_t  type, flags;
	int16_t  model, graph;                /* MODL / ANIM index, -1 = none */
	uint16_t anim;                        /* animation index in the graph (SCEN_NO_ANIM = static) */
	uint16_t region_count;
	float    frame;                       /* animation time in frames (30 Hz) */
	float    pos[3], radius;
	mat4     root;                        /* placement transform */
	uint16_t perm[OBJ_MAX_REGIONS];       /* flat permutation index per region */
} object_t;

typedef struct {
	object_t obj[OBJ_MAX];
	unsigned count;
	unsigned skipped_no_model, skipped_hidden, skipped_cap, skipped_other_bsp;
} object_table_t;

typedef struct {
	const scen_section  *scen;
	const model_section *models;
	const anim_section  *anims;
} object_world_t;

void objects_clear(object_table_t *t);
/* Instantiate every initially visible placement that lies inside BSP `bsp` (include_hidden: also the ones scripts
 * would create later, as a worst-case population for budget tests). Returns the count. */
unsigned objects_spawn_bsp(object_table_t *t, const object_world_t *w, unsigned bsp, int include_hidden);
/* Spawn one object of definition `def` (scen definition index) at `pos` facing `yaw` (radians), e.g. the player's biped
 * for third-person views. Returns the object index or -1. */
int objects_spawn_def(object_table_t *t, const object_world_t *w, unsigned def, const float pos[3], float yaw);
/* One 30 Hz tick: advance animations. */
void objects_tick(object_table_t *t, const object_world_t *w);

/* Node world matrices of `o` in its current pose (animated if it has an animation, else the model's default
 * pose), root = o->root. Returns the node count, or 0 if the object has no skeleton. */
/* world[n]: node in its posed world position (markers, attachments); skin[n] = inverse(default pose world) then world:
 * the matrix that takes bind-pose model-space vertices of node n to the world (what the palette uses). */
typedef struct { mat4 world[SKEL_MAX_NODES]; mat4 skin[SKEL_MAX_NODES]; unsigned count; } object_nodes_t;
unsigned objects_pose(const object_t *o, const object_world_t *w, object_nodes_t *out);
/* Same for an arbitrary model/animation with an explicit root (first-person arms, weapons). */
/* `node_offset`: the model's node i is driven by graph node node_offset + i (first-person weapons share the arms'
 * 42-node graph: arms 0..36, weapon 37..41). */
unsigned objects_pose_model(const object_world_t *w, int model, int graph, unsigned anim, float frame, unsigned node_offset,
                            const mat4 *root, object_nodes_t *out);
/* Fast path for objects that are not animated: every skinning matrix is the placement matrix (bind pose x root);
 * out->world is NOT filled. Returns the node count. */
unsigned objects_pose_static(const object_t *o, const object_world_t *w, object_nodes_t *out);

#endif
