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

#ifndef JPEGLITCH_MT19937_H
#define JPEGLITCH_MT19937_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define JPEGLITCH_MT19937_N 624

typedef struct jpeglitch_mt19937 {
	uint32_t mt[JPEGLITCH_MT19937_N];
	int idx;
} jpeglitch_mt19937;

/* (Re-)seeds the generator. Matches the standard MT19937 reference
 * initialization (same as the JS MersenneTwister in reference/qlitch.html). */
void jpeglitch_mt19937_seed(jpeglitch_mt19937 *rng, uint32_t seed);

/* Returns the next 32-bit pseudo-random number. */
uint32_t jpeglitch_mt19937_next(jpeglitch_mt19937 *rng);

/* Returns a value in [lo, hi] (inclusive). Swaps lo/hi if lo > hi,
 * exactly like the reference implementation's rng.int(). */
uint32_t jpeglitch_mt19937_range(jpeglitch_mt19937 *rng, uint32_t lo, uint32_t hi);

#ifdef __cplusplus
}
#endif

#endif /* JPEGLITCH_MT19937_H */
