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
#include "world/mouth.h"
#include "world/scen.h"

#define OBJ_MAX 1280
#define OBJ_MAX_PLACEMENTS 2048
#define OBJ_MAX_REGIONS 16

#define OBJ_F_ANIMATED 1u       /* has a graph and an animation to play */
#define OBJ_F_VISIBLE 2u        /* initially visible placement */
#define OBJ_F_INACTIVE 4u       /* lies in a BSP that is not loaded: kept, frozen, not drawn */
#define OBJ_F_FREE 8u           /* unused slot (destroyed object), reused by the next spawn */
#define OBJ_F_HIDDEN 16u        /* exists but is not drawn (scripts hide objects) */
#define OBJ_F_HELD 64u          /* a weapon in a unit's hand (hs_host_obj.c): follows the hand marker, cannot be picked up */
#define OBJ_F_POSED 32u         /* drawn at anim/frame but the frame is set by its owner (devices), not advanced by objects_tick */

/* A talking unit's face (docs/12-anim-section.md "Face overlays"): the overlays of the unit's own graph that move the eyelids, brows,
 * lips and jaw on top of whatever it plays. graph = graph index + 1 (0 = no face), emo_anim / talk_anim = animation index + 1 of
 * "stand emotions" / "stand talk", emotion = frame + 1 of the emotions overlay (`unit_set_emotion`, 0 = none), talk = the sound's
 * mouth amplitude of this tick (0..255, the jaw overlay scaled by it). */
typedef struct { int16_t graph, emo_anim, talk_anim; uint8_t emotion, talk; } object_face;

typedef struct {
	int16_t  def;                         /* scen definition */
	int16_t  placement;                   /* scenario placement index it was created from, -1 for runtime objects */
	uint32_t bsp_mask;                    /* BSPs the object lives in (0 = everywhere); mobile objects follow the player */
	uint8_t  type, flags;
	int16_t  model, graph;                /* MODL / ANIM index, -1 = none */
	uint16_t anim;                        /* animation index in the graph (SCEN_NO_ANIM = static) */
	uint16_t region_count;
	float    frame;                       /* animation time in frames (30 Hz) */
	float    pos[3], radius;
	mat4     root;                        /* placement transform */
	uint16_t perm[OBJ_MAX_REGIONS];       /* flat permutation index per region */
	object_face face;
	uint16_t serial;                      /* creation counter (never 0): a destroyed and recreated object in the same slot is a new object (attached effects restart) */
} object_t;

typedef struct {
	object_t obj[OBJ_MAX];
	unsigned count;                       /* slots in use (high-water mark) */
	int16_t  by_placement[OBJ_MAX_PLACEMENTS];   /* placement index -> object slot, -1 not spawned */
	unsigned skipped_no_model, skipped_hidden, skipped_cap, skipped_other_bsp;
	uint16_t next_serial;
} object_table_t;

typedef struct {
	const scen_section  *scen;
	const model_section *models;
	const anim_section  *anims;
	const mouth_section *mouth;           /* lip-sync data (NULL = none) */
} object_world_t;

void objects_clear(object_table_t *t);
/* Instantiate every initially visible placement that lies inside BSP `bsp` (include_hidden: also the ones scripts
 * would create later, as a worst-case population for budget tests). Clears the table first; returns the count. */
unsigned objects_spawn_bsp(object_table_t *t, const object_world_t *w, unsigned bsp, int include_hidden);
/* Phase 4: every initially visible placement of every BSP of the map (the table is not cleared); returns how many were created. */
unsigned objects_spawn_all(object_table_t *t, const object_world_t *w, int include_hidden);
/* Create the object of scenario placement `placement` (a script's object_create); returns the slot, the existing slot when it was spawned already, or -1. */
int  objects_spawn_placement(object_table_t *t, const object_world_t *w, unsigned placement);
/* Remove an object (its slot is reused). */
void objects_destroy(object_table_t *t, unsigned index);
/* Marks objects of other BSPs inactive and the ones in `bsp` (or in every BSP) active. */
void objects_set_active_bsp(object_table_t *t, unsigned bsp);
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
/* The node world matrices only (out->skin is NOT filled): what marker lookups and collision poses need; skips the bind-pose chain and one inverse and one multiply per node. */
unsigned objects_pose_world(const object_t *o, const object_world_t *w, object_nodes_t *out);
/* Same for an arbitrary model/animation with an explicit root (first-person arms, weapons). */
/* Model node i is driven by the graph node of the same NAME (ANIM v2 names); a model whose node order equals the
 * graph's maps 1:1, a node the graph does not name keeps its bind pose. The map is cached per (model, graph). */
unsigned objects_pose_model(const object_world_t *w, int model, int graph, unsigned anim, float frame,
                            const mat4 *root, object_nodes_t *out);
/* Same with a face (NULL = none): the emotion / talk overlays are composed onto the face nodes of the posed skeleton. */
unsigned objects_pose_model_face(const object_world_t *w, int model, int graph, unsigned anim, float frame,
                                 const mat4 *root, object_nodes_t *out, const object_face *face);
/* Fill o->face from the unit's own graph (the face overlays of the graph, none when it has neither). */
void objects_set_face_graph(object_t *o, const object_world_t *w, int graph);
/* Debug: 0 = overlay * base (the overlay turns in the parent's frame), 1 = base * overlay (in the bone's own frame). */
extern int g_face_compose;
/* Fast path for objects that are not animated: every skinning matrix is the placement matrix (bind pose x root);
 * out->world is NOT filled. Returns the node count. */
unsigned objects_pose_static(const object_t *o, const object_world_t *w, object_nodes_t *out);

#endif
