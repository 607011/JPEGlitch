#include <string.h>

#include "jpeglitch_core.h"
#include "test_util.h"

#define N 4096

static void make_input(uint8_t *buf)
{
	for (size_t i = 0; i < N; ++i)
		buf[i] = (uint8_t)((i * 37 + 11) & 0xff);
	/* SOS marker somewhere in the first quarter, avoid the generated 0xFF/0xDA pattern elsewhere. */
	buf[64] = 0xff;
	buf[65] = 0xda;
}

static void run(uint8_t *out, const jpeglitch_params *params)
{
	uint8_t in[N];
	make_input(in);
	memcpy(out, in, N);
	jpeglitch_state state;
	jpeglitch_state_init(&state, 123u);
	jpeglitch_glitch(out, N, params, &state);
}

int main(void)
{
	jpeglitch_params params = {
		.start = 0.1,
		.defects = 100,
		.algorithm = JPEGLITCH_ALG_XOR,
		.fixed_seed = true,
		.seed = 10000u,
		.prevent_ff = true,
		.leave_ff = true,
	};

	uint8_t out1[N], out2[N];
	run(out1, &params);
	run(out2, &params);
	CHECK(memcmp(out1, out2, N) == 0);

	/* A different fixed seed must (almost certainly) produce a different result. */
	jpeglitch_params params2 = params;
	params2.seed = 999999u;
	uint8_t out3[N];
	run(out3, &params2);
	CHECK(memcmp(out1, out3, N) != 0);

	/* Without a fixed seed, consecutive calls on the same persistent state
	 * must NOT repeat (the generator keeps running). */
	uint8_t in[N];
	make_input(in);
	jpeglitch_params free_running = params;
	free_running.fixed_seed = false;
	jpeglitch_state state;
	jpeglitch_state_init(&state, 42u);
	uint8_t a[N], b[N];
	memcpy(a, in, N);
	jpeglitch_glitch(a, N, &free_running, &state);
	memcpy(b, in, N);
	jpeglitch_glitch(b, N, &free_running, &state);
	CHECK(memcmp(a, b, N) != 0);

	TEST_EXIT();
}
