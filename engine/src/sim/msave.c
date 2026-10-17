#include "sim/msave.h"
#include <string.h>

#define MSAVE_MAGIC 0x5653534Du       /* 'MSSV' */

void msave_save(mission_checkpoint_t *c, mission_t *m, const fx_t *fx)
{
	checkpoint_save(&c->base, m->game, m->game->fp, fx);
	c->cur_bsp = m->cur_bsp; c->tick = m->tick; c->difficulty = (uint32_t)m->difficulty;
	c->won = m->won; c->lost = m->lost; c->cinematic = m->cinematic; c->player_obj = m->player_obj; c->objective_flags = m->objective_flags;
	if (m->units) c->units = *m->units;
	if (m->ai) c->ai = *ai_state_ptr(m->ai);
	if (m->hs) c->hs = *hs_state_ptr(m->hs);
	if (m->devices) c->devices = *m->devices;
	if (m->cine) c->cine = *m->cine;
	c->misc = m->misc;
	c->hobj = m->hobj;
	c->valid = 1;
}

int msave_bsp(const mission_checkpoint_t *c) { return c->valid ? c->cur_bsp : -1; }

static void cine_rebind(cine_state *cs, const cinedef_section *sec)
{
	unsigned i;
	for (i = 0; i < CINE_MAX_PLAYS; i++) {
		cine_play_slot *s = &cs->plays[i];
		if (!s->used) continue;
		s->play.sec = sec;
		s->play.stream = sec ? cinedef_rec_stream(sec, s->play.rec, &s->play.bytes) : NULL;
		if (!s->play.stream) s->used = 0;
	}
}

int msave_revert(const mission_checkpoint_t *c, mission_t *m, fx_t *fx)
{
	if (!c->valid) return 0;
	if (!checkpoint_revert(&c->base, m->game, m->objects, m->game->fp, fx)) return 0;
	m->tick = c->tick; m->difficulty = (int)c->difficulty;
	m->won = c->won; m->lost = c->lost; m->cinematic = c->cinematic; m->player_obj = c->player_obj; m->objective_flags = c->objective_flags;
	m->want_bsp = -1;
	if (m->units) *m->units = c->units;
	if (m->devices) { const devdef_section *sec = m->devices->sec; *m->devices = c->devices; devices_rebind(m->devices, sec ? sec : m->dev); }
	if (m->cine) { *m->cine = c->cine; cine_rebind(m->cine, m->cin); }
	{
		mission_misc_env_t env = m->misc_env;                 /* pointers and hooks are the live ones */
		m->misc = c->misc; m->misc_env = env;
	}
	m->hobj = c->hobj;
	if (m->ai) {
		*ai_state_ptr(m->ai) = c->ai;
		if (ai_rebind(m->ai, &m->ai->env) != 0) return 0;
	}
	if (m->hs) {
		*hs_state_ptr(m->hs) = c->hs;
		if (hs_rebind(m->hs, m->scripts, m) != HS_OK) return 0;
	}
	return 1;
}

size_t msave_pack_max(void) { return blob_pack_max(sizeof(mission_checkpoint_t)); }
size_t msave_pack(const mission_checkpoint_t *c, uint8_t *out, size_t cap) { return blob_pack(c, sizeof *c, MSAVE_MAGIC, out, cap); }
int msave_unpack(mission_checkpoint_t *c, const uint8_t *in, size_t n)
{
	if (!blob_unpack(c, sizeof *c, MSAVE_MAGIC, in, n)) return 0;
	return c->valid == 1;
}
