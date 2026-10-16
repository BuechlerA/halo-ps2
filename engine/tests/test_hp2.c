/* Host test: decode a pack written by tools/halo2ps2/hp2.py (path in argv[1]) with the
 * engine's reader and compare against the expected section contents. Run via `make -C engine test`. */
#include "stream/hp2.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); return 1; } } while (0)

static uint8_t *slurp(const char *path, size_t *n)
{
	FILE *f = fopen(path, "rb");
	if (!f) return NULL;
	fseek(f, 0, SEEK_END);
	*n = (size_t)ftell(f);
	rewind(f);
	uint8_t *b = malloc(*n);
	if (b && fread(b, 1, *n, f) != *n) { free(b); b = NULL; }
	fclose(f);
	return b;
}

int main(int argc, char **argv)
{
	size_t n;
	CHECK(argc == 2);
	uint8_t *img = slurp(argv[1], &n);
	CHECK(img);

	hp2_pack pk;
	CHECK(hp2_open(&pk, img, n) == HP2_OK);
	CHECK(strcmp(pk.name, "test") == 0 && pk.count == 4);

	/* RES: "halo-ps2 " x700, then 300 pseudo-random bytes, then a 16-byte tail (LZ4-compressed) */
	const hp2_section *res = hp2_find(&pk, "RES");
	CHECK(res && res->codec == HP2_CODEC_LZ4 && res->raw_size == 9 * 700 + 300 + 16);
	uint8_t *buf = malloc(res->raw_size);
	CHECK(hp2_load(&pk, res, buf, res->raw_size) == HP2_OK);
	CHECK(memcmp(buf, "halo-ps2 halo-ps2 ", 18) == 0);
	CHECK(memcmp(buf + res->raw_size - 16, "tail-bytes-here!", 16) == 0);
	CHECK(hp2_load(&pk, res, buf, res->raw_size - 1) == HP2_ERR_SPACE);

	const hp2_section *bsp = hp2_find(&pk, "BSP0");
	CHECK(bsp && bsp->raw_size == 5000);
	uint8_t *z = malloc(5000);
	CHECK(hp2_load(&pk, bsp, z, 5000) == HP2_OK);
	for (int i = 0; i < 5000; i++) CHECK(z[i] == 0);

	const hp2_section *tex = hp2_find(&pk, "TEX");
	CHECK(tex && tex->codec == HP2_CODEC_STORED && tex->raw_size == 777);
	uint8_t *t = malloc(777);
	CHECK(hp2_load(&pk, tex, t, 777) == HP2_OK);

	const hp2_section *snd = hp2_find(&pk, "SND");
	CHECK(snd && snd->raw_size == 0);
	CHECK(hp2_load(&pk, snd, t, 0) == HP2_OK);
	CHECK(hp2_find(&pk, "NOPE") == NULL);

	/* corruption: flipped payload byte must surface as a decode or CRC error, never a crash */
	uint8_t *bad = malloc(n);
	for (size_t at = 0; at < res->stored_size; at += 37) {
		memcpy(bad, img, n);
		bad[res->offset + at] ^= 0x5A;
		hp2_pack pb;
		CHECK(hp2_open(&pb, bad, n) == HP2_OK);
		CHECK(hp2_load(&pb, hp2_find(&pb, "RES"), buf, res->raw_size) != HP2_OK);
	}
	/* malformed streams are rejected, not overrun */
	uint8_t tiny[8];
	const uint8_t lit_overrun[] = {0xF0, 0xFF};
	const uint8_t zero_offset[] = {0x11, 'a', 0x00, 0x00};
	CHECK(lz4_decode(lit_overrun, sizeof lit_overrun, tiny, sizeof tiny) < 0);
	CHECK(lz4_decode(zero_offset, sizeof zero_offset, tiny, sizeof tiny) < 0);
	CHECK(hp2_open(&pk, img, 10) == HP2_ERR_TRUNCATED);
	CHECK(hp2_open(&pk, img + 1, n - 1) == HP2_ERR_MAGIC);
	/* header-only open + decode from a separately read payload (what the EE loader does) */
	{
		hp2_pack ph, pf;
		const hp2_section *r2;
		uint8_t *buf2;
		size_t hdr = 64 + 32 * 4;       /* the fixture has 4 sections */
		CHECK(hp2_open(&pf, img, n) == HP2_OK);
		res = hp2_find(&pf, "RES");                  /* pk was reset by the open calls above */
		buf2 = malloc(res->raw_size);
		CHECK(hp2_open_header(&ph, img, hdr, n) == HP2_OK && ph.count == 4);
		r2 = hp2_find(&ph, "RES");
		CHECK(r2 && r2->offset == res->offset);
		CHECK(hp2_load_stored(r2, img + r2->offset, buf2, res->raw_size) == HP2_OK);
		CHECK(memcmp(buf2, "halo-ps2 halo-ps2 ", 18) == 0 && memcmp(buf2 + res->raw_size - 16, "tail-bytes-here!", 16) == 0);
		CHECK(hp2_load_stored(r2, img + r2->offset, buf2, res->raw_size - 1) == HP2_ERR_SPACE);
		CHECK(hp2_open_header(&ph, img, hdr - 1, n) == HP2_ERR_TRUNCATED);   /* table cut short */
		CHECK(hp2_open_header(&ph, img, hdr, n + 1) == HP2_ERR_LAYOUT);        /* wrong file size */
		CHECK(hp2_open_header(&ph, img, hdr, n - 1) != HP2_OK);                /* a payload would lie outside */
		free(buf2);
	}
	free(bad); free(t); free(z); free(buf); free(img);
	puts("ok");
	return 0;
}
