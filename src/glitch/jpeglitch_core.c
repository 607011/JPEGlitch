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

#include "jpeglitch_core.h"

#include <math.h>

void jpeglitch_state_init(jpeglitch_state *state, uint32_t initial_seed)
{
	jpeglitch_mt19937_seed(&state->rng, initial_seed);
}

size_t jpeglitch_find_header_size(const uint8_t *data, size_t n)
{
	if (n < 2)
		return 0;
	for (size_t i = 0; i + 1 < n; ++i) {
		if (data[i] == 0xff && data[i + 1] == 0xda)
			return i + 2;
	}
	return 0;
}

uint8_t jpeglitch_apply_algorithm(uint8_t old_byte, uint8_t bit, jpeglitch_algorithm algorithm)
{
	switch (algorithm) {
	case JPEGLITCH_ALG_ONE:
		return (uint8_t)((old_byte | bit) & 0xff);
	case JPEGLITCH_ALG_ZERO:
		return (uint8_t)((old_byte & (uint8_t)~bit) & 0xff);
	case JPEGLITCH_ALG_XOR:
	default:
		return (uint8_t)((old_byte ^ bit) & 0xff);
	}
}

static uint8_t jpeglitch_clamped_byte(uint8_t old_byte, uint8_t new_byte, bool prevent_ff)
{
	return (prevent_ff && new_byte == 0xff) ? old_byte : new_byte;
}

jpeglitch_result jpeglitch_glitch(uint8_t *raw, size_t n, const jpeglitch_params *params, jpeglitch_state *state)
{
	if (!raw || n < 3)
		return JPEGLITCH_ERR_TOO_SMALL;

	double start = params->start;
	if (start < 0.0)
		start = 0.0;
	else if (start > 1.0)
		start = 1.0;

	size_t header_size = jpeglitch_find_header_size(raw, n);
	size_t max_pos = n - 3;
	size_t offset = (size_t)floor((double)(n - header_size) * start);
	size_t first_pos = header_size + offset;
	if (first_pos > max_pos)
		first_pos = max_pos;

	if (params->fixed_seed)
		jpeglitch_mt19937_seed(&state->rng, params->seed);

	for (int i = 0; i < params->defects; ++i) {
		uint32_t pos = jpeglitch_mt19937_range(&state->rng, (uint32_t)first_pos, (uint32_t)max_pos);
		uint8_t old_byte = raw[pos];
		if (params->leave_ff && old_byte == 0xff)
			continue;
		uint8_t bit = (uint8_t)(1u << (jpeglitch_mt19937_next(&state->rng) % 8));
		uint8_t new_byte = jpeglitch_apply_algorithm(old_byte, bit, params->algorithm);
		raw[pos] = jpeglitch_clamped_byte(old_byte, new_byte, params->prevent_ff);
	}

	return JPEGLITCH_OK;
}
