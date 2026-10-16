/* .hp2 pack reader (docs/07-hp2-format.md). Pure C11, no ps2sdk dependency, so the same
 * code is unit-tested on the host (make -C engine test). */
#ifndef HP2_H
#define HP2_H
#include <stddef.h>
#include <stdint.h>

#define HP2_MAX_SECTIONS 64
#define HP2_SECTOR 2048u

enum { HP2_CODEC_STORED = 0, HP2_CODEC_LZ4 = 1 };

typedef enum {
	HP2_OK = 0,
	HP2_ERR_TRUNCATED,   /* shorter than the header, table or a payload */
	HP2_ERR_MAGIC,       /* not an hp2 pack or unsupported version */
	HP2_ERR_LAYOUT,      /* sizes/offsets inconsistent */
	HP2_ERR_NOT_FOUND,   /* no section with that tag */
	HP2_ERR_CODEC,       /* unknown codec id */
	HP2_ERR_DECODE,      /* corrupt compressed stream */
	HP2_ERR_SPACE,       /* destination too small */
	HP2_ERR_CRC          /* decoded data does not match the section CRC */
} hp2_status;

typedef struct {
	char     tag[5];     /* NUL-terminated */
	uint32_t offset, stored_size, raw_size, codec, crc32, align;
} hp2_section;

typedef struct {
	const uint8_t *image; /* whole file, not owned */
	size_t         size;
	char           name[17];
	uint32_t       count;
	hp2_section    sections[HP2_MAX_SECTIONS];
} hp2_pack;

/* Parse and validate the header and section table of a pack held in memory. */
hp2_status hp2_open(hp2_pack *pk, const uint8_t *image, size_t size);

/* Section by tag, e.g. "RES" or "BSP0"; NULL if absent. */
const hp2_section *hp2_find(const hp2_pack *pk, const char *tag);

/* Decode section `s` into dst (capacity cap >= raw_size) and verify its CRC. */
hp2_status hp2_load(const hp2_pack *pk, const hp2_section *s, uint8_t *dst, size_t cap);

/* LZ4 block decoder: returns decoded size or -1 on malformed input / overflow of dst.
 * Reads and writes are bounds-checked; matches may overlap their own output. */
long lz4_decode(const uint8_t *src, size_t src_len, uint8_t *dst, size_t dst_cap);

uint32_t hp2_crc32(const uint8_t *p, size_t n);

#endif
