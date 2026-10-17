/* Glue between the data readers and the simulation: player_params_t from the globals/biped tags, and phys_world over a collision BSP. Pure C11, host-tested. */
#ifndef SIM_PLAYERDEF_H
#define SIM_PLAYERDEF_H
#include "sim/phys.h"
#include "sim/player.h"
#include "sim/unitdef.h"
#include "world/coll.h"

/* Converts tag units (wu/tick, wu/tick^2, degrees) into player_params_t (wu/s, rad). Either pointer may be NULL: defaults stay. */
void player_params_from_tags(player_params_t *p, const unitdef_globals_def *g, const unitdef_biped_def *b);
/* A phys_world answering from `bsp` (which must outlive it). */
void phys_world_from_coll(phys_world *w, const coll_bsp *bsp);

#endif
