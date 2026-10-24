#include "sim/settings.h"
#include <string.h>

void settings_default(settings_t *s)
{
	memset(s, 0, sizeof *s);
	s->sens = SETTINGS_SENS_DEFAULT; s->volume = SETTINGS_VOLUME_DEFAULT; s->last_diff = 1;
}

void settings_clamp(settings_t *s)
{
	s->invert = s->invert ? 1u : 0u;
	if (s->sens < 1u) s->sens = 1u;
	if (s->sens > 10u) s->sens = 10u;
	if (s->volume > 10u) s->volume = 10u;
	if (s->last_diff > 3u) s->last_diff = 1u;
	s->completed &= 0x03FFu;
}

static uint16_t checksum(const uint8_t *p, size_t n)
{
	uint32_t a = 1, b = 0;
	size_t i;
	for (i = 0; i < n; i++) { a = (a + p[i]) % 251u; b = (b + a) % 251u; }
	return (uint16_t)(b << 8 | a);
}

size_t settings_pack(const settings_t *s, uint8_t out[SETTINGS_BYTES])
{
	settings_t c = *s;
	uint16_t sum;
	settings_clamp(&c);
	memset(out, 0, SETTINGS_BYTES);
	out[0] = 'H'; out[1] = 'S'; out[2] = 1;
	out[3] = c.invert; out[4] = c.sens; out[5] = c.volume; out[6] = c.last_diff;
	out[7] = (uint8_t)c.completed; out[8] = (uint8_t)(c.completed >> 8);
	sum = checksum(out, SETTINGS_BYTES - 2);
	out[SETTINGS_BYTES - 2] = (uint8_t)sum; out[SETTINGS_BYTES - 1] = (uint8_t)(sum >> 8);
	return SETTINGS_BYTES;
}

int settings_unpack(settings_t *s, const uint8_t *d, size_t n)
{
	uint16_t sum;
	settings_default(s);
	if (!d || n < SETTINGS_BYTES || d[0] != 'H' || d[1] != 'S' || d[2] != 1) return 0;
	sum = checksum(d, SETTINGS_BYTES - 2);
	if (d[SETTINGS_BYTES - 2] != (uint8_t)sum || d[SETTINGS_BYTES - 1] != (uint8_t)(sum >> 8)) return 0;
	s->invert = d[3]; s->sens = d[4]; s->volume = d[5]; s->last_diff = d[6]; s->completed = (uint16_t)(d[7] | d[8] << 8);
	settings_clamp(s);
	return 1;
}

uint16_t settings_unlocked(const settings_t *s, uint16_t installed)
{
	uint16_t out = 0;
	unsigned n;
	for (n = 0; n < 10u; n++) {
		if (!((installed >> n) & 1u)) continue;
		if (n == 0u || ((s->completed >> (n - 1u)) & 1u)) out |= (uint16_t)(1u << n);
	}
	return out;
}
