/* Linear arenas (ADR-006). The only allocator the engine uses after boot: one malloc at startup
 * hands the whole budget to arena_init(); everything else is carved out of arenas that are reset
 * wholesale (per frame, per BSP, per level). Pure C11 so it is unit-tested on the host. */
#ifndef CORE_MEM_H
#define CORE_MEM_H
#include <stddef.h>
#include <stdint.h>

typedef struct {
	uint8_t *base;
	size_t   cap, used, high;   /* high = peak `used`, for the memory report */
	const char *name;
} arena_t;

void  arena_init(arena_t *a, const char *name, void *mem, size_t cap);
/* Carve a sub-arena out of `parent` (aligned to 16). Returns 0 on success. */
int   arena_sub(arena_t *out, arena_t *parent, const char *name, size_t cap);
/* Allocate `n` bytes aligned to `align` (power of two, >= 1). NULL if the arena is full. */
void *arena_alloc(arena_t *a, size_t n, size_t align);
void  arena_reset(arena_t *a);
/* Temporary allocations: take a mark, allocate scratch, release the mark (everything allocated
 * after the mark is freed; allocations made before it stay). */
size_t arena_mark(const arena_t *a);
void   arena_release(arena_t *a, size_t mark);
size_t arena_free(const arena_t *a);

#endif
