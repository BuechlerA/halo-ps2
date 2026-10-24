#include "sim/bot.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#define PICKUP_REACH 1.4f

static void bot_report(const bot_t *b, const mission_t *m, const char *what, const float from[3], const float to[3])
{
	const game_t *g = m->game;
	phys_hit h;
	float down[3];
	int hit = 0;
	if (!m->log) return;
	down[0] = to[0]; down[1] = to[1]; down[2] = to[2] - 60.0f;
	if (g->has_phys) hit = g->phys.ray(g->phys.user, to, down, &h);
	{
		unsigned i;
		printf("[bot] awaited volumes at tick %u:", (unsigned)m->tick);
		for (i = 0; i < 8; i++) if (m->awaited_volume[i].active) printf(" %u(age %u hits %u)", (unsigned)m->awaited_volume[i].volume, (unsigned)(m->tick - m->awaited_volume[i].last_tick), (unsigned)m->awaited_volume[i].hits);
		printf("\n");
	}
	printf("[bot] %s: tick %u from %.2f %.2f %.2f to %.2f %.2f %.2f goal kind %d id %u at %.2f %.2f %.2f route %d start %d goal %d floor below target %s %.2f\n",
	       what, (unsigned)m->tick, (double)from[0], (double)from[1], (double)from[2], (double)to[0], (double)to[1], (double)to[2], b->goal_kind, (unsigned)b->goal_id,
	       (double)b->goal[0], (double)b->goal[1], (double)b->goal[2], b->route_status, b->route_start, b->route_goal, hit ? "yes" : "NONE", hit ? (double)(to[2] - 60.0f * h.t) : 0.0);
}

#define BOT_PIN_TICKS 20u      /* scripts poll their volume tests (sleep_until) every few ticks: hold a floorless hop target for a couple of polls */
static int floor_below(const game_t *g, const float p[3], float depth)
{
	phys_hit h;
	float down[3];
	down[0] = p[0]; down[1] = p[1]; down[2] = p[2] - depth;
	return !g->has_phys || g->phys.ray(g->phys.user, p, down, &h);
}

/* Skip a volume until tick `until`. A volume already listed is extended; otherwise the slot that expires first is reused (the scripts wait on a dozen volumes
 * at once and most lie in other BSPs, so a handful of slots would be overwritten before they expire). */
static void ignore_volume(bot_t *b, const mission_t *m, uint16_t vol, uint32_t until)
{
	unsigned q, pick = 0;
	for (q = 0; q < BOT_IGNORE_MAX; q++) {
		if (b->ignore_vol[q] == vol && b->ignore_until[q] > m->tick) { b->ignore_until[q] = until; return; }
		if (b->ignore_until[q] < b->ignore_until[pick]) pick = q;
	}
	b->ignore_vol[pick] = vol; b->ignore_until[pick] = until;
}

/* Teleport the player onto `to` (an assist, counted). A target with no floor under it is held for BOT_PIN_TICKS so the scripts can see the player inside the volume. */
static void bot_hop(bot_t *b, mission_t *m, const char *what, const float to[3])
{
	game_t *g = m->game;
	bot_report(b, m, what, g->player.pos, to);
	game_teleport(g, to, g->player.yaw);
	b->hop_tick = m->tick;
	if (!floor_below(g, to, 80.0f)) { memcpy(b->pin_pos, to, sizeof b->pin_pos); b->pin_until = m->tick + BOT_PIN_TICKS; }
	b->assists++; b->stuck_ticks = 0; b->unreach_ticks = 0; b->replan_at = 0; b->wp_n = 0; b->progress_dist = 0.0f;
}

static float dist2d(const float a[3], const float b[3]) { float dx = a[0] - b[0], dy = a[1] - b[1]; return sqrtf(dx * dx + dy * dy); }

void bot_init(bot_t *b)
{
	memset(b, 0, sizeof *b);
	b->safe_pos[2] = -1e30f;
	b->enabled = 1;
	b->target_unit = -1;
}

