/* EE cycle counter (COP0 Count). Increments every EE clock at 294.912 MHz. */
#pragma once
#include <tamtypes.h>

#define EE_CLOCK_HZ 294912000u

static inline u32 timer_cycles(void)
{
	u32 c;
	__asm__ volatile("mfc0 %0, $9" : "=r"(c));
	return c;
}

/* Elapsed milliseconds between two timer_cycles() samples (wraps every ~14.5 s). */
static inline float timer_ms(u32 start, u32 end)
{
	return (float)(end - start) * (1000.0f / (float)EE_CLOCK_HZ);
}
