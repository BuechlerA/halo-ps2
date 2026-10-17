/* GS / GIF / VIF encodings used by the renderer. Pure C11 so packet layouts are unit-tested on
 * the host; no ps2sdk includes (the sdk macros are not used on purpose: these helpers are the one
 * place that documents every bit the engine relies on). */
#ifndef RENDER_GS_H
#define RENDER_GS_H
#include <stdint.h>

/* GS register addresses for A+D packets. Drawing uses context 1 (PRIM.CTXT = 0). */
enum {
	GS_A_PRIM = 0x00, GS_A_TEX0_1 = 0x06, GS_A_CLAMP_1 = 0x08, GS_A_TEX1_1 = 0x14,
	GS_A_MIPTBP1_1 = 0x34, GS_A_MIPTBP2_1 = 0x36, GS_A_TEXA = 0x3B, GS_A_FOGCOL = 0x3D,
	GS_A_TEXFLUSH = 0x3F, GS_A_ALPHA_1 = 0x42, GS_A_TEST_1 = 0x47, GS_A_PABE = 0x49,
	GS_A_ZBUF_1 = 0x4E, GS_A_BITBLTBUF = 0x50, GS_A_TRXPOS = 0x51, GS_A_TRXREG = 0x52,
	GS_A_TRXDIR = 0x53, GS_A_FINISH = 0x61
};

enum { GS_PSMCT32 = 0x00, GS_PSMCT16S = 0x0A, GS_PSMT8 = 0x13, GS_PSMT4 = 0x14, GS_PSMZ24 = 0x31 };

typedef struct { uint64_t data, addr; } gs_ad;   /* one A+D quadword */

/* PRIM register fields. */
#define GSX_PRIM_TRIANGLE 3
#define GS_PRIM(prim, iip, tme, fge, abe, aa1, fst, ctxt, fix) \
	((uint64_t)(prim) | (uint64_t)(iip) << 3 | (uint64_t)(tme) << 4 | (uint64_t)(fge) << 5 | \
	 (uint64_t)(abe) << 6 | (uint64_t)(aa1) << 7 | (uint64_t)(fst) << 8 | (uint64_t)(ctxt) << 9 | (uint64_t)(fix) << 10)

/* TEX0: base in blocks of 64 words, buffer width in units of 64 pixels, tw/th = log2 size,
 * TCC = RGBA, TFX 0 modulate / 1 decal; CLUT: base in blocks, CPSM, CSM1, CSA, CLD. */
#define GS_TEX0(tbp, tbw, psm, tw, th, tcc, tfx, cbp, cpsm, csm, csa, cld) \
	((uint64_t)((tbp) & 0x3FFF) | (uint64_t)((tbw) & 0x3F) << 14 | (uint64_t)((psm) & 0x3F) << 20 | \
	 (uint64_t)((tw) & 0xF) << 26 | (uint64_t)((th) & 0xF) << 30 | (uint64_t)((tcc) & 1) << 34 | \
	 (uint64_t)((tfx) & 3) << 35 | (uint64_t)((cbp) & 0x3FFF) << 37 | (uint64_t)((cpsm) & 0xF) << 51 | \
	 (uint64_t)((csm) & 1) << 55 | (uint64_t)((csa) & 0x1F) << 56 | (uint64_t)((cld) & 7) << 61)
/* TEX1: LCM, MXL (max mip level), MMAG, MMIN, MTBA, L, K (signed 12-bit, 4 fraction bits). */
#define GS_TEX1(lcm, mxl, mmag, mmin, mtba, l, k) \
	((uint64_t)(lcm) | (uint64_t)(mxl) << 2 | (uint64_t)(mmag) << 5 | (uint64_t)(mmin) << 6 | \
	 (uint64_t)(mtba) << 9 | (uint64_t)(l) << 19 | (uint64_t)((k) & 0xFFF) << 32)
#define GS_CLAMP(wms, wmt, minu, maxu, minv, maxv) \
	((uint64_t)(wms) | (uint64_t)(wmt) << 2 | (uint64_t)(minu) << 4 | (uint64_t)(maxu) << 14 | \
	 (uint64_t)(minv) << 24 | (uint64_t)(maxv) << 34)
#define GS_ALPHA(a, b, c, d, fix) \
	((uint64_t)(a) | (uint64_t)(b) << 2 | (uint64_t)(c) << 4 | (uint64_t)(d) << 6 | (uint64_t)(fix) << 32)
/* TEST: ATE, ATST, AREF, AFAIL, DATE, DATM, ZTE, ZTST (2 = GEQUAL, 1 = ALWAYS). */
#define GS_TEST(ate, atst, aref, afail, date, datm, zte, ztst) \
	((uint64_t)(ate) | (uint64_t)(atst) << 1 | (uint64_t)(aref) << 4 | (uint64_t)(afail) << 12 | \
	 (uint64_t)(date) << 14 | (uint64_t)(datm) << 15 | (uint64_t)(zte) << 16 | (uint64_t)(ztst) << 17)
/* ZBUF: base in 2048-word pages, PSM, ZMSK (1 = do not write Z). */
#define GS_ZBUF(zbp, psm, zmsk) ((uint64_t)(zbp) | (uint64_t)(psm) << 24 | (uint64_t)(zmsk) << 32)
#define GS_FOGCOL(r, g, b) ((uint64_t)(r) | (uint64_t)(g) << 8 | (uint64_t)(b) << 16)
#define GS_BITBLTBUF(sbp, sbw, spsm, dbp, dbw, dpsm) \
	((uint64_t)(sbp) | (uint64_t)(sbw) << 16 | (uint64_t)(spsm) << 24 | \
	 (uint64_t)(dbp) << 32 | (uint64_t)(dbw) << 48 | (uint64_t)(dpsm) << 56)
#define GS_TRXREG(w, h) ((uint64_t)(w) | (uint64_t)(h) << 32)

/* GIF tag, low 64 bits: NLOOP, EOP, PRE, PRIM, FLG (0 packed, 1 reglist, 2 image), NREG. */
#define GIF_TAG(nloop, eop, pre, prim, flg, nreg) \
	((uint64_t)(nloop) | (uint64_t)(eop) << 15 | (uint64_t)(pre) << 46 | (uint64_t)(prim) << 47 | \
	 (uint64_t)(flg) << 58 | (uint64_t)((nreg) & 15) << 60)
#define GIF_REGS_AD 0x0Eu          /* register descriptor 0xE = A+D */
#define GIFX_FLG_PACKED 0
#define GIFX_FLG_IMAGE 2

/* VIF codes (32-bit): cmd << 24 | num << 16 | imm. */
#define VIF_CODE(cmd, num, imm) ((uint32_t)(cmd) << 24 | (uint32_t)((num) & 0xFF) << 16 | (uint32_t)((imm) & 0xFFFF))
enum { VIF_NOP = 0x00, VIF_STCYCL = 0x01, VIF_FLUSHE = 0x10, VIF_FLUSH = 0x11, VIF_FLUSHA = 0x13,
       VIF_MSCAL = 0x14, VIF_OFFSET = 0x02, VIF_BASE = 0x03, VIF_MSCNT = 0x17, VIF_DIRECT = 0x50, VIF_UNPACK_V4_32 = 0x6C };
/* Two VIF codes in the upper half of a DMA tag (transferred when the channel has TTE set). */
#define VIF_PAIR(lo, hi) ((uint64_t)(uint32_t)(lo) | (uint64_t)(uint32_t)(hi) << 32)

#endif