/* Nearest hostile unit with a clear line of sight from the player's eye (and the world's collision). Returns the unit index or -1. */
static int find_enemy(bot_t *b, mission_t *m, const float eye[3], float out_pos[3], float *out_dist)
{
	unsigned i;
	int best = -1;
	float bestd = 40.0f;
	game_t *g = m->game;
	for (i = 0; i < m->units->count; i++) {
		const unit_t *u = &m->units->u[i];
		float t[3], d[3], dist;
		phys_hit h;
		unsigned q;
		int shunned = 0;
		if (!u->active || u->obj < 0 || !units_teams_hostile(1, u->team) || !units_alive(m->units, g, (int)i)) continue;
		for (q = 0; q < BOT_SHUN_MAX; q++) if (b->shun_unit[q] == (int16_t)i && b->shun_until[q] > m->tick) shunned = 1;
		if (shunned) continue;
		if (m->objects->obj[u->obj].flags & (OBJ_F_INACTIVE | OBJ_F_FREE)) continue;
		t[0] = u->body.pos[0]; t[1] = u->body.pos[1]; t[2] = u->body.pos[2] + 0.45f;
		d[0] = t[0] - eye[0]; d[1] = t[1] - eye[1]; d[2] = t[2] - eye[2];
		dist = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
		if (dist >= bestd) continue;
		if (g->has_phys && g->phys.ray(g->phys.user, eye, t, &h)) continue;
		bestd = dist; best = (int)i;
		out_pos[0] = t[0]; out_pos[1] = t[1]; out_pos[2] = t[2];
	}
	*out_dist = bestd;
	return best;
}

static int pick_goal(bot_t *b, mission_t *m)
{
	unsigned i;
	float best = 1e30f;
	int found = 0;
	const float *pp = m->game->player.pos;
	{                                                           /* unarmed: the nearest weapon lying about comes first (not on a10, which starts unarmed: its script gives the pistol) */
		const game_slot_t *s0 = game_cur_slot(m->game);
		if ((!s0 || s0->weapon < 0) && !m->game->unarmed_start) {
			unsigned k;
			float bd = 1e30f;
			for (k = 0; k < m->objects->count; k++) {
				const object_t *o = &m->objects->obj[k];
				float d;
				if (o->type != SCEN_WEAPON || o->model < 0 || (o->flags & (OBJ_F_INACTIVE | OBJ_F_FREE | OBJ_F_HIDDEN))) continue;
				d = dist2d(pp, o->pos);
				if (d < bd && d < 60.0f) { bd = d; b->goal[0] = o->pos[0]; b->goal[1] = o->pos[1]; b->goal[2] = o->pos[2]; found = 1; b->goal_id = k; }
			}
			if (found) { b->goal_kind = 3; return 1; }
		}
	}
	/* 1. a weapon pickup in reach when the current weapon is low on ammo is handled by the caller; here: the trigger volume the scripts wait for */
	for (i = 0; i < 8; i++) {
		const mission_awaited *a = &m->awaited_volume[i];
		devdef_volume v;
		float c[3], d;
		unsigned q;
		int skip = 0;
		if (!a->active || m->tick - a->last_tick > 90 || !m->dev || a->volume >= m->dev->volume_count) continue;
		for (q = 0; q < BOT_IGNORE_MAX; q++) if (b->ignore_vol[q] == a->volume && b->ignore_until[q] > m->tick) skip = 1;
		if (skip) continue;
		v = devdef_get_volume(m->dev, a->volume);
		volume_center(&v, c);
		d = dist2d(pp, c);
		if (d < best) { best = d; b->goal[0] = c[0]; b->goal[1] = c[1]; b->goal[2] = c[2]; b->goal_id = a->volume; found = 1; }
	}
	if (found) { b->goal_kind = 1; return 1; }
	/* 2. the most recent navpoint */
	if (mission_navpoint_target(m, b->goal)) { b->goal_kind = 2; return 1; }
	return 0;
}

