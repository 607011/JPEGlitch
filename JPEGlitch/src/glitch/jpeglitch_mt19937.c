/*
jpeglitch-obs - JPEG glitch filter for OBS Studio
Copyright (C) 2025 Oliver Lau <oliver.lau@gmail.com>

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#include "jpeglitch_mt19937.h"

void jpeglitch_mt19937_seed(jpeglitch_mt19937 *rng, uint32_t seed)
{
	rng->mt[0] = seed;
	for (int i = 1; i < JPEGLITCH_MT19937_N; ++i) {
		uint32_t prev = rng->mt[i - 1] ^ (rng->mt[i - 1] >> 30);
		rng->mt[i] = (uint32_t)(1812433253u * prev) + (uint32_t)i;
	}
	rng->idx = JPEGLITCH_MT19937_N;
}

static void jpeglitch_mt19937_generate(jpeglitch_mt19937 *rng)
{
	for (int k = 0; k < JPEGLITCH_MT19937_N; ++k) {
		uint32_t y = (rng->mt[k] & 0x80000000u) | (rng->mt[(k + 1) % JPEGLITCH_MT19937_N] & 0x7fffffffu);
		uint32_t next = rng->mt[(k + 397) % JPEGLITCH_MT19937_N] ^ (y >> 1);
		if (y & 1u)
			next ^= 0x9908b0dfu;
		rng->mt[k] = next;
	}
	rng->idx = 0;
}

uint32_t jpeglitch_mt19937_next(jpeglitch_mt19937 *rng)
{
	if (rng->idx >= JPEGLITCH_MT19937_N)
		jpeglitch_mt19937_generate(rng);

	uint32_t y = rng->mt[rng->idx++];
	y ^= y >> 11;
	y ^= (y << 7) & 0x9d2c5680u;
	y ^= (y << 15) & 0xefc60000u;
	y ^= y >> 18;
	return y;
}

uint32_t jpeglitch_mt19937_range(jpeglitch_mt19937 *rng, uint32_t lo, uint32_t hi)
{
	if (lo > hi) {
		uint32_t tmp = lo;
		lo = hi;
		hi = tmp;
	}
	return lo + jpeglitch_mt19937_next(rng) % (hi - lo + 1u);
}
