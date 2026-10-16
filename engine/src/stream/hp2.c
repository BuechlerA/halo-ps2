#include "stream/hp2.h"
#include <string.h>

static uint32_t rd16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
static uint32_t rd32(const uint8_t *p) { return rd16(p) | rd16(p + 2) << 16; }

uint32_t hp2_crc32(const uint8_t *p, size_t n)
{
	uint32_t crc = 0xFFFFFFFFu;
	while (n--) {
		crc ^= *p++;
		for (int k = 0; k < 8; k++)
			crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
	}
	return ~crc;
}

hp2_status hp2_open(hp2_pack *pk, const uint8_t *image, size_t size)
{
	memset(pk, 0, sizeof *pk);
	if (size < 64) return HP2_ERR_TRUNCATED;
	if (memcmp(image, "HP2P", 4) != 0 || rd16(image + 4) != 1) return HP2_ERR_MAGIC;
	pk->count = rd16(image + 6);
	if (pk->count > HP2_MAX_SECTIONS || rd32(image + 8) != size) return HP2_ERR_LAYOUT;
	if (64u + 32u * pk->count > size) return HP2_ERR_TRUNCATED;
	memcpy(pk->name, image + 0x10, 16);
	pk->name[16] = 0;
	pk->image = image;
	pk->size = size;
	for (uint32_t i = 0; i < pk->count; i++) {
		const uint8_t *e = image + 64 + 32 * i;
		hp2_section *s = &pk->sections[i];
		memcpy(s->tag, e, 4);
		s->tag[4] = 0;
		s->offset = rd32(e + 4);
		s->stored_size = rd32(e + 8);
		s->raw_size = rd32(e + 12);
		s->codec = rd32(e + 16);
		s->crc32 = rd32(e + 20);
		s->align = rd32(e + 24);
		if (s->offset % HP2_SECTOR) return HP2_ERR_LAYOUT;
		if (s->offset > size || s->stored_size > size - s->offset) return HP2_ERR_TRUNCATED;
	}
	return HP2_OK;
}

const hp2_section *hp2_find(const hp2_pack *pk, const char *tag)
{
	for (uint32_t i = 0; i < pk->count; i++)
		if (strncmp(pk->sections[i].tag, tag, 4) == 0) return &pk->sections[i];
	return NULL;
}

long lz4_decode(const uint8_t *src, size_t n, uint8_t *dst, size_t cap)
{
	size_t i = 0, o = 0;
	while (i < n) {
		uint32_t token = src[i++];
		size_t lit = token >> 4;
		if (lit == 15) {
			uint32_t b;
			do {
				if (i >= n) return -1;
				b = src[i++];
				lit += b;
			} while (b == 255);
		}
		if (lit > n - i || lit > cap - o) return -1;
		memcpy(dst + o, src + i, lit);
		i += lit;
		o += lit;
		if (i >= n) break;                      /* last sequence has literals only */
		if (n - i < 2) return -1;
		size_t off = rd16(src + i);
		i += 2;
		size_t ml = token & 15;
		if (ml == 15) {
			uint32_t b;
			do {
				if (i >= n) return -1;
				b = src[i++];
				ml += b;
			} while (b == 255);
		}
		ml += 4;
		if (off == 0 || off > o || ml > cap - o) return -1;
		for (size_t k = 0; k < ml; k++, o++)    /* byte-wise: source and destination may overlap */
			dst[o] = dst[o - off];
	}
	return (long)o;
}

hp2_status hp2_load(const hp2_pack *pk, const hp2_section *s, uint8_t *dst, size_t cap)
{
	const uint8_t *stored = pk->image + s->offset;
	if (cap < s->raw_size) return HP2_ERR_SPACE;
	if (s->codec == HP2_CODEC_STORED) {
		if (s->stored_size != s->raw_size) return HP2_ERR_LAYOUT;
		memcpy(dst, stored, s->raw_size);
	} else if (s->codec == HP2_CODEC_LZ4) {
		long got = lz4_decode(stored, s->stored_size, dst, s->raw_size);
		if (got < 0 || (uint32_t)got != s->raw_size) return HP2_ERR_DECODE;
	} else {
		return HP2_ERR_CODEC;
	}
	return hp2_crc32(dst, s->raw_size) == s->crc32 ? HP2_OK : HP2_ERR_CRC;
}
