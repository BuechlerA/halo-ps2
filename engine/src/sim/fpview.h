/* First-person view model: the player's arms and the weapon, attached to the camera (docs/04 Phase 2).
 * The arms animate (idle) on the tick; the weapon follows the arms' hand marker. */
#ifndef SIM_FPVIEW_H
#define SIM_FPVIEW_H
#include "render/camera.h"
#include "sim/objects.h"

typedef struct {
	int       active;
	int       arms_model, arms_graph, weapon_model, weapon_graph;
	unsigned  arms_anim, weapon_anim;
	int       hand_node;                   /* node named by weapon_hand_marker when the model has no such marker */
	int       hand_marker;                 /* marker index on the arms model, -1 = fall back to the arms root */
	float     offset[3];                   /* camera-space offset (right, up, forward) */
	float     arms_frame, weapon_frame;
	mat4      arms_root, weapon_root;      /* set by fpview_place */
	object_nodes_t arms_nodes;
} fpview_t;

/* Resolve the model/animation indices of the player data. active = 0 if the arms model is missing. */
void fpview_init(fpview_t *f, const object_world_t *w, const scen_player *p);
void fpview_tick(fpview_t *f, const object_world_t *w);
/* Attach to the camera and pose the arms; computes the root matrices used by the renderer. */
void fpview_place(fpview_t *f, const object_world_t *w, const camera_t *cam);

#endif
