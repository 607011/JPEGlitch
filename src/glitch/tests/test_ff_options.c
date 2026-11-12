#include <string.h>

#include "jpeglitch_core.h"
#include "test_util.h"

#define N 4096

static void make_input_no_ff(uint8_t *buf)
{
	for (size_t i = 0; i < N; ++i)
		buf[i] = (uint8_t)((i * 29 + 5) & 0x7f); /* never 0xff, never 0xda by construction */
	buf[16] = 0xff;
	buf[17] = 0xda; /* SOS marker */
}

static void make_input_with_ff(uint8_t *buf)
{
	for (size_t i = 0; i < N; ++i)
		buf[i] = (i % 10 == 0) ? 0xff : (uint8_t)((i * 17 + 1) & 0x7f);
	buf[16] = 0xff;
	buf[17] = 0xda;
}

int main(void)
{
	uint8_t raw[N];

	/* preventFF: with algorithm ONE (bit-set) and many hits, no byte may
	 * ever end up as 0xff in the touched range. */
	make_input_no_ff(raw);
	jpeglitch_params prevent_params = {
		.start = 0.0,
		.defects = 2000,
		.algorithm = JPEGLITCH_ALG_ONE,
		.fixed_seed = true,
		.seed = 55u,
		.prevent_ff = true,
		.leave_ff = false,
	};
	jpeglitch_state state;
	jpeglitch_state_init(&state, 1u);
	jpeglitch_glitch(raw, N, &prevent_params, &state);
	for (size_t i = 18; i < N - 3; ++i)
		CHECK(raw[i] != 0xff);

	/* leaveFF: bytes that are already 0xff before touching them must stay
	 * untouched forever, even under aggressive ZERO-algorithm settings. */
	make_input_with_ff(raw);
	uint8_t original_ff_mask[N];
	for (size_t i = 0; i < N; ++i)
		original_ff_mask[i] = (raw[i] == 0xff) ? 1 : 0;

	jpeglitch_params leave_params = {
		.start = 0.0,
		.defects = 2000,
		.algorithm = JPEGLITCH_ALG_ZERO,
		.fixed_seed = true,
		.seed = 77u,
		.prevent_ff = false,
		.leave_ff = true,
	};
	jpeglitch_state_init(&state, 1u);
	jpeglitch_glitch(raw, N, &leave_params, &state);
	for (size_t i = 18; i < N - 3; ++i) {
		if (original_ff_mask[i])
			CHECK(raw[i] == 0xff);
	}

	TEST_EXIT();
}
