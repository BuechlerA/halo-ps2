#include "sim/input.h"
#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

int main(void)
{
	game_input_t g;
	raw_input_t r;
	input_script s;
	cfg_t c;
	const char *text = "input 0 0 0 0 0 0\ninput 1.5 0 1 0.5 0 0x0200\ninput 1.0 9 9 9 9 0\ninput 3 0 0 0 0 0x4000\nfoo bar\ninput x\n";
	memset(&r, 0, sizeof r);
	r.buttons = 0x4000u | 0x0200u; r.ly = 1.0f;
	input_map(&g, &r, 0);
	CHECK((g.held & IN_JUMP) && (g.held & IN_FIRE) && g.pressed == g.held && g.move.jump && g.move.move_y == 1.0f);
	input_map(&g, &r, g.held);
	CHECK(g.pressed == 0);
	r.buttons = 0x0001u;
	input_map(&g, &r, 0);
	CHECK(g.held == IN_OVERLAY);
	cfg_parse(&c, text, strlen(text));
	CHECK(input_script_parse(&s, &c) == 3);              /* the out-of-order and malformed lines are dropped */
	input_script_eval(&s, -1.0f, &r);
	CHECK(r.buttons == 0 && r.ly == 0.0f);
	input_script_eval(&s, 1.6f, &r);
	CHECK(r.buttons == 0x0200u && r.ly == 1.0f && r.rx == 0.5f);
	input_script_eval(&s, 100.0f, &r);
	CHECK(r.buttons == 0x4000u);
	if (failures) { printf("%d failure(s)\n", failures); return 1; }
	printf("ok\n");
	return 0;
}
