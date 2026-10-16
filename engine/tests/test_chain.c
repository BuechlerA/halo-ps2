/* Host tests for render/chain and the render/gs.h encodings. */
#include "render/chain.h"
#include "render/gs.h"
#include "render/upload.h"
#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

int main(void)
{
	static uint64_t mem[64 * 2] __attribute__((aligned(16)));
	static const uint8_t data[64] __attribute__((aligned(16)));
	chain_t c;
	const uint64_t *t;
	uint64_t *d;

	chain_init(&c, mem, 64);
	chain_ref(&c, data, 5, VIF_PAIR(VIF_CODE(VIF_NOP, 0, 0), VIF_CODE(VIF_NOP, 0, 0)));
	chain_ref(&c, data + 16, 3, VIF_PAIR(VIF_CODE(VIF_MSCAL, 0, 140), 0));
	d = chain_cnt(&c, 2, 0);
	CHECK(d == mem + 6);                                   /* after two tags (4 u64) + this tag (2 u64) */
	chain_end(&c, VIF_PAIR(VIF_CODE(VIF_MSCAL, 0, 0), 0));
	CHECK(chain_qwords(&c) == 2 + 1 + 2 + 1 && !c.overflow);

	t = mem;
	CHECK((t[0] & 0xFFFF) == 5 && ((t[0] >> 28) & 7) == DMA_ID_REF && (uint32_t)(t[0] >> 32) == CHAIN_PHYS(data));
	CHECK(t[1] == 0);
	t = mem + 2;
	CHECK((t[0] & 0xFFFF) == 3 && ((t[0] >> 28) & 7) == DMA_ID_REF && (uint32_t)(t[0] >> 32) == CHAIN_PHYS(data + 16));
	CHECK((uint32_t)t[1] == 0x14000000u + 140);            /* MSCAL 140 in the low VIF word */
	t = mem + 4;
	CHECK((t[0] & 0xFFFF) == 2 && ((t[0] >> 28) & 7) == DMA_ID_CNT);
	t = mem + 10;
	CHECK((t[0] & 0xFFFF) == 0 && ((t[0] >> 28) & 7) == DMA_ID_END && (uint32_t)t[1] == 0x14000000u);

	/* Overflow is sticky and never writes past the buffer. */
	chain_init(&c, mem, 3);
	chain_ref(&c, data, 1, 0);
	CHECK(chain_cnt(&c, 4, 0) == NULL && c.overflow);
	chain_ref(&c, data, 1, 0);
	chain_ref(&c, data, 1, 0);
	CHECK(chain_qwords(&c) == 3);

	/* Encodings: spot-check against hand-computed field positions. */
	CHECK(GIF_TAG(7, 1, 0, 0, 0, 3) == (7ull | 1ull << 15 | 3ull << 60));
	CHECK(GS_PRIM(GSX_PRIM_TRIANGLE, 1, 1, 0, 1, 0, 0, 0, 0) == (3u | 1u << 3 | 1u << 4 | 1u << 6));
	CHECK(GS_TEX0(0x123, 4, GS_PSMT8, 7, 6, 1, 1, 0x40, GS_PSMCT32, 0, 0, 1) ==
	      (0x123ull | 4ull << 14 | 0x13ull << 20 | 7ull << 26 | 6ull << 30 | 1ull << 34 | 1ull << 35 |
	       0x40ull << 37 | 1ull << 61));
	CHECK(GS_ALPHA(1, 2, 0, 2, 0) == (1u | 2u << 2 | 0u << 4 | 2u << 6));
	CHECK(GS_TEST(0, 1, 0, 0, 0, 0, 1, 2) == (1u << 16 | 2u << 17 | 1u << 1));
	CHECK(GS_BITBLTBUF(0, 0, 0, 5, 4, GS_PSMT8) == (5ull << 32 | 4ull << 48 | 0x13ull << 56));
	CHECK(VIF_CODE(VIF_DIRECT, 0, 9) == 0x50000009u);
	CHECK(sizeof(gs_ad) == 16);

	/* Image upload: one cnt tag with 6 qw (4 A+D + IMAGE tag, GIF tag first) then a ref tag. */
	{
		uint64_t *u;
		chain_init(&c, mem, 64);
		CHECK(gs_upload(&c, data, 64, 0x123, 2, GS_PSMT8, 8, 8) == 0);
		CHECK(chain_qwords(&c) == 1 + 6 + 1);
		u = mem + 2;
		CHECK(u[0] == GIF_TAG(4, 0, 0, 0, 0, 1) && u[1] == GIF_REGS_AD);
		CHECK(u[2] == GS_BITBLTBUF(0, 0, 0, 0x123, 2, GS_PSMT8) && u[3] == 0x50);
		CHECK(u[6] == GS_TRXREG(8, 8) && u[7] == 0x52 && u[9] == 0x53);
		CHECK(u[10] == (4ull | 1ull << 15 | 2ull << 58));
		t = mem + 2 + 12;
		CHECK((t[0] & 0xFFFF) == 4 && ((t[0] >> 28) & 7) == DMA_ID_REF);
		CHECK(gs_upload(&c, data, 0, 0, 1, GS_PSMT8, 1, 1) == -1);     /* empty / unaligned rejected */
		CHECK(gs_upload(&c, data, 17, 0, 1, GS_PSMT8, 1, 1) == -1);
		chain_init(&c, mem, 4);
		CHECK(gs_upload(&c, data, 64, 0, 1, GS_PSMT8, 8, 8) == -1 && c.overflow);
	}

	if (failures) { printf("%d failure(s)\n", failures); return 1; }
	printf("ok\n");
	return 0;
}
