/* Host test: open the SND0 / SNDD sections of a pack written by tools/tests/test_soundconv.py and check them against
 * the synthetic map that test builds (8 sounds: three weapon sounds, a looping hum with a detail sound, a two-pitch-range
 * impact, a stereo-folded footstep, one streamed dialogue line; 12 permutations; one sound_looping with one track and one
 * detail sound). Then truncate at every length, corrupt every table and field and flip every byte: a corrupt section is
 * either rejected or safe to walk completely (ASan/UBSan). Extra arguments are real packs (the .snd.hp2 files of assets/hp2): opened,
 * walked, ADPCM-checked and byte-flipped on a sample of positions. */
#include "audio/snddef.h"
#include "stream/hp2.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); return 1; } } while (0)

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static void wr32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static void wr16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }

static volatile uint32_t sink;

/* Touch everything an opened section hands out and check the invariants open() promises. */
static void walk(const snd_section *s)
{
	uint32_t acc = 0, i, k, j;
	for (i = 0; i < s->sound_count; i++) {
		snd_sound d = snd_get_sound(s, i);
		uint32_t run = 0;
		if (snd_find(s, d.tag_id) != (int)i) abort();
		acc += d.tag_id + d.klass + d.flags + d.rate + (uint32_t)strlen(d.name) + d.promotion_count + d.max_play_ms;
		if (d.min_dist != d.min_dist || d.gain_mod != d.gain_mod || d.scale_mod[5] != d.scale_mod[5]) abort();
		for (k = 0; k < d.range_count; k++) {
			snd_range r = snd_get_range(s, &d, k);
			if (r.perm_first != run || (uint32_t)r.perm_first + r.perm_count > d.perm_count) abort();
			run += r.perm_count;
			acc += (uint32_t)strlen(r.name);
			for (j = 0; j < r.perm_count; j++) {
				snd_perm p = snd_get_perm(s, &d, r.perm_first + j);
				if (p.range != k || (p.next != SND_NO_PERM && p.next >= r.perm_count)) abort();
			}
		}
		if (run != d.perm_count) abort();
		for (k = 0; k < d.perm_count; k++) {
			snd_perm p = snd_get_perm(s, &d, k);
			if ((uint64_t)p.sample_offset + p.size > s->sndd_size || p.sample_offset % 16u || p.size % 16u || p.size < 48u) abort();
			if (p.frames > (p.size / 16u - 2u) * 28u || p.rate == 0 || p.rate > 48000u) abort();
			if ((p.flags & SND_P_LOOP) ? (p.loop_start != 0 || p.loop_end != p.size - 32u) : (p.loop_start || p.loop_end)) abort();
			if (p.gain != p.gain || p.skip != p.skip) abort();
			acc += p.sample_offset + p.size + p.frames + p.rate + (uint32_t)strlen(p.name) + p.src_bytes + p.next;
		}
	}
	for (i = 0; i < s->loop_count; i++) {
		snd_loop l = snd_get_loop(s, i);
		if (snd_find_loop(s, l.tag_id) != (int)i) abort();
		acc += l.tag_id + l.flags + (uint32_t)strlen(l.name);
		for (k = 0; k < l.track_count; k++) {
			snd_track t = snd_get_track(s, &l, k);
			acc += t.flags + t.start + t.loop + t.end + t.alt_loop + t.alt_end;
			if (t.gain != t.gain || t.fade_in != t.fade_in || t.fade_out != t.fade_out) abort();
		}
		for (k = 0; k < l.detail_count; k++) {
			snd_detail d = snd_get_detail(s, &l, k);
			acc += d.sound + d.flags;
			if (d.period_lo != d.period_lo || d.dist_hi != d.dist_hi || d.yaw_lo != d.yaw_lo) abort();
		}
	}
	/* out-of-range queries give neutral records */
	{
		snd_sound d = snd_get_sound(s, s->sound_count + 5);
		snd_loop l = snd_get_loop(s, s->loop_count + 5);
		if (d.tag_id || d.perm_count || l.tag_id) abort();
		if (snd_get_perm(s, &d, 0).size || snd_get_range(s, &d, 0).perm_count) abort();
		if (snd_get_track(s, &l, 0).start != SND_NO_TAG || snd_get_detail(s, &l, 0).sound != SND_NO_TAG) abort();
	}
	sink = acc;
}

