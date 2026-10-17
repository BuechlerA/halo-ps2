#include "sim/input.h"
#include <stdlib.h>
#include <string.h>

/* The PS2 pad button bits (platform/pad.h); duplicated so this file builds on the host. */
#define B_SELECT 0x0001u
#define B_L3     0x0002u
#define B_R3     0x0004u
#define B_START  0x0008u
#define B_L2     0x0100u
#define B_R2     0x0200u
#define B_L1     0x0400u
#define B_R1     0x0800u
#define B_TRI    0x1000u
#define B_CIRCLE 0x2000u
#define B_CROSS  0x4000u
#define B_SQUARE 0x8000u

void input_map(game_input_t *out, const raw_input_t *raw, unsigned prev_held)
{
	unsigned b = raw->buttons, h = 0;
	memset(out, 0, sizeof *out);
	if (b & B_CROSS) h |= (b & B_SELECT) ? IN_SAVE : IN_JUMP;
	if (b & B_L3) h |= IN_CROUCH;
	if (b & B_R2) h |= IN_FIRE;
	if (b & B_L2) h |= IN_GRENADE;
	if (b & B_CIRCLE) h |= IN_MELEE;
	if (b & B_SQUARE) h |= IN_RELOAD;
	if (b & B_TRI) h |= IN_SWITCH;
	if (b & B_L1) h |= IN_SWAPGREN;
	if (b & B_R1) h |= IN_LIGHT;
	if (b & B_R3) h |= IN_ZOOM;
	if ((b & B_SELECT) && !(b & B_START) && !(b & B_CROSS)) h |= IN_OVERLAY;
	if (b & B_START) h |= (b & B_SELECT) ? IN_REVERT : IN_PAUSE;
	out->held = h;
	out->pressed = h & ~prev_held;
	out->move.move_x = raw->lx; out->move.move_y = raw->ly;
	out->move.look_x = raw->rx; out->move.look_y = raw->ry;
	out->move.jump = (h & IN_JUMP) != 0;
	out->move.crouch = (h & IN_CROUCH) != 0;
	out->move.walk = 0;
}

int input_script_parse(input_script *s, const cfg_t *cfg)
{
	int i;
	const char *v;
	s->count = 0;
	for (i = 0; (v = cfg_nth(cfg, "input", i)) != NULL && s->count < INPUT_SCRIPT_MAX; i++) {
		char *e;
		input_key k;
		k.t = strtof(v, &e);          if (e == v) continue;
		v = e; k.lx = strtof(v, &e);  if (e == v) continue;
		v = e; k.ly = strtof(v, &e);  if (e == v) continue;
		v = e; k.rx = strtof(v, &e);  if (e == v) continue;
		v = e; k.ry = strtof(v, &e);  if (e == v) continue;
		v = e; k.buttons = (unsigned)strtoul(v, &e, 0);
		if (s->count > 0 && k.t < s->key[s->count - 1].t) continue;
		s->key[s->count++] = k;
	}
	return s->count;
}

void input_script_eval(const input_script *s, float t, raw_input_t *out)
{
	int i, k = -1;
	memset(out, 0, sizeof *out);
	for (i = 0; i < s->count; i++) if (s->key[i].t <= t) k = i;
	if (k < 0) return;
	out->lx = s->key[k].lx; out->ly = s->key[k].ly; out->rx = s->key[k].rx; out->ry = s->key[k].ry;
	out->buttons = s->key[k].buttons;
}
