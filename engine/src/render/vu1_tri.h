/* Phase-0 VU1 smoke test: one Gouraud triangle transformed on VU1, double-buffered VIF packets. */
#pragma once
#include <tamtypes.h>

void vu1_tri_init(void);                 /* uploads microcode, sets VU1 double buffer */
void vu1_tri_update(float angle);        /* sim side: rotate */
void vu1_tri_draw(void);                 /* render side: build + send VIF1 chain */
