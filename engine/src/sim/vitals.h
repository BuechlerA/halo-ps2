/* Health and shield of a unit (biped/vehicle). Damage empties the shield first; what is left hurts the body. A hit that touches the shield
 * delays its recharge (the shield "stun" time), then it refills at `recharge` of its maximum per tick. Pure C11, host-tested.
 * Amounts use the tag unit (the biped's health/shield values, e.g. 75/75 for the player); modifiers come from the collision model. */
#ifndef SIM_VITALS_H
#define SIM_VITALS_H
#include <stdint.h>

#define VITALS_F_DEAD 1u
#define VITALS_F_SHIELD_DOWN 2u     /* the shield was emptied by the last hit (cleared when it recharges) */

typedef struct {
	float    health, shield;
	float    max_health, max_shield;
	float    recharge;             /* fraction of max_shield per tick while recharging */
	uint16_t stun_ticks;           /* recharge delay after the shield was hit */
	uint16_t stun;                 /* ticks left of the delay */
	uint32_t flags;
	float    overshield;           /* extra shield above max (powerup), decays at 1/ (some) ticks */
	float    last_damage;          /* damage applied to health + shield by the latest hit, for HUD flashes */
} vitals_t;

void  vitals_init(vitals_t *v, float health, float shield, float recharge_per_tick, uint16_t stun_ticks);
/* Applies a hit. `shield_mult` / `body_mult` are the collision region's multipliers (1 when unknown), `shield_leak`
 * (0..1) is the fraction of the shield-absorbed damage that still reaches the body. Returns the damage dealt in total. */
float vitals_damage(vitals_t *v, float amount, float shield_mult, float body_mult, float shield_leak);
void  vitals_tick(vitals_t *v);
void  vitals_heal(vitals_t *v, float amount);
void  vitals_add_overshield(vitals_t *v, float amount);
int   vitals_dead(const vitals_t *v);

#endif