static int load_section(const hp2_pack *pk, const char *tag, uint8_t **raw, size_t *n)
{
	const hp2_section *sec = hp2_find(pk, tag);
	CHECK(sec);
	*raw = malloc(sec->raw_size ? sec->raw_size : 1);
	CHECK(*raw && hp2_load(pk, sec, *raw, sec->raw_size) == HP2_OK);
	*n = sec->raw_size;
	return 0;
}

static int load_pack(const char *path, uint8_t **img, uint8_t **snd0, size_t *n0, uint8_t **sndd, size_t *n1)
{
	static hp2_pack pk;
	FILE *f = fopen(path, "rb");
	size_t n;
	CHECK(f);
	fseek(f, 0, SEEK_END);
	n = (size_t)ftell(f);
	rewind(f);
	*img = malloc(n);
	CHECK(*img && fread(*img, 1, n, f) == n);
	fclose(f);
	CHECK(hp2_open(&pk, *img, n) == HP2_OK);
	if (load_section(&pk, "SND0", snd0, n0) || load_section(&pk, "SNDD", sndd, n1)) return 1;
	return 0;
}

static int near1(float a, float b) { return fabsf(a - b) < 1e-5f; }

/* corrupt one byte of a copy; the section is refused or completely walkable */
static int fuzz(const uint8_t *raw, size_t rawn, uint32_t sndd_n, size_t stride)
{
	uint8_t *copy = malloc(rawn);
	snd_section bad;
	size_t at;
	int pat;
	CHECK(copy);
	for (at = 4; at < rawn; at += stride)
		for (pat = 0; pat < 3; pat++) {
			memcpy(copy, raw, rawn);
			copy[at] = pat == 0 ? (uint8_t)(copy[at] ^ 0xFF) : pat == 1 ? (uint8_t)(copy[at] ^ 0x01) : 0x7F;
			if (snd_open(&bad, copy, rawn, sndd_n) == SND_OK) walk(&bad);
		}
	free(copy);
	return 0;
}

static int real_pack(const char *path)
{
	uint8_t *img, *raw, *sndd;
	size_t rawn, dn;
	snd_section s;
	if (load_pack(path, &img, &raw, &rawn, &sndd, &dn)) return 1;
	CHECK(snd_open(&s, raw, rawn, (uint32_t)dn) == SND_OK);
	CHECK(s.sound_count > (strstr(path, "/ui.snd") ? 20u : 100u) && s.perm_count > (strstr(path, "/ui.snd") ? 20u : 100u) && s.sndd_size % 64u == 0);     /* the ui scenario's pack is small: its effects and the menu clicks */
	walk(&s);
	CHECK(snd_check_vag(&s, sndd, dn) == 0);
	uint32_t spu = 0;
	for (uint32_t i = 0; i < s.sound_count; i++) {          /* the contract: mono <= 22050 Hz, SPU2 resident set <= 1.4 MiB */
		snd_sound d = snd_get_sound(&s, i);
		CHECK(d.rate <= 22050u);
		for (uint32_t k = 0; k < d.perm_count; k++) {
			snd_perm p = snd_get_perm(&s, &d, k);
			CHECK(p.rate <= 22050u);
			spu += p.size - 16u;                              /* audsrv uploads everything after the 16-byte header */
		}
	}
	CHECK(spu <= SND_BUDGET_BYTES);
	if (fuzz(raw, rawn, (uint32_t)dn, 61)) return 1;
	printf("%s: %u sounds, %u permutations, %u loops, SNDD %u bytes: ok\n", path, s.sound_count, s.perm_count, s.loop_count, s.sndd_size);
	free(raw);
	free(sndd);
	free(img);
	return 0;
}

