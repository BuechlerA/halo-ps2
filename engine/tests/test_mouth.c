/* Host test: the MOUT reader (world/mouth.c) on the blob tools/tests/test_mouthconv.py writes (MOUT_FIXTURE): lookups by (tag, variant), the
 * lengths and bytes, misses, and that every corrupted / truncated blob is either refused or completely safe to look up in (ASan / UBSan). */
#include "world/mouth.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); return 1; } } while (0)

static volatile unsigned sink;

static void poke(const mouth_section *s)
{
	static const uint32_t tags[] = { 0xE8F00001u, 0xE8F00002u, 0xE8F00003u, 0u, 0xFFFFFFFFu };
	unsigned acc = 0, i, v, len, k;
	for (i = 0; i < sizeof tags / sizeof tags[0]; i++)
		for (v = 0; v < 3; v++) {
			const uint8_t *p = mouth_find(s, tags[i], v, &len);
			if (p) for (k = 0; k < len; k++) acc += p[k];
		}
	sink = acc;
}

int main(int argc, char **argv)
{
	FILE *f;
	uint8_t *buf, *mut;
	long n;
	mouth_section s;
	unsigned len = 99, i;
	const uint8_t *p;
	if (argc < 2) { fprintf(stderr, "usage: test_mouth blob\n"); return 2; }
	f = fopen(argv[1], "rb");
	CHECK(f != NULL);
	fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
	buf = malloc((size_t)n); mut = malloc((size_t)n);
	CHECK(buf && mut && fread(buf, 1, (size_t)n, f) == (size_t)n);
	fclose(f);

	CHECK(mouth_open(&s, buf, (size_t)n) == 0 && s.count == 3);
	p = mouth_find(&s, 0xE8F00001u, 0, &len);
	CHECK(p && len == 50 && p[0] == 0 && p[1] == 5 && p[49] == 245);
	p = mouth_find(&s, 0xE8F00002u, 0, &len);
	CHECK(p && len == 1 && p[0] == 7);
	p = mouth_find(&s, 0xE8F00002u, 1, &len);
	CHECK(p && len == 3 && p[0] == 0 && p[1] == 5 && p[2] == 255);
	CHECK(mouth_find(&s, 0xE8F00002u, 2, &len) == NULL && len == 0);          /* unknown take */
	CHECK(mouth_find(&s, 0xE8F00003u, 0, &len) == NULL);                      /* unknown line */
	CHECK(mouth_find(&s, 0xE8F00001u, 0, NULL) != NULL);                      /* len is optional */
	CHECK(mouth_find(NULL, 1, 0, &len) == NULL);
	poke(&s);

	for (i = 0; i < (unsigned)n; i++) {                                      /* every byte flipped */
		memcpy(mut, buf, (size_t)n);
		mut[i] ^= 0xFFu;
		if (mouth_open(&s, mut, (size_t)n) == 0) poke(&s);
	}
	for (i = 0; i < (unsigned)n; i++) if (mouth_open(&s, buf, i) == 0) poke(&s);   /* every truncation */
	CHECK(mouth_open(&s, NULL, 0) != 0 && mouth_open(&s, buf, 8) != 0);
	puts("ok");
	free(buf); free(mut);
	return 0;
}