static void follow(bot_t *b, mission_t *m, const path_bsp *path, path_scratch *scratch, const float pos[3], float wish[3])
{
	wish[0] = wish[1] = wish[2] = 0.0f;
	if (!b->has_goal) return;
	if (path && scratch && (m->tick >= b->replan_at || b->wp_i >= b->wp_n)) {
		path_query q;
		path_route_info info;
		path_query_default(&q);
		q.radius = 0.2f;
		q.kinds = PATH_KIND(PATH_LINK_WALK) | PATH_KIND(PATH_LINK_STEP) | PATH_KIND(PATH_LINK_VAULT) | PATH_KIND(PATH_LINK_DROP);
		q.max_drop = 4.0f;
		b->wp_n = path_route(path, scratch, &q, pos, b->goal, b->wp, BOT_WP_MAX, &info);
		b->route_status = (int)info.status; b->route_start = info.start_sector; b->route_goal = info.goal_sector;
		if (info.status == PATH_ROUTE_NO_GOAL && b->goal_kind == 1) {            /* the volume lies in another BSP: ignore it for 10 s */
			b->nogoal_n++;                    /* any off-mesh volume: with several awaited ones the bot alternates between them */
			if (b->nogoal_n >= 3) {          /* nothing nearer kept the bot busy for three off-mesh goals in a row: the scripts wait on this volume, so hop onto it (an assist) */
				float to[3] = { b->goal[0], b->goal[1], b->goal[2] };
				b->nogoal_n = 0;
				ignore_volume(b, m, (uint16_t)b->goal_id, m->tick + 900);
				bot_hop(b, m, "assist off-mesh volume", to);
			} else {
				ignore_volume(b, m, (uint16_t)b->goal_id, m->tick + 300);
				b->has_goal = 0;
			}
		}
		if (info.status != PATH_ROUTE_NO_GOAL) b->nogoal_n = 0;
		b->wp_i = 0;
		b->replans++;
		{ static unsigned dbg; if (dbg++ < 6 && m->log) printf("[bot] route from %.2f %.2f %.2f to %.2f %.2f %.2f: status %d start %d goal %d sectors %u count %d\n", (double)pos[0], (double)pos[1], (double)pos[2], (double)b->goal[0], (double)b->goal[1], (double)b->goal[2], (int)info.status, (int)info.start_sector, (int)info.goal_sector, (unsigned)info.sector_count, b->wp_n); }
		b->replan_at = m->tick + 45;
	}
	if (b->wp_n > 0 && b->wp_i < b->wp_n) {
		const path_waypoint *w = &b->wp[b->wp_i];
		while (b->wp_i < b->wp_n - 1 && dist2d(pos, b->wp[b->wp_i].pos) < 0.45f) b->wp_i++;
		w = &b->wp[b->wp_i];
		wish[0] = w->pos[0] - pos[0]; wish[1] = w->pos[1] - pos[1];
		wish[2] = (w->kind == PATH_LINK_VAULT && dist2d(pos, w->pos) < 0.9f) ? 1.0f : 0.0f;       /* jump up the riser */
	} else {
		wish[0] = b->goal[0] - pos[0]; wish[1] = b->goal[1] - pos[1];
	}
}

