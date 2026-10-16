/* Host test: open the BSP0 section of a pack written by tools/tests/test_bspconv.py and check it
 * against the values that test baked (40 triangles in 2 batches, one material). */
#include "stream/hp2.h"
#include "world/bsp.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); return 1; } } while (0)

int main(int argc, char **argv)
{
	CHECK(argc == 2);
	FILE *f = fopen(argv[1], "rb");
	CHECK(f);
	fseek(f, 0, SEEK_END);
	size_t n = (size_t)ftell(f);
	rewind(f);
	uint8_t *img = malloc(n);
	CHECK(img && fread(img, 1, n, f) == n);
	fclose(f);

	hp2_pack pk;
	CHECK(hp2_open(&pk, img, n) == HP2_OK);
	const hp2_section *sec = hp2_find(&pk, "BSP0");
	CHECK(sec);
	uint8_t *raw = malloc(sec->raw_size);
	CHECK(hp2_load(&pk, sec, raw, sec->raw_size) == HP2_OK);

	bsp_section b;
	CHECK(bsp_open(&b, raw, sec->raw_size) == BSP_OK);
	CHECK(b.material_count == 1 && b.batch_count == 2 && b.triangle_count == 40);
	CHECK(fabsf(b.bounds[0] + 11.0f) < 1e-6f && fabsf(b.bounds[4] - 117.5f) < 1e-6f);
	bsp_material m = bsp_get_material(&b, 0);
	CHECK(m.shader_id == 0xE1740042u && m.lightmap_page == 3 && m.batch_count == 2 && m.triangles == 40);
	bsp_batch b0 = bsp_get_batch(&b, 0), b1 = bsp_get_batch(&b, 1);
	CHECK(b0.triangles == 32 && b1.triangles == 8 && b0.radius > 0.0f);
	/* first word of a batch packet: UNPACK V4-16 (cmd 0x6D), TOPS-relative, num = 3 x triangles, addr 4 */
	const uint8_t *p = bsp_batch_vif(&b, &b0);
	uint32_t w = (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
	CHECK((w >> 24) == 0x6D && ((w >> 16) & 0xFF) == 96 && (w & 0xFFFF) == (0x8000u | 4u));

	/* every truncation or field corruption of the header is rejected, never read past the end */
	bsp_section bad;
	for (size_t cut = 0; cut < 64; cut++) CHECK(bsp_open(&bad, raw, cut) != BSP_OK);
	CHECK(bsp_open(&bad, raw, sec->raw_size - 16) != BSP_OK);
	uint8_t *copy = malloc(sec->raw_size);
	for (size_t at = 4; at < 64; at++) {
		memcpy(copy, raw, sec->raw_size);
		copy[at] ^= 0x40;
		(void)bsp_open(&bad, copy, sec->raw_size);   /* must not crash (ASan) whatever the verdict */
	}
	memcpy(copy, raw, sec->raw_size);
	copy[64 + 32 + 2] = 33;                          /* first batch: 33 triangles > limit */
	CHECK(bsp_open(&bad, copy, sec->raw_size) == BSP_ERR_LAYOUT);
	puts("ok");
	return 0;
}
