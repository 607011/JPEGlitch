#include "jpeglitch_mt19937.h"
#include "test_util.h"

int main(void)
{
	/* Standard MT19937 reference test vector: seed 5489 (the default seed)
	 * must produce 3499211612 as its first 32-bit output. */
	jpeglitch_mt19937 rng;
	jpeglitch_mt19937_seed(&rng, 5489u);
	CHECK(jpeglitch_mt19937_next(&rng) == 3499211612u);

	/* range(): bounds are inclusive. */
	jpeglitch_mt19937_seed(&rng, 1u);
	for (int i = 0; i < 1000; ++i) {
		uint32_t v = jpeglitch_mt19937_range(&rng, 10u, 20u);
		CHECK(v >= 10u && v <= 20u);
	}

	/* range(): a single-value range always returns that value. */
	CHECK(jpeglitch_mt19937_range(&rng, 42u, 42u) == 42u);

	/* range(): lo/hi are swapped when lo > hi, matching the reference's
	 * `if (lo > hi) [lo, hi] = [hi, lo];`. */
	jpeglitch_mt19937 a, b;
	jpeglitch_mt19937_seed(&a, 7u);
	jpeglitch_mt19937_seed(&b, 7u);
	uint32_t swapped = jpeglitch_mt19937_range(&a, 20u, 10u);
	uint32_t normal = jpeglitch_mt19937_range(&b, 10u, 20u);
	CHECK(swapped == normal);
	CHECK(swapped >= 10u && swapped <= 20u);

	TEST_EXIT();
}
