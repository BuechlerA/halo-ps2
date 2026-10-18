#include "sim/mission.h"
#include "ai/ai.h"
#include "script/hs_host_misc.h"
#include "script/hs_host_obj.h"
#include <math.h>
#include <string.h>

void mission_init(mission_t *m)
{
	memset(m, 0, sizeof *m);
	m->want_bsp = -1;
	m->player_obj = -1;
	m->difficulty = 1;
}

void mission_note_missing(mission_t *m, const char *what)
{
	(void)what;
	m->missing_noted[0]++;
}

void mission_note_awaited_volume(mission_t *m, unsigned volume, int player_inside)
{
	unsigned i, free_slot = 8, oldest = 0;
	for (i = 0; i < 8; i++) {
		mission_awaited *a = &m->awaited_volume[i];
		if (a->active && a->volume == volume) {
			if (player_inside) a->active = 0; else { a->last_tick = m->tick; a->hits++; }
			return;
		}
		if (!a->active && free_slot == 8) free_slot = i;
		if (m->awaited_volume[i].last_tick < m->awaited_volume[oldest].last_tick) oldest = i;
	}
	if (player_inside) return;
	i = free_slot < 8 ? free_slot : oldest;
	m->awaited_volume[i].volume = (uint16_t)volume; m->awaited_volume[i].active = 1; m->awaited_volume[i].last_tick = m->tick; m->awaited_volume[i].hits = 1;
}

/* ---- lists ---- */
mission_list *mission_list_new(mission_t *m, uint32_t *handle_out)
{
	unsigned slot = m->list_next++ % MISSION_LISTS;
	m->lists[slot].count = 0;
	if (handle_out) *handle_out = MISSION_LIST_BIT | slot;
	return &m->lists[slot];
}

mission_list *mission_list_get(mission_t *m, uint32_t handle)
{
	if (!(handle & MISSION_LIST_BIT) || handle == HS_HANDLE_NONE) return NULL;
	handle &= ~MISSION_LIST_BIT;
	return handle < MISSION_LISTS ? &m->lists[handle] : NULL;
}

void mission_list_add(mission_list *l, int obj)
{
	if (l->count < MISSION_LIST_MAX) l->obj[l->count++] = (int16_t)obj;
}

const mission_list *mission_arg_list(mission_t *m, const hs_value *a, mission_list *tmp)
{
	mission_list *l;
	if (a->type != HS_HANDLE) return NULL;
	l = mission_list_get(m, a->v.h);
	if (l) return l;
	tmp->count = 0;
	{
		int o = mission_resolve_object(m, a, 0);
		if (o >= 0) mission_list_add(tmp, o);
	}
	return tmp;
}

/* ---- objects ---- */
int mission_object_live(const mission_t *m, int obj)
{
	return m->objects && obj >= 0 && (unsigned)obj < m->objects->count && !(m->objects->obj[obj].flags & OBJ_F_FREE);
}

void mission_object_pos(const mission_t *m, int obj, float out[3])
{
	out[0] = out[1] = out[2] = 0.0f;
	if (!mission_object_live(m, obj)) return;
	out[0] = m->objects->obj[obj].pos[0]; out[1] = m->objects->obj[obj].pos[1]; out[2] = m->objects->obj[obj].pos[2] + 0.5f * m->objects->obj[obj].radius;
}

int mission_object_by_name(mission_t *m, uint32_t name_index, int create)
{
	devdef_name nm;
	int slot;
	if (!m->dev || name_index >= m->dev->name_count) return -1;
	nm = devdef_get_name(m->dev, name_index);
	if (!(nm.flags & DEVDEF_NF_RESOLVED) || nm.scen_index == DEVDEF_NONE || nm.scen_index >= OBJ_MAX_PLACEMENTS) return -1;
	slot = m->objects->by_placement[nm.scen_index];
	if (slot >= 0 && mission_object_live(m, slot)) return slot;
	return create ? mission_spawn_named(m, name_index) : -1;
}