void bot_tick(bot_t *b, mission_t *m, const path_bsp *path, path_scratch *scratch, game_input_t *in)
{
	game_t *g = m->game;
	float eye[3], fwd[3], right[3], epos[3], edist = 0.0f, wish[3];
	const game_slot_t *sl;
	int enemy, moving = 0;
	memset(in, 0, sizeof *in);
	if (!b->enabled || !g || !m->units || vitals_dead(&g->vitals) || m->cinematic) return;
	if (m->tick < b->pin_until) {                               /* held on a floorless volume: no falling, no input */
		game_teleport(g, b->pin_pos, g->player.yaw);
		b->stuck_ticks = 0; b->unreach_ticks = 0;
		return;
	}
	if (m->cur_bsp != b->safe_bsp) { b->safe_bsp = m->cur_bsp; b->safe_pos[2] = -1e30f; }   /* positions of another BSP are no rescue target */
	if (g->player.on_ground) memcpy(b->safe_pos, g->player.pos, sizeof b->safe_pos);
	else if (g->has_phys && m->tick - b->hop_tick >= 4 && b->safe_pos[2] > -1e6f) {
		/* An assisted hop can land on a volume that lies over no floor of the loaded BSP (its centre is in the next BSP's space): the scripts see the
		 * player inside it for the tick(s) they need, then the player must not fall for ever (the renderer then has no cluster and draws everything).
		 * No floor within 80 wu below: put the player back where it last stood. A drop that ends on a floor is left alone. */
		if (!floor_below(g, g->player.pos, 80.0f)) {
			float to[3] = { b->safe_pos[0], b->safe_pos[1], b->safe_pos[2] + 0.05f };
			bot_report(b, m, "rescue (no floor below)", g->player.pos, to);
			game_teleport(g, to, g->player.yaw);
			b->rescues++; b->assists++; b->stuck_ticks = 0; b->unreach_ticks = 0; b->replan_at = 0; b->wp_n = 0;
		}
	}
	game_eye(g, eye, fwd);
	right[0] = sinf(g->player.yaw); right[1] = -cosf(g->player.yaw); right[2] = 0.0f;
	sl = game_cur_slot(g);

	/* combat */
	enemy = find_enemy(b, m, eye, epos, &edist);
	if (!sl || sl->weapon < 0) enemy = -1;                      /* unarmed: nothing to shoot with */
	if (enemy >= 0) {                                           /* give up on a target that does not lose health */
		const game_unit_t *gu = &g->unit_state[m->units->u[enemy].obj];
		float hp = gu->vitals.health + gu->vitals.shield;
		if (b->engage_unit != enemy || hp < b->engage_hp - 0.5f) { b->engage_unit = enemy; b->engage_start = m->tick; b->engage_hp = hp; }
		if (m->tick - b->engage_start > 300) {
			{ unsigned q = b->shun_next++ % BOT_SHUN_MAX; b->shun_unit[q] = (int16_t)enemy; b->shun_until[q] = m->tick + 1800; }
			b->engage_unit = -1; enemy = -1;
		}
	}
	b->target_unit = enemy;
	if (enemy >= 0) {
		float d[3] = { epos[0] - eye[0], epos[1] - eye[1], epos[2] - eye[2] };
		g->player.yaw = atan2f(d[1], d[0]);
		g->player.pitch = atan2f(d[2], sqrtf(d[0] * d[0] + d[1] * d[1]));
		in->held |= IN_FIRE;
		b->shots_ticks++;
		b->enemies_seen++;
		if ((m->tick & 255) == 100 && g->grenades[g->grenade_type] > 0 && edist > 5.0f && edist < 18.0f) in->held |= IN_GRENADE;
	}
	/* weapon upkeep */
	if (sl && sl->weapon >= 0 && sl->def.magazine_count) {
		const weap_state *st = &sl->st;
		unsigned mag = sl->def.trigger_count ? sl->def.trigger[0].magazine : 0;
		if (mag < sl->def.magazine_count && st->loaded[mag] == 0 && st->mode == WEAP_MODE_IDLE) {
			if (st->reserve[mag] > 0 && (m->tick & 7) == 0) in->held |= IN_RELOAD;
			else if (st->reserve[mag] == 0 && g->slot[1 - g->cur].weapon >= 0 && (m->tick & 15) == 0) in->held |= IN_SWITCH;
		}
	}
	if (g->nearby_weapon >= 0 && (m->tick & 15) == 0 && (!sl || sl->weapon < 0 || (sl->st.reserve[0] == 0 && sl->st.loaded[0] == 0))) in->held |= IN_SWITCH;

	/* navigation */
	if (!pick_goal(b, m)) { b->has_goal = 0; b->goal_kind = 0; } else b->has_goal = 1;
	if (b->has_goal && !(enemy >= 0 && edist < 7.0f)) {
		float len;
		follow(b, m, path, scratch, g->player.pos, wish);
		len = sqrtf(wish[0] * wish[0] + wish[1] * wish[1]);
		if (len > 0.05f) {
			wish[0] /= len; wish[1] /= len;
			if (enemy < 0) {                          /* no enemy: face where we go */
				g->player.yaw = atan2f(wish[1], wish[0]);
				g->player.pitch = 0.0f;
			}
			right[0] = sinf(g->player.yaw); right[1] = -cosf(g->player.yaw);
			in->move.move_y = wish[0] * cosf(g->player.yaw) + wish[1] * sinf(g->player.yaw);
			in->move.move_x = wish[0] * right[0] + wish[1] * right[1];
			moving = 1;
			if (wish[2] > 0.5f) { in->held |= IN_JUMP; in->move.jump = 1; }
		}
	}
	in->move.jump = 0;
	/* nothing to do and nobody to fight: the scripts may be waiting for the player to demonstrate a control (tutorial steps), so cycle through them */
	if (!b->has_goal && enemy < 0) {
		unsigned pat = (m->tick / 25u) % 16u;
		switch (pat) {
		case 0: in->move.look_x = 1.0f; break;
		case 1: in->move.look_x = -1.0f; break;
		case 2: in->move.look_y = 1.0f; break;
		case 3: in->move.look_y = -1.0f; break;
		case 4: in->move.move_y = -1.0f; break;
		case 5: in->held |= IN_JUMP; in->move.jump = 1; break;
		case 6: in->held |= IN_CROUCH; in->move.crouch = 1; break;
		case 7: in->held |= IN_LIGHT; break;
		case 8: in->held |= IN_MELEE; break;
		case 9: in->held |= IN_FIRE; break;
		case 10: in->held |= IN_GRENADE; break;
		case 11: in->held |= IN_RELOAD; break;
		case 12: in->held |= IN_SWITCH; break;
		case 13: in->held |= IN_ZOOM; break;
		case 14: in->move.move_y = 1.0f; break;
		default: break;
		}
		b->exercise_ticks++;
	}
	/* unreachable goal (a partial route, or the goal is at another height than we can climb to): after 4 s hop to the goal's floor (counted assist) */
	if (b->has_goal && !(enemy >= 0 && edist < 7.0f)) {
		float dz = fabsf(b->goal[2] - g->player.pos[2]);
		int unreachable = b->route_status != 0 || (dist2d(g->player.pos, b->goal) < 1.0f && dz > 0.9f);
		{                                                        /* a goal that does not get closer in 6 s counts as unreachable (closed doors, volumes off the mesh) */
			float d3 = dist2d(g->player.pos, b->goal);
			if (m->tick - b->progress_tick >= 180) {
				if (b->progress_dist > 0.0f && d3 > 1.5f && d3 > b->progress_dist - 0.5f) b->unreach_ticks += 120;
				b->progress_tick = m->tick; b->progress_dist = d3;
			}
		}
		if (unreachable) b->unreach_ticks++; else if (b->unreach_ticks < 120) b->unreach_ticks = b->unreach_ticks;
		if (b->unreach_ticks > 120) {
			float snap[3], hop[3];
			int32_t sec = path ? path_nearest_sector(path, b->goal, 6.0f, NULL, snap) : -1;
			if (sec >= 0 && dist2d(snap, g->player.pos) < 1.0f && dist2d(b->goal, g->player.pos) > 1.5f) sec = -1;   /* the mesh ends here: the goal is off it, go straight there */
			if (sec >= 0) { hop[0] = snap[0]; hop[1] = snap[1]; hop[2] = snap[2] + 0.05f; }
			else { hop[0] = b->goal[0]; hop[1] = b->goal[1]; hop[2] = b->goal[2]; }
			bot_report(b, m, "assist unreachable", g->player.pos, hop);
			if (b->goal_kind == 1) {                                 /* a volume that two hops in a row did not satisfy (needs something else first): try the other awaited ones for a while */
				if (b->hop_goal == b->goal_id && b->hop_goal_n) b->hop_goal_n++; else { b->hop_goal = b->goal_id; b->hop_goal_n = 1; }
				if (b->hop_goal_n >= 3) {
					ignore_volume(b, m, (uint16_t)b->goal_id, m->tick + 1800);
					b->hop_goal_n = 0; b->has_goal = 0;
					if (m->log) printf("[bot] volume %u was not satisfied by 3 hops: set aside for 60 s\n", (unsigned)b->goal_id);
				}
			}
			game_teleport(g, hop, g->player.yaw);
			b->hop_tick = m->tick;
			if (!floor_below(g, hop, 80.0f)) { memcpy(b->pin_pos, hop, sizeof hop); b->pin_until = m->tick + BOT_PIN_TICKS; }
			b->assists++; b->unreach_ticks = 0; b->replan_at = 0; b->wp_n = 0; b->progress_dist = 0.0f;
		}
	} else b->unreach_ticks = 0;
	if (moving) b->ticks_moving++; else b->ticks_idle++;

	/* stuck handling: no progress for 2 s: jump; 5 s: replan; 8 s: assisted hop along the route */
	{
		float moved = dist2d(g->player.pos, b->last_pos);
		if (!moving || moved > 0.012f * 5.0f) { b->stuck_ticks = 0; }
		else b->stuck_ticks++;
		if ((m->tick % 5) == 0) { b->last_pos[0] = g->player.pos[0]; b->last_pos[1] = g->player.pos[1]; b->last_pos[2] = g->player.pos[2]; }
		if (b->stuck_ticks > 20 && (m->tick & 15) < 3) in->move.jump = 1, in->held |= IN_JUMP;
		if (b->stuck_ticks > 150) { b->replan_at = 0; }
		if (b->stuck_ticks > 240 && b->wp_n > 0) {
			float hop[3];
			unsigned k = (unsigned)(b->wp_i + 1 < b->wp_n ? b->wp_i + 1 : b->wp_n - 1);
			hop[0] = b->wp[k].pos[0]; hop[1] = b->wp[k].pos[1]; hop[2] = b->wp[k].pos[2] + 0.05f;
			bot_report(b, m, "assist stuck", g->player.pos, hop);
			game_teleport(g, hop, g->player.yaw);
			b->hop_tick = m->tick;
			if (!floor_below(g, hop, 80.0f)) { memcpy(b->pin_pos, hop, sizeof hop); b->pin_until = m->tick + BOT_PIN_TICKS; }
			b->assists++; b->stuck_ticks = 0; b->replan_at = 0;
		}
	}
	/* IN_JUMP is a held button for the pad mapping: the move command already carries it */
	if (in->held & IN_JUMP) in->move.jump = 1;
	in->pressed = 0;
}
