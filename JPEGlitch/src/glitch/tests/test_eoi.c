#include <string.h>

#include "jpeglitch_core.h"
#include "test_util.h"

#define N 2048

static void make_input(uint8_t *buf)
{
	for (size_t i = 0; i < N; ++i)
		buf[i] = (uint8_t)((i * 53 + 3) & 0xff);
	buf[32] = 0xff;
	buf[33] = 0xda;
	buf[N - 2] = 0xff;
	buf[N - 1] = 0xd9; /* EOI */
}

int main(void)
{
	uint8_t raw[N];

	/* Extreme settings: start at the very end, max defects, no FF protection. */
	jpeglitch_params params = {
		.start = 1.0,
		.defects = 200,
		.algorithm = JPEGLITCH_ALG_XOR,
		.fixed_seed = true,
		.seed = 777u,
		.prevent_ff = false,
		.leave_ff = false,
	};

	make_input(raw);
	jpeglitch_state state;
	jpeglitch_state_init(&state, 1u);
	jpeglitch_glitch(raw, N, &params, &state);
	CHECK(raw[N - 2] == 0xff);
	CHECK(raw[N - 1] == 0xd9);

	/* Repeated calls on the same buffer: EOI still untouched. */
	for (int i = 0; i < 20; ++i)
		jpeglitch_glitch(raw, N, &params, &state);
	CHECK(raw[N - 2] == 0xff);
	CHECK(raw[N - 1] == 0xd9);

	TEST_EXIT();
}
