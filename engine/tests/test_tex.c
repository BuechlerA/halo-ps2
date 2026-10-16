/* Host test: open the TEXS and LM0 sections of the pack written by tools/tests/test_texconv.py
 * (TEX_FIXTURE) and check them against the values that test baked; then corrupt every byte of every
 * table and truncate the sections everywhere: whatever tex_open / lm_open accept must be fully
 * readable through the accessors (ASan / UBSan watch for reads outside the buffer). */
#include "stream/hp2.h"
#include "world/tex.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); return 1; } } while (0)

static uint32_t clut_swap(uint32_t i) { return (i & ~0x18u) | ((i & 8u) << 1) | ((i & 0x10u) >> 1); }

/* Read every byte range the accessors can hand out; returns a checksum so nothing is optimised away. */
static unsigned touch_tex(const tex_section *s)
{
	unsigned sum = 0;
	for (uint32_t i = 0; i < s->texture_count; i++) {
		tex_info t = tex_get(s, i);
		const uint8_t *clut = tex_clut(s, &t);
		sum += clut[0] + clut[t.clut_entries * 4u - 1u];
		for (uint32_t m = 0; m < t.mip_count; m++) {
			tex_mip_view v;
			if (!tex_mip(s, &t, m, &v)) continue;
			sum += v.pixels[0] + v.pixels[v.bytes - 1u] + v.width + v.height;
		}
	}
	for (uint32_t i = 0; i < s->binding_count; i++) {
		tex_binding b = tex_get_binding(s, i), f;
		if (!tex_find_binding(s, b.shader_id, &f) || f.shader_id != b.shader_id) sum += 1000000u;
		if (b.texture != TEX_NONE) sum += tex_get(s, b.texture).width;
	}
	return sum;
}

static unsigned touch_lm(const lm_section *s)
{
	unsigned sum = s->clut[0] + s->clut[LM_CLUT_BYTES - 1u];
	for (uint32_t i = 0; i <= s->page_count; i++) {
		lm_page_info p;
		if (!lm_get_page(s, i, &p)) continue;
		sum += p.pixels[0] + p.pixels[p.pixel_bytes - 1u];
	}
	return sum;
}

