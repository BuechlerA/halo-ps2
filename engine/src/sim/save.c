#include "sim/save.h"
#include <string.h>

void checkpoint_save(checkpoint_t *c, const game_t *g, const fpview_t *fp, const fx_t *fx)
{
	c->game = *g;
	c->objects = *g->objects;
	c->fp = *fp;
	c->has_fx = fx != NULL;
	if (fx) c->fx = *fx;
	c->tick = g->tick;
	c->valid = 1;
}

int checkpoint_revert(const checkpoint_t *c, game_t *g, object_table_t *objects, fpview_t *fp, fx_t *fx)
{
	if (!c->valid) return 0;
	*g = c->game;
	*objects = c->objects;
	*fp = c->fp;
	if (fx && c->has_fx) *fx = c->fx;
	g->objects = objects; g->fp = fp;       /* the pointers are the live ones */
	g->ev_count = 0;
	return 1;
}
