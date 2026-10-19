#include "sim/vitals.h"
#include <math.h>
#include <stdio.h>

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)
#define NEAR(a, b, e) (fabsf((a) - (b)) <= (e))

int main(void)
{
	vitals_t v;
	int i;
	vitals_init(&v, 75.0f, 75.0f, 1.0f / 120.0f, 90);
	CHECK(NEAR(vitals_damage(&v, 30.0f, 1.0f, 1.0f, 0.0f), 30.0f, 1e-4f));
	CHECK(NEAR(v.shield, 45.0f, 1e-4f) && NEAR(v.health, 75.0f, 1e-4f));
	/* recharge waits for the stun, then refills in 120 ticks */
	for (i = 0; i < 89; i++) vitals_tick(&v);
	CHECK(NEAR(v.shield, 45.0f, 1e-4f));
	for (i = 0; i < 40; i++) vitals_tick(&v);
	CHECK(v.shield > 45.0f && v.shield < 75.0f);
	for (i = 0; i < 200; i++) vitals_tick(&v);
	CHECK(NEAR(v.shield, 75.0f, 1e-3f) && !(v.flags & VITALS_F_SHIELD_DOWN));
	/* a hit larger than the shield spills into the body */
	vitals_damage(&v, 100.0f, 1.0f, 1.0f, 0.0f);
	CHECK(v.shield == 0.0f && NEAR(v.health, 50.0f, 1e-3f) && (v.flags & VITALS_F_SHIELD_DOWN));
	/* body multipliers apply to the part that reaches the body */
	vitals_init(&v, 75.0f, 75.0f, 0.01f, 30);
	vitals_damage(&v, 150.0f, 1.0f, 0.5f, 0.0f);          /* 75 absorbed, half of the rest -> 37.5 */
	CHECK(NEAR(v.health, 75.0f - 37.5f, 1e-3f));
	/* unshielded units */
	vitals_init(&v, 10.0f, 0.0f, 0.0f, 0);
	vitals_damage(&v, 4.0f, 1.0f, 2.0f, 0.0f);
	CHECK(NEAR(v.health, 2.0f, 1e-4f) && !vitals_dead(&v));
	vitals_damage(&v, 1.0f, 1.0f, 2.0f, 0.0f);
	CHECK(vitals_dead(&v) && v.health == 0.0f);
	CHECK(vitals_damage(&v, 5.0f, 1.0f, 1.0f, 0.0f) == 0.0f);
	/* overshield absorbs first */
	vitals_init(&v, 75.0f, 75.0f, 0.01f, 30);
	vitals_add_overshield(&v, 75.0f);
	vitals_damage(&v, 100.0f, 1.0f, 1.0f, 0.0f);
	CHECK(NEAR(v.overshield, 0.0f, 1e-3f) && NEAR(v.shield, 50.0f, 1e-3f));
	/* leak */
	vitals_init(&v, 75.0f, 75.0f, 0.01f, 30);
	vitals_damage(&v, 20.0f, 1.0f, 1.0f, 0.5f);
	CHECK(NEAR(v.health, 65.0f, 1e-3f) && NEAR(v.shield, 55.0f, 1e-3f));
	vitals_heal(&v, 100.0f);
	CHECK(v.health == 75.0f);
	if (failures) { printf("%d failure(s)\n", failures); return 1; }
	printf("ok\n");
	return 0;
}
