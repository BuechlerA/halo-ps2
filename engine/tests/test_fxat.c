/* FXAT: object attachments of the effects runtime (sim/fx.c fx_set_attachments, fx_attachments_of, fx_attachment). */
#include "sim/fx.h"
#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static void row(uint8_t *p, uint32_t obj, uint32_t tag, uint8_t kind, const char *marker)
{
	memset(p, 0, 32);
	put32(p, obj); put32(p + 4, tag); p[8] = kind;
	strncpy((char *)p + 12, marker, 19);
}

int main(void)
{
	uint8_t blob[16 + 4 * 32];
	fx_t f;
	uint32_t first;
	fx_attach_row r;
	memset(blob, 0, sizeof blob);
	memcpy(blob, "FXAT", 4); put32(blob + 4, 2); put32(blob + 8, 4); put32(blob + 12, 0x1234u);
	row(blob + 16, 100, 7, FX_ATT_PCTL, "smoker");
	row(blob + 48, 200, 8, FX_ATT_EFFE, "");
	row(blob + 80, 200, 9, FX_ATT_PCTL, "smoke");
	row(blob + 112, 300, 10, FX_ATT_EFFE, "light");
	fx_init(&f, NULL, 3.0f, 1);
	CHECK(fx_set_attachments(&f, blob, sizeof blob) == 1 && f.attach_count == 4 && f.glow_tag == 0x1234u);
	CHECK(fx_attachments_of(&f, 100, &first) == 1 && first == 0);
	CHECK(fx_attachments_of(&f, 200, &first) == 2 && first == 1);
	CHECK(fx_attachments_of(&f, 300, &first) == 1 && first == 3);
	CHECK(fx_attachments_of(&f, 150, &first) == 0);
	CHECK(fx_attachments_of(&f, 999, &first) == 0);
	r = fx_attachment(&f, 2);
	CHECK(r.object_tag == 200 && r.tag == 9 && r.kind == FX_ATT_PCTL && strcmp(r.marker, "smoke") == 0);
	r = fx_attachment(&f, 99);
	CHECK(r.tag == 0 && r.kind == 0);
	CHECK(fx_set_attachments(&f, blob, 20) == 0 && f.attach_count == 0);             /* truncated */
	blob[4] = 1;
	CHECK(fx_set_attachments(&f, blob, sizeof blob) == 0);                            /* wrong version */
	blob[4] = 2; put32(blob + 8, 1000);
	CHECK(fx_set_attachments(&f, blob, sizeof blob) == 0);                            /* count beyond the data */
	CHECK(fx_set_attachments(&f, NULL, 0) == 0);
	{                                                                                  /* no definitions: an update is a harmless no-op */
		float p[3] = { 0, 0, 0 }, d[3] = { 0, 0, 1 };
		put32(blob + 8, 4);
		fx_set_attachments(&f, blob, sizeof blob);
		fx_attach_update(&f, 5, 0, p, d);
		fx_tick(&f);
		CHECK(fx_active_particles(&f) == 0);
	}
	if (failures) { printf("%d failure(s)\n", failures); return 1; }
	printf("ok\n");
	return 0;
}
