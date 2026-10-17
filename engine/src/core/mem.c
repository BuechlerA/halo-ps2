#include "core/mem.h"
#include <string.h>

void arena_init(arena_t *a, const char *name, void *mem, size_t cap)
{
	a->base = mem;
	a->cap = cap;
	a->used = a->high = 0;
	a->name = name;
}

void *arena_alloc(arena_t *a, size_t n, size_t align)
{
	size_t start;
	if (align == 0 || (align & (align - 1))) return NULL;
	/* Align the address, not just the offset: `base` itself may not be aligned. */
	start = ((uintptr_t)a->base + a->used + align - 1) & ~(uintptr_t)(align - 1);
	start -= (uintptr_t)a->base;
	if (start > a->cap || n > a->cap - start) return NULL;
	a->used = start + n;
	if (a->used > a->high) a->high = a->used;
	return a->base + start;
}

int arena_sub(arena_t *out, arena_t *parent, const char *name, size_t cap)
{
	void *p = arena_alloc(parent, cap, 16);
	if (!p) return -1;
	arena_init(out, name, p, cap);
	return 0;
}

void arena_reset(arena_t *a)
{
	a->used = 0;
}

size_t arena_free(const arena_t *a)
{
	return a->cap - a->used;
}

size_t arena_mark(const arena_t *a)
{
	return a->used;
}

void arena_release(arena_t *a, size_t mark)
{
	if (mark <= a->used) a->used = mark;
}
