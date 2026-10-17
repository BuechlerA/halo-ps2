/* Host test of the checkpoint image: pack / unpack round trip, damaged images, pointer relocation. Uses a synthetic game state (no assets). */
#include "sim/save.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static checkpoint_t a, b;
static game_t g, g2;
static object_table_t objs, objs2;
static fpview_t fp, fp2;

int main(void)
{
	uint8_t *img;
	size_t n, i;
	objects_clear(&objs); objects_clear(&objs2);
	memset(&g, 0, sizeof g); memset(&g2, 0, sizeof g2);
	g.objects = &objs; g.fp = &fp; g.tick = 777; g.st_shots = 12; g.player.pos[0] = 3.5f; g.vitals.health = 41.0f;
	objs.count = 5; objs.obj[2].pos[1] = 9.0f; objs.obj[2].model = 3; objs.by_placement[7] = 2;
	g.unit_state[2].dead = 1;
	fp.active = 1; fp.arms_frame = 4.0f;
	checkpoint_save(&a, &g, &fp, NULL);
	img = malloc(checkpoint_pack_max());
	n = checkpoint_pack(&a, img, checkpoint_pack_max());
	printf("  image %u bytes for a %u byte snapshot\n", (unsigned)n, (unsigned)sizeof a);
	CHECK(n > 0 && n < sizeof a / 3);
	/* unpack into a fresh buffer and revert into a different live game: pointers are the live ones, state is the saved one */
	memset(&b, 0, sizeof b);
	CHECK(checkpoint_unpack(&b, img, n));
	g2.objects = &objs2; g2.fp = &fp2;
	CHECK(checkpoint_revert(&b, &g2, &objs2, &fp2, NULL));
	CHECK(g2.tick == 777 && g2.st_shots == 12 && g2.player.pos[0] == 3.5f && g2.vitals.health == 41.0f && g2.unit_state[2].dead == 1);
	CHECK(g2.objects == &objs2 && g2.fp == &fp2);
	CHECK(objs2.count == 5 && objs2.obj[2].pos[1] == 9.0f && objs2.by_placement[7] == 2 && fp2.active && fp2.arms_frame == 4.0f);
	/* damaged images are rejected */
	{
		uint8_t *bad = malloc(n);
		memcpy(bad, img, n);
		CHECK(!checkpoint_unpack(&b, bad, n - 1));
		CHECK(!checkpoint_unpack(&b, bad, 10));
		for (i = 64; i < n; i += n / 97 + 1) { bad[i] ^= 0x5A; CHECK(!checkpoint_unpack(&b, bad, n)); bad[i] ^= 0x5A; }
		bad[0] ^= 1; CHECK(!checkpoint_unpack(&b, bad, n)); bad[0] ^= 1;
		CHECK(checkpoint_unpack(&b, bad, n));
		free(bad);
	}
	free(img);
	if (failures) { printf("%d failure(s)\n", failures); return 1; }
	printf("ok\n");
	return 0;
}