int mission_resolve_object(mission_t *m, const hs_value *a, int create)
{
	if (a->type != HS_HANDLE || a->v.h == HS_HANDLE_NONE) return -1;
	if (a->v.h & MISSION_LIST_BIT) {                       /* a list used as an object: its first element */
		mission_list *l = mission_list_get(m, a->v.h);
		return l && l->count ? l->obj[0] : -1;
	}
	if (a->v.h & MISSION_OBJ_BIT) {                        /* a runtime handle: MISSION_OBJ_BIT | slot */
		int slot = (int)(a->v.h & 0xFFFFu);
		return mission_object_live(m, slot) ? slot : -1;
	}
	if (a->vtype == 23 || (a->vtype >= 37 && a->vtype <= 43)) return mission_object_by_name(m, a->v.h, create);   /* object-name constants */
	if (mission_object_live(m, (int)a->v.h)) return (int)a->v.h;     /* legacy raw slot of another vtype */
	return -1;
}

void mission_destroy_object(mission_t *m, int obj)
{
	if (!mission_object_live(m, obj)) return;
	if (m->units) units_detach(m->units, units_of_object(m->units, obj));
	objects_destroy(m->objects, (unsigned)obj);
	if (m->game && obj >= 0) { m->game->unit_state[obj].model = -1; m->game->unit_state[obj].has_vitals = 0; }
}

static void after_spawn(mission_t *m, int slot)
{
	object_t *o = &m->objects->obj[slot];
	if (m->game) game_object_spawned(m->game, (unsigned)slot);
	o->bsp_mask = m->cur_bsp >= 0 ? (1u << m->cur_bsp) : 0;
	if (o->type == SCEN_BIPED && m->units) {
		if (units_attach(m->units, m->game, slot, 0, 0) >= 0 && m->dev) { /* placed bipeds start idle under script control until an encounter takes them */ }
	}
}

int mission_spawn_named(mission_t *m, uint32_t name_index)
{
	devdef_name nm;
	int slot;
	if (!m->dev || name_index >= m->dev->name_count) return -1;
	nm = devdef_get_name(m->dev, name_index);
	if (!(nm.flags & DEVDEF_NF_RESOLVED) || nm.scen_index == DEVDEF_NONE) return -1;
	slot = objects_spawn_placement(m->objects, &m->ow, nm.scen_index);
	if (slot < 0) return -1;
	{
		object_t *o = &m->objects->obj[slot];
		if (o->bsp_mask == 0 || (m->cur_bsp >= 0 && (o->bsp_mask & (1u << m->cur_bsp)))) o->flags &= ~OBJ_F_INACTIVE; else o->flags |= OBJ_F_INACTIVE;
	}
	if (m->game) game_object_spawned(m->game, (unsigned)slot);
	return slot;
}

int mission_spawn_def_at(mission_t *m, uint32_t def, const float pos[3], float yaw)
{
	int slot = objects_spawn_def(m->objects, &m->ow, def, pos, yaw);
	if (slot < 0) return -1;
	m->objects->obj[slot].bsp_mask = m->cur_bsp >= 0 ? (1u << m->cur_bsp) : 0;
	after_spawn(m, slot);
	return slot;
}

/* ---- player body ---- */
void mission_attach_player(mission_t *m)
{
	scen_player pl;
	float pos[3];
	if (!m->game || !m->ow.scen) return;
	pl = scen_get_player(m->ow.scen);
	if (!pl.biped_def) return;
	pos[0] = m->game->player.pos[0]; pos[1] = m->game->player.pos[1]; pos[2] = m->game->player.pos[2];
	m->player_obj = objects_spawn_def(m->objects, &m->ow, pl.biped_def - 1, pos, m->game->player.yaw);
	if (m->player_obj >= 0) {
		object_t *o = &m->objects->obj[m->player_obj];
		o->flags |= OBJ_F_HIDDEN;                              /* first person: the body is not drawn */
		o->bsp_mask = 0;
		m->game->unit_state[m->player_obj].model = -1;         /* the player's hits are handled by the capsule, not by hit shapes */
		m->game->unit_state[m->player_obj].has_vitals = 0;
		m->game->player_obj = m->player_obj;
	}
}

