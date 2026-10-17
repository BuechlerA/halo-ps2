#include "sim/vitals.h"
#include <string.h>

void vitals_init(vitals_t *v, float health, float shield, float recharge_per_tick, uint16_t stun_ticks)
{
	memset(v, 0, sizeof *v);
	v->health = v->max_health = health;
	v->shield = v->max_shield = shield;
	v->recharge = recharge_per_tick;
	v->stun_ticks = stun_ticks;
}

int vitals_dead(const vitals_t *v) { return (v->flags & VITALS_F_DEAD) != 0; }

float vitals_damage(vitals_t *v, float amount, float shield_mult, float body_mult, float shield_leak)
{
	float dealt = 0.0f, as = amount * shield_mult, ab = amount * body_mult, pool = v->shield + v->overshield;
	v->last_damage = 0.0f;
	if (amount <= 0.0f || vitals_dead(v)) return 0.0f;
	if (v->max_shield > 0.0f && pool > 0.0f && as > 0.0f) {
		float absorbed = as < pool ? as : pool, left = absorbed, share = absorbed / as;
		if (v->overshield > 0.0f) { float o = left < v->overshield ? left : v->overshield; v->overshield -= o; left -= o; }
		v->shield -= left;
		if (v->shield < 0.0f) v->shield = 0.0f;
		dealt += absorbed;
		v->stun = v->stun_ticks;
		if (share < 1.0f) {                       /* the shield broke: the rest of the hit reaches the body */
			float bd = ab * (1.0f - share);
			v->health -= bd; dealt += bd;
			v->flags |= VITALS_F_SHIELD_DOWN;
		}
		if (shield_leak > 0.0f) { float lk = ab * share * shield_leak; v->health -= lk; dealt += lk; }
	} else {
		v->health -= ab; dealt += ab;
		v->stun = v->stun_ticks;
	}
	if (v->health <= 0.0f) { v->health = 0.0f; v->flags |= VITALS_F_DEAD; }
	v->last_damage = dealt;
	return dealt;
}

void vitals_tick(vitals_t *v)
{
	if (vitals_dead(v)) return;
	if (v->stun) { v->stun--; }
	else if (v->shield < v->max_shield) {
		v->shield += v->recharge * v->max_shield;
		if (v->shield >= v->max_shield) { v->shield = v->max_shield; v->flags &= ~VITALS_F_SHIELD_DOWN; }
	}
	if (v->overshield > 0.0f) { v->overshield -= v->max_shield * 0.0005f; if (v->overshield < 0.0f) v->overshield = 0.0f; }
}

void vitals_heal(vitals_t *v, float amount)
{
	if (vitals_dead(v)) return;
	v->health += amount;
	if (v->health > v->max_health) v->health = v->max_health;
}

void vitals_add_overshield(vitals_t *v, float amount)
{
	v->overshield += amount;
	if (v->overshield > v->max_shield * 2.0f) v->overshield = v->max_shield * 2.0f;
}