static int expect_texel_colour(const tex_section *s, const tex_info *t, uint32_t x, uint32_t y,
                               unsigned r, unsigned g, unsigned b, unsigned a)
{
	tex_mip_view v;
	CHECK(tex_mip(s, t, 0, &v));
	uint32_t idx = t->psm == TEX_PSMT4 ? (v.pixels[(y * v.width + x) / 2] >> (((y * v.width + x) & 1u) * 4u)) & 15u
	                                    : v.pixels[y * v.width + x];
	const uint8_t *clut = tex_clut(s, t);
	/* the GS reads PSMT8 index i from CLUT storage slot swap(i) (CSM1); PSMT4 CLUTs are stored straight */
	uint32_t slot = t->psm == TEX_PSMT8 ? clut_swap(idx) : idx;
	const uint8_t *e = clut + slot * 4u;
	CHECK(abs((int)e[0] - (int)r) <= 8 && abs((int)e[1] - (int)g) <= 8 && abs((int)e[2] - (int)b) <= 8);
	CHECK(e[3] == a);
	return 0;
}

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
	const hp2_section *ts = hp2_find(&pk, "TEXS"), *ls = hp2_find(&pk, "LM0");
	CHECK(ts && ls);
	uint8_t *raw = malloc(ts->raw_size), *lraw = malloc(ls->raw_size);
	CHECK(raw && lraw);
	CHECK(hp2_load(&pk, ts, raw, ts->raw_size) == HP2_OK && hp2_load(&pk, ls, lraw, ls->raw_size) == HP2_OK);
	size_t size = ts->raw_size, lsize = ls->raw_size;

	tex_section t;
	CHECK(tex_open(&t, raw, size) == TEX_OK);
	CHECK(t.texture_count == 3 && t.binding_count == 4);
	tex_info t0 = tex_get(&t, 0), t1 = tex_get(&t, 1), t2 = tex_get(&t, 2);
	CHECK(t0.psm == TEX_PSMT8 && t0.width == 16 && t0.height == 16 && t0.mip_count == 2 && t0.clut_entries == 256);
	CHECK(t0.flags == 0 && t0.source_id == 0xE1780001u && t0.pixel_bytes == 256 + 64);
	CHECK(t1.psm == TEX_PSMT4 && t1.width == 32 && t1.mip_count == 3 && t1.clut_entries == 16 && t1.flags == TEX_F_PUNCH);
	CHECK(t1.pixel_bytes == 512 + 128 + 32);
	CHECK(t2.psm == TEX_PSMT8 && t2.width == 8 && t2.mip_count == 1 && t2.flags == TEX_F_BLEND);
	CHECK(((uintptr_t)tex_clut(&t, &t0) - (uintptr_t)raw) % 16 == 0);
	CHECK(tex_max_mips(256, 256) == 4 && tex_max_mips(64, 64) == 4 && tex_max_mips(16, 16) == 2 &&
	      tex_max_mips(128, 8) == 1 && tex_max_mips(32, 16) == 2 && tex_max_mips(8, 8) == 1);

	tex_mip_view m;
	CHECK(tex_mip(&t, &t0, 1, &m) && m.width == 8 && m.height == 8 && m.bytes == 64);
	CHECK(m.pixels == raw + t0.pixel_offset + 256);
	CHECK(tex_mip(&t, &t1, 2, &m) && m.width == 8 && m.height == 8 && m.bytes == 32);
	CHECK(!tex_mip(&t, &t0, 2, &m) && m.pixels == NULL && !tex_mip(&t, &t2, 1, &m));

	/* texture 0 holds 256 distinct colours: texel i = (r5 = i & 31, g6 = i >> 5, b = 0), alpha 0x80 */
	for (uint32_t i = 0; i < 256; i++) {
		unsigned r5 = i & 31u, g6 = i >> 5;
		if (expect_texel_colour(&t, &t0, i % 16, i / 16, r5 << 3 | r5 >> 2, g6 << 2 | g6 >> 4, 0, 0x80)) return 1;
	}
	/* texture 1 (punch, 4 bit): left half transparent (entry 0), right half opaque red */
	{
		tex_mip_view v;
		CHECK(tex_mip(&t, &t1, 0, &v));
		CHECK((v.pixels[0] & 15u) == 0 && tex_clut(&t, &t1)[3] == 0);     /* texel (0,0): entry 0, alpha 0 */
		uint32_t idx = (v.pixels[(5u * 32u + 31u) / 2u] >> 4) & 15u;      /* texel (31, 5): odd index, high nibble */
		const uint8_t *e = tex_clut(&t, &t1) + idx * 4u;
		CHECK(e[3] == 0x80 && abs((int)e[0] - 200) <= 8 && e[1] < 60 && e[2] < 60);
	}

	tex_binding b;
	CHECK(tex_find_binding(&t, 0xE1740002u, &b) && b.texture == TEX_NONE && (b.flags & TEX_B_UNTEXTURED));
	CHECK(b.flat_rgba == 0x80808080u && b.group == TEX_GROUP_UNKNOWN && b.blend == TEX_BLEND_OPAQUE);
	CHECK(tex_find_binding(&t, 0xE1740010u, &b) && b.texture == 1 && b.flags == (TEX_B_ALPHA_TEST | TEX_B_TWO_SIDED));
	CHECK(b.blend == TEX_BLEND_ALPHA_TEST && b.group == TEX_GROUP_SENV && b.uv_scale[0] == 1.0f && b.uv_scale[1] == 1.0f);
	CHECK(tex_find_binding(&t, 0xE1740020u, &b) && b.texture == 2 && b.blend == TEX_BLEND_ALPHA && b.group == TEX_GROUP_SGLA);
	CHECK(b.flat_rgba == t2.avg_rgba);
	CHECK(tex_find_binding(&t, 0xE1740030u, &b) && b.texture == 0 && b.blend == TEX_BLEND_ADD && b.group == TEX_GROUP_SOTR);
	CHECK(fabsf(b.uv_scale[0] - 8.0f) < 1e-6f && fabsf(b.uv_scale[1] - 4.0f) < 1e-6f && b.flat_rgba == t0.avg_rgba);
	CHECK(!tex_find_binding(&t, 0, &b) && !tex_find_binding(&t, 0xE1740001u, &b) && !tex_find_binding(&t, 0xE1740021u, &b) &&
	      !tex_find_binding(&t, 0xFFFFFFFFu, &b));
	tex_binding last = tex_get_binding(&t, 3), first = tex_get_binding(&t, 0);
	CHECK(first.shader_id == 0xE1740002u && last.shader_id == 0xE1740030u);
	CHECK(touch_tex(&t) > 0);

	/* lightmap section */
	lm_section l;
	CHECK(lm_open(&l, lraw, lsize) == TEX_OK);
	CHECK(l.page_count == 3 && l.used_count == 2 && l.gain == 1.0f);
	lm_page_info p;
	CHECK(lm_get_page(&l, 0, &p) && p.width == 32 && p.height == 16 && p.src_width == 64 && p.src_height == 32);
	CHECK(p.psm == TEX_PSMT8 && p.pixel_bytes == 512 && p.pixels == lraw + p.pixel_offset && p.pixel_offset % 16 == 0);
	CHECK(p.pixels[0] <= 4 && p.pixels[31] >= 240);                       /* horizontal ramp, left dark right bright */
	CHECK(!lm_get_page(&l, 1, &p) && p.pixels == NULL && !lm_get_page(&l, 3, &p) && !lm_get_page(&l, 0xFFFFu, &p));
	CHECK(lm_get_page(&l, 2, &p) && p.width == 8 && p.height == 8 && p.avg_luma == 90 && p.pixels[63] == 90);
	CHECK(l.clut[16 * 4] == 8 && l.clut[8 * 4] == 16 && l.clut[255 * 4 + 3] == 255);   /* grey ramp, CSM1 order */
	CHECK(touch_lm(&l) > 0);

	/* truncation anywhere is rejected */
	for (size_t cut = 0; cut < size; cut++) CHECK(tex_open(&t, raw, cut) != TEX_OK);
	for (size_t cut = 0; cut < lsize; cut++) CHECK(lm_open(&l, lraw, cut) != TEX_OK);

	/* corrupt every byte of the header and both tables (and of the page table): the verdict may be
	 * either way, but anything accepted must be readable end to end */
	static const uint8_t patterns[] = { 0x01, 0x10, 0x80, 0xFF };
	uint8_t *copy = malloc(size > lsize ? size : lsize);
	CHECK(copy);
	unsigned sink = 0, accepted = 0, rejected = 0;
	size_t tex_tables = 64u + 3u * 32u + 4u * 32u;
	for (size_t at = 0; at < tex_tables; at++) {
		for (size_t k = 0; k < sizeof patterns; k++) {
			memcpy(copy, raw, size);
			copy[at] ^= patterns[k];
			tex_section bad;
			if (tex_open(&bad, copy, size) == TEX_OK) { sink += touch_tex(&bad); accepted++; } else rejected++;
		}
	}
	size_t lm_tables = 64u + 3u * 32u;
	for (size_t at = 0; at < lm_tables; at++) {
		for (size_t k = 0; k < sizeof patterns; k++) {
			memcpy(copy, lraw, lsize);
			copy[at] ^= patterns[k];
			lm_section bad;
			if (lm_open(&bad, copy, lsize) == TEX_OK) { sink += touch_lm(&bad); accepted++; } else rejected++;
		}
	}
	CHECK(rejected > 400 && accepted > 0);            /* flags, averages, uv scales etc. are free-form */
	/* specific hostile edits */
	memcpy(copy, raw, size);
	copy[64 + 8] = 0x12;                              /* texture 0 psm */
	CHECK(tex_open(&t, copy, size) == TEX_ERR_LAYOUT);
	memcpy(copy, raw, size);
	copy[64 + 9] = 3;                                 /* 16x16 allows two mips only */
	CHECK(tex_open(&t, copy, size) == TEX_ERR_LAYOUT);
	memcpy(copy, raw, size);
	copy[64 + 16 + 3] = 0x7F;                         /* pixel offset far outside */
	CHECK(tex_open(&t, copy, size) != TEX_OK);
	memcpy(copy, raw, size);
	copy[64 + 3 * 32 + 32 + 4] = 0x30;                /* binding 1 -> texture 0x30 (out of range) */
	CHECK(tex_open(&t, copy, size) == TEX_ERR_LAYOUT);
	memcpy(copy, raw, size);
	memcpy(copy + 64 + 3 * 32 + 32, raw + 64 + 3 * 32, 4);   /* binding 1 id == binding 0 id: not strictly sorted */
	CHECK(tex_open(&t, copy, size) == TEX_ERR_LAYOUT);
	(void)sink;
	free(copy); free(lraw); free(raw); free(img);
	puts("ok");
	return 0;
}
