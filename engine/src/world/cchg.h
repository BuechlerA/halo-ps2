/* Colour change of objects: the `CCHG` section (tools/halo2ps2/chgconv.py). A model's records give up to four change colours
 * (a..d) of up to eight permutations each (cumulative weight, lower and upper bound); an object picks one per colour from a seed. */
#ifndef WORLD_CCHG_H
#define WORLD_CCHG_H
#include <stddef.h>
#include <stdint.h>

#define CCHG_MAX_CHANGE 4
#define CCHG_MAX_PERM 8
#define CCHG_REC_SIZE 276u

typedef struct {
	const uint8_t *rec;        /* count records of CCHG_REC_SIZE, sorted by model tag id */
	uint32_t       count;
} cchg_t;

/* 0 on success; a bad or missing section leaves the table empty (every lookup fails). */
int cchg_open(cchg_t *c, const uint8_t *data, size_t size);
/* The record of a model tag id, or NULL. */
const uint8_t *cchg_find(const cchg_t *c, uint32_t model_tag);
/* Tint of change colour `source` (1..4 = a..d) for an object with this seed: rgb 0..255; returns 0 when the model has no such colour. */
int cchg_tint(const uint8_t *rec, unsigned source, uint32_t seed, uint8_t rgb[3]);
/* The same with the permutation chosen by the caller (0 .. permutations - 1; the colour inside its bounds still comes from the seed). */
int cchg_tint_perm(const uint8_t *rec, unsigned source, unsigned perm, uint32_t seed, uint8_t rgb[3]);

#endif
