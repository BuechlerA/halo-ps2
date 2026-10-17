#include "render/chain.h"
#include <string.h>

void chain_init(chain_t *c, void *mem, uint32_t cap_qw)
{
	c->base = mem;
	c->cap_qw = cap_qw;
	c->qw = 0;
	c->overflow = 0;
}

static uint64_t *put_tag(chain_t *c, uint32_t qwc, uint32_t id, uint32_t addr, uint64_t vif_pair)
{
	uint64_t *t;
	if (c->qw + 1 > c->cap_qw) { c->overflow = 1; return NULL; }
	t = (uint64_t *)(c->base + (size_t)c->qw * 16);
	t[0] = (uint64_t)qwc | (uint64_t)id << 28 | (uint64_t)addr << 32;
	t[1] = vif_pair;
	c->qw++;
	return t;
}

void chain_ref(chain_t *c, const void *data, uint32_t qw, uint64_t vif_pair)
{
	put_tag(c, qw, DMA_ID_REF, CHAIN_PHYS(data), vif_pair);
}

void *chain_cnt(chain_t *c, uint32_t data_qw, uint64_t vif_pair)
{
	uint8_t *d;
	if (c->qw + 1 + data_qw > c->cap_qw) { c->overflow = 1; return NULL; }
	put_tag(c, data_qw, DMA_ID_CNT, 0, vif_pair);
	d = c->base + (size_t)c->qw * 16;
	c->qw += data_qw;
	return d;
}

void chain_end(chain_t *c, uint64_t vif_pair)
{
	put_tag(c, 0, DMA_ID_END, 0, vif_pair);
}

uint32_t chain_qwords(const chain_t *c)
{
	return c->qw;
}
