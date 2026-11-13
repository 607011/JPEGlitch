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

#ifndef JPEGLITCH_CORE_H
#define JPEGLITCH_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "jpeglitch_mt19937.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum jpeglitch_algorithm {
	JPEGLITCH_ALG_XOR = 0,
	JPEGLITCH_ALG_ONE = 1,
	JPEGLITCH_ALG_ZERO = 2,
} jpeglitch_algorithm;

typedef enum jpeglitch_result {
	JPEGLITCH_OK = 0,
	JPEGLITCH_ERR_TOO_SMALL = 1, /* fewer than 3 bytes of input */
} jpeglitch_result;

typedef struct jpeglitch_params {
	double start; /* position in the bitstream, 0..1 */
	int defects;  /* number of manipulated bytes, >= 0 */
	jpeglitch_algorithm algorithm;
	bool fixed_seed; /* reseed the RNG with `seed` before every call */
	uint32_t seed;   /* only used when fixed_seed is true */
	bool prevent_ff; /* never let a manipulated byte become 0xFF */
	bool leave_ff;   /* never touch a byte that already is 0xFF */
} jpeglitch_params;

/* Persistent state carried across frames/calls: the RNG, which keeps running
 * when fixed_seed is off. */
typedef struct jpeglitch_state {
	jpeglitch_mt19937 rng;
} jpeglitch_state;

/* Seeds the carried-over RNG once (e.g. with a random value at startup, or
 * any fixed value for reproducible testing). */
void jpeglitch_state_init(jpeglitch_state *state, uint32_t initial_seed);

/* Finds the end of the JPEG header: the position right after the first
 * 0xFF 0xDA (SOS) marker, or 0 if none is found. */
size_t jpeglitch_find_header_size(const uint8_t *data, size_t n);

/* Applies `bit` to `old_byte` according to `algorithm`. */
uint8_t jpeglitch_apply_algorithm(uint8_t old_byte, uint8_t bit, jpeglitch_algorithm algorithm);

/* Glitches `raw[0..n)` in place, exactly as described in the jpeglitch-obs
 * project spec / reference/qlitch.html's glitchOnce(). The last two bytes
 * (EOI marker) are never touched. */
jpeglitch_result jpeglitch_glitch(uint8_t *raw, size_t n, const jpeglitch_params *params, jpeglitch_state *state);

#ifdef __cplusplus
}
#endif

#endif /* JPEGLITCH_CORE_H */
