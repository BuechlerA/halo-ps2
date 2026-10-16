/* Host tests for core/config. */
#include "core/config.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

int main(void)
{
	static const char text[] =
		"# comment\n"
		"pack   assets/hp2/a10.hp2  \r\n"
		"frames 600\n"
		"\n"
		"camera 1.5 -2 3e1 0.25\n"
		"fbdump 30\n"
		"fbdump 0x40\n"
		"flag\n"
		"   indented value with spaces\n";
	cfg_t c;
	float f[5];
	int bad = cfg_parse(&c, text, sizeof text - 1);
	CHECK(bad == 0);
	CHECK(c.count == 7);
	CHECK(strcmp(cfg_str(&c, "pack", ""), "assets/hp2/a10.hp2") == 0);
	CHECK(cfg_int(&c, "frames", 1) == 600);
	CHECK(cfg_int(&c, "missing", 7) == 7);
	CHECK(cfg_int(&c, "pack", 9) == 9);                 /* not a number */
	CHECK(cfg_int(&c, "fbdump", 0) == 30);
	CHECK(strcmp(cfg_nth(&c, "fbdump", 1), "0x40") == 0 && cfg_int(&c, "fbdump", 0) == 30);
	CHECK(cfg_nth(&c, "fbdump", 2) == NULL);
	CHECK(cfg_floats(&c, "camera", f, 5) == 4);
	CHECK(fabsf(f[0] - 1.5f) < 1e-6f && fabsf(f[1] + 2.0f) < 1e-6f && fabsf(f[2] - 30.0f) < 1e-6f);
	CHECK(cfg_floats(&c, "camera", f, 2) == 2);
	CHECK(cfg_floats(&c, "pack", f, 5) == 0);
	CHECK(strcmp(cfg_str(&c, "flag", "d"), "d") == 0);   /* empty value falls back */
	CHECK(strcmp(cfg_str(&c, "indented", ""), "value with spaces") == 0);

	/* Limits: overlong key / value and a full table are counted, not stored. */
	{
		char big[CFG_MAX_ENTRIES * 8 + 600];
		size_t n = 0;
		int i;
		memset(big, 'k', 40); n = 40;
		n += (size_t)snprintf(big + n, 8, " v\n");
		memcpy(big + n, "k ", 2); n += 2;
		memset(big + n, 'v', CFG_ARG_LEN + 5); n += CFG_ARG_LEN + 5;
		big[n++] = '\n';
		for (i = 0; i < CFG_MAX_ENTRIES + 3; i++) n += (size_t)snprintf(big + n, 16, "a%d x\n", i % 10);
		bad = cfg_parse(&c, big, n);
		CHECK(c.count == CFG_MAX_ENTRIES);
		CHECK(bad == 2 + 3);
	}
	CHECK(cfg_parse(&c, "", 0) == 0 && c.count == 0);
	CHECK(cfg_parse(&c, "novalue", 7) == 0 && c.count == 1 && cfg_str(&c, "novalue", "z")[0] == 'z');

	if (failures) { printf("%d failure(s)\n", failures); return 1; }
	printf("ok\n");
	return 0;
}
