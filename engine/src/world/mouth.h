/* MOUT section reader (tools/halo2ps2/mouthconv.py): the lip-sync data of the dialogue lines, one loudness byte (0..255) per 30 Hz tick,
 * keyed by (sound tag id, STRM variant). Pure C11, no heap; the data is borrowed from the pack. */
#ifndef WORLD_MOUTH_H
#define WORLD_MOUTH_H
#include <stddef.h>
#include <stdint.h>

typedef struct {
	const uint8_t *recs, *pool;
	uint32_t       count;
} mouth_section;

/* 0 = ok. */
int mouth_open(mouth_section *s, const uint8_t *data, size_t size);
/* The loudness bytes of line `tag`, take `variant` (NULL when it has none); *len receives their count. */
const uint8_t *mouth_find(const mouth_section *s, uint32_t tag, unsigned variant, unsigned *len);

#endif
