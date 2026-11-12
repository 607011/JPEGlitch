/* Codec robustness test: intentionally mangles encoded JPEGs with the glitch
 * core (extreme settings included) and verifies that decoding, via the same
 * fatal/warning fallback strategy the OBS filter will use, always yields
 * *some* valid image -- never a crash, never a missing/garbage result. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <turbojpeg.h>

#include "jpeglitch_codec.h"
#include "jpeglitch_core.h"

static int failures = 0;
#define CHECK(cond)                                                                         \
	do {                                                                                  \
		if (!(cond)) {                                                                \
			fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
			failures++;                                                              \
		}                                                                             \
	} while (0)

#define W 160
#define H 120

static uint8_t *make_source_jpeg(size_t *out_size)
{
	uint8_t *rgb = malloc((size_t)W * H * 3);
	for (int y = 0; y < H; ++y) {
		for (int x = 0; x < W; ++x) {
			uint8_t *p = rgb + ((size_t)y * W + x) * 3;
			p[0] = (uint8_t)(x * 255 / W);
			p[1] = (uint8_t)(y * 255 / H);
			p[2] = (uint8_t)((x + y) & 0xff);
		}
	}
	tjhandle enc = tj3Init(TJINIT_COMPRESS);
	tj3Set(enc, TJPARAM_QUALITY, 50);
	tj3Set(enc, TJPARAM_SUBSAMP, TJSAMP_420);
	uint8_t *jpeg = NULL;
	size_t size = 0;
	if (tj3Compress8(enc, rgb, W, 0, H, TJPF_RGB, &jpeg, &size) < 0) {
		fprintf(stderr, "setup failed: %s\n", tj3GetErrorStr(enc));
		exit(1);
	}
	tj3Destroy(enc);
	free(rgb);
	*out_size = size;
	return jpeg;
}

/* Mirrors the fallback strategy from the project spec: try to decode the
 * glitched buffer; on a fatal error, fall back to the last known-good
 * image (here: the un-glitched source). Always succeeds. */
static void decode_with_fallback(tjhandle dec, const uint8_t *glitched, size_t glitched_size, const uint8_t *good,
				 size_t good_size, uint8_t **buf, size_t *cap)
{
	int w, h;
	int rc = jpeglitch_codec_decode(dec, glitched, glitched_size, TJPF_RGB, buf, cap, &w, &h);
	if (rc < 0) {
		rc = jpeglitch_codec_decode(dec, good, good_size, TJPF_RGB, buf, cap, &w, &h);
		CHECK(rc >= 0);
	}
	CHECK(w == W && h == H);
	CHECK(*buf != NULL);
}

int main(void)
{
	size_t source_size = 0;
	uint8_t *source = make_source_jpeg(&source_size);

	tjhandle dec = tj3Init(TJINIT_DECOMPRESS);
	uint8_t *buf = NULL;
	size_t cap = 0;

	jpeglitch_state state;
	jpeglitch_state_init(&state, 1);

	/* A wide sweep of settings, including the extreme ones called out in
	 * the project's acceptance criteria (quality handled by the caller;
	 * here: start 0%/100%, max defects, both FF options off). */
	double starts[] = {0.0, 0.006, 0.5, 1.0};
	int defect_counts[] = {0, 1, 100, 200};
	jpeglitch_algorithm algorithms[] = {JPEGLITCH_ALG_XOR, JPEGLITCH_ALG_ONE, JPEGLITCH_ALG_ZERO};

	for (size_t si = 0; si < sizeof(starts) / sizeof(starts[0]); ++si) {
		for (size_t di = 0; di < sizeof(defect_counts) / sizeof(defect_counts[0]); ++di) {
			for (size_t ai = 0; ai < sizeof(algorithms) / sizeof(algorithms[0]); ++ai) {
				for (int prevent_leave = 0; prevent_leave < 4; ++prevent_leave) {
					uint8_t *raw = malloc(source_size);
					memcpy(raw, source, source_size);

					jpeglitch_params params = {
						.start = starts[si],
						.defects = defect_counts[di],
						.algorithm = algorithms[ai],
						.fixed_seed = true,
						.seed = (uint32_t)(si * 1000 + di * 10 + ai),
						.prevent_ff = (prevent_leave & 1) != 0,
						.leave_ff = (prevent_leave & 2) != 0,
					};
					jpeglitch_result res = jpeglitch_glitch(raw, source_size, &params, &state);
					CHECK(res == JPEGLITCH_OK);

					decode_with_fallback(dec, raw, source_size, source, source_size, &buf, &cap);

					free(raw);
				}
			}
		}
	}

	free(buf);
	free(source);
	tj3Destroy(dec);

	if (failures)
		fprintf(stderr, "%d check(s) failed\n", failures);
	return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