void mission_sync_player(mission_t *m)
{
	object_t *o;
	if (m->player_obj < 0 || !mission_object_live(m, m->player_obj)) return;
	o = &m->objects->obj[m->player_obj];
	o->pos[0] = m->game->player.pos[0]; o->pos[1] = m->game->player.pos[1]; o->pos[2] = m->game->player.pos[2];
	mat4_from_ypr(&o->root, m->game->player.yaw, 0.0f, 0.0f, o->pos);
	o->flags &= ~OBJ_F_INACTIVE;
}

void mission_sync_devices(mission_t *m)
{
	/* the placement and animation of every device are fixed for the mission: decode them once (the per-tick decode of the packed records cost over 1 ms) */
	static const devdef_section *cache_sec;
	static unsigned cache_count;
	static uint32_t scen[DEV_MAX_DEVICES];
	static int32_t  anim[DEV_MAX_DEVICES];                      /* pos_anim, -1 none */
	unsigned i;
	if (!m->devices || !m->dev || !m->objects) return;
	if (cache_sec != m->dev || cache_count != m->devices->count) {
		cache_sec = m->dev; cache_count = m->devices->count > DEV_MAX_DEVICES ? DEV_MAX_DEVICES : m->devices->count;
		for (i = 0; i < cache_count; i++) {
			devdef_device pd = devdef_get_device(m->dev, i);
			devdef_def df = devdef_get_def(m->dev, pd.def);
			scen[i] = pd.scen_index;
			anim[i] = (pd.scen_index == DEVDEF_NONE || pd.scen_index >= OBJ_MAX_PLACEMENTS || df.pos_anim == DEVDEF_NO_ANIM) ? -1 : (int32_t)df.pos_anim;
		}
	}
	for (i = 0; i < cache_count; i++) {
		object_t *o;
		int slot;
		if (anim[i] < 0) continue;
		slot = m->objects->by_placement[scen[i]];
		if (!mission_object_live(m, slot)) continue;
		o = &m->objects->obj[slot];
		if (o->graph < 0) continue;
		o->anim = (uint16_t)anim[i];
		o->frame = devices_anim_frame(m->devices, i);
		o->flags = (o->flags & ~OBJ_F_ANIMATED) | OBJ_F_POSED;
	}
}

/* zones: 0 scripts, 1 devices, 2 object pre-hook, 3 AI, 4 units, 5 game, 6 post-hook + misc, 7 cinematics */
#define ZONE(z, stmt) do { uint32_t zt = m->clock ? m->clock() : 0; stmt; if (m->clock) { float zms = m->clock_ms(zt, m->clock()); m->zone_sum[z] += zms; if (zms > m->zone_peak[z]) m->zone_peak[z] = zms; } } while (0)
void mission_tick(mission_t *m, const game_input_t *in)
{
	game_input_t none;
	if (m->game) m->game->player_frozen = m->cinematic;
	mission_sync_player(m);
	ZONE(0, if (m->hs) hs_tick(m->hs));
	ZONE(1, if (m->devices) { devices_tick(m->devices); mission_sync_devices(m); });
	ZONE(2, hs_host_obj_pre(m, in));                           /* recordings feed their units; latches the player's input for the action tests */
	ZONE(3, if (m->ai) ai_tick(m->ai));                        /* decisions: unit controls */
	if (m->units) m->units->lod_all = (m->cinematic && m->units->lod_cine) ? 1u : 0u;        /* a cinematic: nobody plays it, bodies integrate at half rate (ADR-063) */
	ZONE(4, if (m->units && m->game) units_tick(m->units, m->game));    /* execution */
	memset(&none, 0, sizeof none);
	ZONE(5, if (m->game) game_tick(m->game, (m->cinematic || m->hobj.player_input_disabled) ? &none : in));
	ZONE(6, hs_host_obj_post(m); misc_tick(m));
	ZONE(7, if (m->cine) cine_tick(m->cine));
	m->zone_n++;
	m->tick++;
}