int main(int argc, char **argv)
{
	CHECK(argc >= 2);
	uint8_t *img, *raw, *sndd;
	size_t rawn, dn;
	if (load_pack(argv[1], &img, &raw, &rawn, &sndd, &dn)) return 1;
	uint32_t sndd_n = (uint32_t)dn;

	snd_section s;
	CHECK(snd_open(&s, raw, rawn, sndd_n) == SND_OK);
	CHECK(s.sound_count == 8 && s.range_count == 8 && s.perm_count == 12 && s.loop_count == 1 && s.track_count == 1 && s.detail_count == 1);
	CHECK(s.sndd_size == 9216 && s.budget == SND_BUDGET_BYTES && s.dropped_sounds == 0 && s.dropped_perms == 0);
	CHECK(s.degraded_sounds == 0 && s.streamed_sounds == 1);

	/* sounds are sorted by tag id and found by binary search */
	static const uint32_t ids[8] = { 0xE17D0009u, 0xE17E000Au, 0xE17F000Bu, 0xE180000Cu, 0xE181000Du, 0xE182000Eu, 0xE183000Fu, 0xE1850011u };
	for (uint32_t i = 0; i < 8; i++) {
		CHECK(snd_find(&s, ids[i]) == (int)i);
		CHECK(snd_get_sound(&s, i).tag_id == ids[i]);
	}
	CHECK(snd_find(&s, 0) == -1 && snd_find(&s, 0xFFFFFFFFu) == -1 && snd_find(&s, ids[3] + 1) == -1 && snd_find(&s, 0xE1840010u) == -1);

	/* the rifle: three permutations in one pitch range, a chain, gains, distances, a promotion sound */
	snd_sound f = snd_get_sound(&s, 0);
	CHECK(f.klass == SND_CLASS_WEAPON_FIRE && f.flags == 0 && f.rate == 22050 && f.range_count == 1 && f.perm_count == 3);
	CHECK(f.promotion_tag == 0xE1850011u && f.promotion_count == 5 && f.perms_dropped == 0 && f.max_play_ms == 97);
	CHECK(near1(f.min_dist, 1.5f) && near1(f.max_dist, 12.0f) && near1(f.pitch_lo, 0.9f) && near1(f.pitch_hi, 1.1f));
	CHECK(near1(f.inner_cone, 6.2831855f) && near1(f.outer_gain, 1.0f) && near1(f.gain_mod, 0.85f));
	CHECK(near1(f.scale_mod[0], 1.0f) && near1(f.scale_mod[1], 0.0f) && near1(f.scale_mod[2], 1.0f) && near1(f.scale_mod[4], 1.0f));
	CHECK(strcmp(f.name, "sound\\sfx\\weapons\\rifle\\fire") == 0);
	snd_range fr = snd_get_range(&s, &f, 0);
	CHECK(fr.perm_first == 0 && fr.perm_count == 3 && near1(fr.natural_pitch, 1.0f) && strcmp(fr.name, "default") == 0);
	static const uint32_t foff[3] = { 0, 768, 1904 }, fsize[3] = { 768, 1136, 960 }, fframes[3] = { 1280, 1920, 1600 };
	static const char *const fname[3] = { "shot1", "shot2", "shot3" };
	static const float fgain[3] = { 1.0f, 0.5f, 0.75f };
	for (uint32_t k = 0; k < 3; k++) {
		snd_perm p = snd_get_perm(&s, &f, k);
		CHECK(p.sample_offset == foff[k] && p.size == fsize[k] && p.frames == fframes[k] && p.rate == 22050 && p.flags == 0);
		CHECK(near1(p.gain, fgain[k]) && strcmp(p.name, fname[k]) == 0 && p.range == 0 && p.src_index == k);
		CHECK(p.next == (k == 0 ? 1u : SND_NO_PERM) && p.loop_start == 0 && p.loop_end == 0 && p.src_bytes == (fframes[k] / 64u) * 36u);
	}
	CHECK(near1(snd_get_perm(&s, &f, 1).skip, 0.25f));
	CHECK(snd_get_perm(&s, &f, 3).size == 0 && snd_get_range(&s, &f, 1).perm_count == 0);       /* past the end: neutral */

	/* the looping hum: loop flags, 28-frame multiple, rate lowered by the shrink */
	snd_sound h = snd_get_sound(&s, 2);
	CHECK(h.klass == SND_CLASS_DEVICE_MACHINERY && h.flags == SND_F_LOOP && h.perm_count == 1);
	snd_perm hp = snd_get_perm(&s, &h, 0);
	CHECK(hp.flags == SND_P_LOOP && hp.frames == 2548 && hp.frames % 28 == 0 && hp.rate == 21947 && hp.size == 1488);
	CHECK(hp.loop_start == 0 && hp.loop_end == 1456 && hp.sample_offset == 4368);

	/* stereo folded footsteps */
	snd_sound st = snd_get_sound(&s, 4);
	CHECK(st.klass == SND_CLASS_UNIT_FOOTSTEPS && st.perm_count == 2);
	CHECK(snd_get_perm(&s, &st, 0).flags == SND_P_FOLDED && snd_get_perm(&s, &st, 1).frames == 960 && snd_get_perm(&s, &st, 0).src_bytes == 2160);

	/* two pitch ranges: 2 + 1 permutations, range index per permutation */
	snd_sound r = snd_get_sound(&s, 5);
	CHECK(r.range_count == 2 && r.perm_count == 3);
	snd_range r0 = snd_get_range(&s, &r, 0), r1 = snd_get_range(&s, &r, 1);
	CHECK(r0.perm_first == 0 && r0.perm_count == 2 && r1.perm_first == 2 && r1.perm_count == 1 && near1(r1.natural_pitch, 1.5f));
	CHECK(snd_get_perm(&s, &r, 1).range == 0 && snd_get_perm(&s, &r, 2).range == 1 && strcmp(r1.name, "range1") == 0);

	/* streamed dialogue: listed, flagged, no data */
	snd_sound d = snd_get_sound(&s, 6);
	CHECK(d.klass == SND_CLASS_UNIT_DIALOG && d.flags == SND_F_STREAMED && d.perm_count == 0 && d.range_count == 0 && d.rate == 0);

	/* sound_looping: one track whose loop is the hum, one detail sound */
	CHECK(snd_find_loop(&s, 0xE17A0006u) == 0 && snd_find_loop(&s, 1) == -1);
	snd_loop lp = snd_get_loop(&s, 0);
	CHECK(lp.flags == 1 && near1(lp.param, 5.0f) && lp.track_count == 1 && lp.detail_count == 1 && strcmp(lp.name, "sound\\sfx\\ambience\\hum") == 0);
	snd_track tr = snd_get_track(&s, &lp, 0);
	CHECK(tr.flags == 3 && tr.loop == 0xE17F000Bu && tr.start == SND_NO_TAG && tr.end == SND_NO_TAG && tr.alt_loop == SND_NO_TAG && tr.alt_end == SND_NO_TAG);
	CHECK(near1(tr.gain, 1.0f) && near1(tr.fade_in, 0.1f) && near1(tr.fade_out, 0.2f));
	snd_detail de = snd_get_detail(&s, &lp, 0);
	CHECK(de.sound == 0xE180000Cu && near1(de.period_lo, 0.5f) && near1(de.period_hi, 3.0f) && near1(de.gain, 0.8f) && de.flags == 0);
	CHECK(near1(de.yaw_lo, -3.14f) && near1(de.pitch_hi, 1.5f) && near1(de.dist_lo, 0.0f) && near1(de.dist_hi, 20.0f));
	CHECK(snd_get_track(&s, &lp, 1).start == SND_NO_TAG && snd_get_detail(&s, &lp, 1).sound == SND_NO_TAG);
	walk(&s);

	/* the ADPCM payload agrees with the tables; damaging it is noticed and attributed */
	CHECK(snd_check_vag(&s, sndd, dn) == 0);
	uint8_t *dcopy = malloc(dn);
	CHECK(dcopy);
	memcpy(dcopy, sndd, dn);
	dcopy[hp.sample_offset + 16] = 0x70;                                  /* predictor 7 in the hum's first block: permutation index 4 (1-based 5) */
	CHECK(snd_check_vag(&s, dcopy, dn) == 5);
	memcpy(dcopy, sndd, dn);
	dcopy[hp.sample_offset + hp.size - 32 + 1] = 0;                       /* lose the loop end flag of the last data block */
	CHECK(snd_check_vag(&s, dcopy, dn) == 5);
	memcpy(dcopy, sndd, dn);
	dcopy[hp.sample_offset + 16 + 1] = 0;                                 /* lose the loop start flag */
	CHECK(snd_check_vag(&s, dcopy, dn) == 5);
	memcpy(dcopy, sndd, dn);
	dcopy[hp.sample_offset + 6] = 0;                                      /* header loop byte disagrees with the table */
	CHECK(snd_check_vag(&s, dcopy, dn) == 5);
	memcpy(dcopy, sndd, dn);
	dcopy[hp.sample_offset + 8] ^= 1;                                     /* wrong pitch word */
	CHECK(snd_check_vag(&s, dcopy, dn) == 5);
	memcpy(dcopy, sndd, dn);
	dcopy[hp.sample_offset + 12] ^= 1;                                    /* wrong frame count in the header */
	CHECK(snd_check_vag(&s, dcopy, dn) == 5);
	memcpy(dcopy, sndd, dn);
	dcopy[hp.sample_offset + hp.size - 15] = 3;                           /* terminator flags */
	CHECK(snd_check_vag(&s, dcopy, dn) == 5);
	memcpy(dcopy, sndd, dn);
	dcopy[hp.sample_offset + hp.size - 3] = 1;                            /* terminator nibbles must be zero */
	CHECK(snd_check_vag(&s, dcopy, dn) == 5);
	memcpy(dcopy, sndd, dn);
	dcopy[0] = 'X';                                                       /* magic of the first sample */
	CHECK(snd_check_vag(&s, dcopy, dn) == 1);
	memcpy(dcopy, sndd, dn);
	dcopy[fsize[0] - 32 + 1] = 2;                                         /* a one-shot whose last block repeats */
	CHECK(snd_check_vag(&s, dcopy, dn) == 1);
	memcpy(dcopy, sndd, dn);
	dcopy[16 + 16 + 1] = 1;                                               /* an end flag in the middle of a permutation */
	CHECK(snd_check_vag(&s, dcopy, dn) == 1);
	memcpy(dcopy, sndd, dn);
	dcopy[16 + 16] = 0x0D;                                                /* shift 13 */
	CHECK(snd_check_vag(&s, dcopy, dn) == 1);
	CHECK(snd_check_vag(&s, sndd, dn - 16) != 0 && snd_check_vag(&s, NULL, dn) != 0);
	for (size_t at = 0; at < dn; at++) {                                  /* flipping any payload byte never crashes the checker */
		memcpy(dcopy, sndd, dn);
		dcopy[at] ^= 0xFF;
		sink = snd_check_vag(&s, dcopy, dn);
	}
	free(dcopy);

	/* a failed open leaves a safe, empty view */
	snd_section bad;
	CHECK(snd_open(&bad, raw, 10, sndd_n) == SND_ERR_TRUNCATED);
	CHECK(bad.sound_count == 0 && snd_find(&bad, ids[0]) == -1 && snd_get_sound(&bad, 0).tag_id == 0);
	walk(&bad);

	/* ---- hostile input ---- */
	for (size_t cut = 0; cut < rawn; cut++) CHECK(snd_open(&bad, raw, cut, sndd_n) != SND_OK);
	CHECK(snd_open(&bad, NULL, rawn, sndd_n) != SND_OK);
	CHECK(snd_open(&bad, raw, rawn, sndd_n + 16) == SND_ERR_LAYOUT);      /* SNDD size differs from the header */
	CHECK(snd_open(&bad, raw, rawn, sndd_n - 16) == SND_ERR_LAYOUT);
	CHECK(snd_open(&bad, raw, rawn, 0) == SND_ERR_LAYOUT);
	uint8_t *copy = malloc(rawn + 16);
	CHECK(copy);
	memcpy(copy, raw, rawn);
	memset(copy + rawn, 0, 16);
	CHECK(snd_open(&bad, copy, rawn + 16, sndd_n) == SND_ERR_LAYOUT);     /* size field no longer matches */
	memcpy(copy, raw, rawn);
	copy[4] = 2;
	CHECK(snd_open(&bad, copy, rawn, sndd_n) == SND_ERR_VERSION);
	memcpy(copy, raw, rawn);
	copy[0] = 'X';
	CHECK(snd_open(&bad, copy, rawn, sndd_n) == SND_ERR_MAGIC);

	/* every counted table of the header (count offset, table offset): moved, misaligned, oversized, counted wrong */
	static const struct { const char *name; size_t count_at, off_at; } tabs[] = {
		{ "sounds", 0x0C, 0x28 }, { "ranges", 0x10, 0x2C }, { "perms", 0x14, 0x30 }, { "loops", 0x18, 0x34 },
		{ "tracks", 0x1C, 0x38 }, { "details", 0x20, 0x3C } };
	for (size_t t = 0; t < sizeof tabs / sizeof tabs[0]; t++) {
		uint32_t off = rd32(raw + tabs[t].off_at), cnt = rd32(raw + tabs[t].count_at);
		const struct { uint32_t off, cnt; } tries[] = { { off + 4, cnt }, { (uint32_t)rawn + 16, cnt }, { off, 0xFFFFFFFFu },
		                                                { off, (uint32_t)rawn }, { 0, cnt }, { 0xFFFFFFF0u, cnt } };
		for (size_t k = 0; k < sizeof tries / sizeof tries[0]; k++) {
			memcpy(copy, raw, rawn);
			wr32(copy + tabs[t].off_at, tries[k].off);
			wr32(copy + tabs[t].count_at, tries[k].cnt);
			if (snd_open(&bad, copy, rawn, sndd_n) == SND_OK) { fprintf(stderr, "table %s try %zu accepted\n", tabs[t].name, k); return 1; }
		}
		for (int dd = -1; dd <= 1; dd += 2) {                              /* merely wrong counts must never read out of bounds */
			memcpy(copy, raw, rawn);
			wr32(copy + tabs[t].count_at, cnt + (uint32_t)dd);
			if (snd_open(&bad, copy, rawn, sndd_n) == SND_OK) walk(&bad);
		}
	}

	/* targeted fields: each of these must be refused */
	uint32_t os = rd32(raw + 0x28), orr = rd32(raw + 0x2C), op = rd32(raw + 0x30), ol = rd32(raw + 0x34), ot = rd32(raw + 0x38), od = rd32(raw + 0x3C);
	const struct { const char *what; size_t at; uint32_t val; int bytes; } hit[] = {
		{ "sounds out of order", os + 256, ids[0], 4 }, { "sounds equal", os + 128, ids[0], 4 },
		{ "sound flags", os + 9, 0x80, 1 }, { "sound flags bit 5", os + 9, 0x20, 1 }, { "sound rate", os + 10, 50000, 2 },
		{ "range_first past table", os + 12, 100, 4 }, { "range_count past table", os + 16, 100, 2 },
		{ "perm_first past table", os + 20, 100, 4 }, { "perm_count past table", os + 18, 100, 2 },
		{ "perm_count 0 with ranges", os + 18, 0, 2 }, { "range_count 0 with perms", os + 16, 0, 2 },
		{ "data on a streamed sound", os + 6 * 128 + 18, 1, 2 }, { "dropped flag with data", os + 9, SND_F_DROPPED, 1 },
		{ "sound float NaN", os + 32, 0x7FC00000u, 4 }, { "sound float inf", os + 44, 0x7F800000u, 4 }, { "gain_mod NaN", os + 64, 0x7FC00000u, 4 },
		{ "scale_mod NaN", os + 88, 0x7FC00000u, 4 }, { "sound name unterminated", os + 96 + 31, 'x', 1 },
		{ "range first perm", orr + 12, 1, 2 }, { "range count too big", orr + 14, 4, 2 }, { "range count short", orr + 14, 2, 2 },
		{ "range NaN", orr, 0x7FC00000u, 4 }, { "range name unterminated", orr + 16 + 15, 'x', 1 },
		{ "perm offset past SNDD", op, sndd_n, 4 }, { "perm offset misaligned", op, 8, 4 }, { "perm size not 16s", op + 4, 17, 4 },
		{ "perm size 0", op + 4, 0, 4 }, { "perm runs past SNDD", op + 4, sndd_n, 4 }, { "perm frames too many", op + 8, 5000, 4 },
		{ "perm frames too few", op + 8, 10, 4 }, { "perm rate 0", op + 12, 0, 4 }, { "perm rate 50000", op + 12, 50000, 4 },
		{ "loop start on a one-shot", op + 16, 16, 4 }, { "loop end on a one-shot", op + 20, 736, 4 },
		{ "perm gain NaN", op + 24, 0x7FC00000u, 4 }, { "perm skip inf", op + 28, 0x7F800000u, 4 },
		{ "next past range", op + 32, 3, 2 }, { "perm flags", op + 34, 8, 2 }, { "perm range index", op + 36, 1, 2 },
		{ "perm name unterminated", op + 40 + 15, 'x', 1 },
		{ "loop flag without loop bounds", op + 34, SND_P_LOOP, 2 }, { "loop start not 0", op + 64 * 4 + 16, 16, 4 },
		{ "loop end short", op + 64 * 4 + 20, 16, 4 },
		{ "loops first track past table", ol + 16, 5, 4 }, { "loops track count", ol + 12, 5, 2 }, { "loops detail first", ol + 20, 5, 4 },
		{ "loops detail count", ol + 14, 5, 2 }, { "loop param NaN", ol + 8, 0x7FC00000u, 4 }, { "loop name unterminated", ol + 32 + 31, 'x', 1 },
		{ "track gain NaN", ot + 4, 0x7FC00000u, 4 }, { "track fade_out inf", ot + 12, 0x7F800000u, 4 },
		{ "detail period NaN", od + 4, 0x7FC00000u, 4 }, { "detail yaw inf", od + 20, 0x7F800000u, 4 }, { "detail dist NaN", od + 40, 0x7FC00000u, 4 },
		{ "header size", 8, 1, 4 }, { "sndd size field", 0x24, 16, 4 } };
	for (size_t k = 0; k < sizeof hit / sizeof hit[0]; k++) {
		memcpy(copy, raw, rawn);
		if (hit[k].bytes == 4) wr32(copy + hit[k].at, hit[k].val);
		else if (hit[k].bytes == 2) wr16(copy + hit[k].at, hit[k].val);
		else copy[hit[k].at] = (uint8_t)hit[k].val;
		if (snd_open(&bad, copy, rawn, sndd_n) == SND_OK) { fprintf(stderr, "accepted: %s\n", hit[k].what); return 1; }
	}

	/* no single-byte change anywhere may let an out-of-bounds read through */
	if (fuzz(raw, rawn, sndd_n, 1)) return 1;
	free(copy);
	free(raw);
	free(sndd);
	free(img);

	for (int i = 2; i < argc; i++)
		if (real_pack(argv[i])) return 1;
	puts("ok");
	return 0;
}
