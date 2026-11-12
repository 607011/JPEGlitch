/*
jpeglitch-obs - JPEG glitch filter for OBS Studio
Copyright (C) 2025 Oliver Lau <oliver.lau@gmail.com>

jpeglitch-cli: a small, OBS-free command line tool around the glitch core
library. Takes a JPEG file, applies the same glitch parameters the OBS
filter will expose, and writes both the glitched (and possibly corrupt)
JPEG bitstream and a decoded PNG preview. Useful to compare the native
core's output against the web reference (reference/qlitch.html).

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

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <turbojpeg.h>

#include "jpeglitch_codec.h"
#include "jpeglitch_core.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

static void usage(const char *argv0)
{
	fprintf(stderr,
		"Usage: %s <input.jpg> [options]\n"
		"\n"
		"Options:\n"
		"  -o, --out <prefix>      output file prefix (default: <input>-glitched)\n"
		"  --start <0..1>          position in the bitstream (default 0.006)\n"
		"  --defects <n>           number of manipulated bytes (default 100)\n"
		"  --quality <0..100>      re-encode input at this JPEG quality before glitching\n"
		"  --algorithm xor|one|zero  bit operation (default xor)\n"
		"  --seed <n>              fixed RNG seed (implies a reproducible run)\n"
		"  --no-prevent-ff         allow a glitched byte to become 0xff\n"
		"  --no-leave-ff           allow touching bytes that already are 0xff\n",
		argv0);
}

static int read_file(const char *path, uint8_t **out_data, size_t *out_size)
{
	FILE *f = fopen(path, "rb");
	if (!f) {
		fprintf(stderr, "error: cannot open '%s': %s\n", path, strerror(errno));
		return -1;
	}
	fseek(f, 0, SEEK_END);
	long size = ftell(f);
	if (size < 0) {
		fclose(f);
		return -1;
	}
	fseek(f, 0, SEEK_SET);
	uint8_t *data = malloc((size_t)size);
	if (!data) {
		fclose(f);
		return -1;
	}
	size_t read = fread(data, 1, (size_t)size, f);
	fclose(f);
	if (read != (size_t)size) {
		free(data);
		return -1;
	}
	*out_data = data;
	*out_size = (size_t)size;
	return 0;
}

static int write_file(const char *path, const uint8_t *data, size_t size)
{
	FILE *f = fopen(path, "wb");
	if (!f) {
		fprintf(stderr, "error: cannot write '%s': %s\n", path, strerror(errno));
		return -1;
	}
	size_t written = fwrite(data, 1, size, f);
	fclose(f);
	return written == size ? 0 : -1;
}

/* Thin wrapper around jpeglitch_codec_decode() that also logs TurboJPEG's error
 * string, for CLI diagnostics. Broken (glitched) data causing a warning is
 * expected and not an error; only a fatal decode failure is reported. */
static int decode_rgb(tjhandle handle, const uint8_t *jpeg, size_t jpeg_size, uint8_t **out_rgb, int *out_w, int *out_h)
{
	*out_rgb = NULL;
	int rc = jpeglitch_codec_decode(handle, jpeg, jpeg_size, TJPF_RGB, out_rgb, NULL, out_w, out_h);
	if (rc < 0) {
		fprintf(stderr, "decode failed: %s\n", tj3GetErrorStr(handle));
		free(*out_rgb);
		*out_rgb = NULL;
		return -1;
	}
	if (rc == 1)
		fprintf(stderr, "note: decode warning (expected for glitched data): %s\n", tj3GetErrorStr(handle));
	return 0;
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		usage(argv[0]);
		return 2;
	}
	if (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help")) {
		usage(argv[0]);
		return 0;
	}
	const char *input_path = argv[1];

	jpeglitch_params params = {
		.start = 0.006,
		.defects = 100,
		.algorithm = JPEGLITCH_ALG_XOR,
		.fixed_seed = false,
		.seed = 10000u,
		.prevent_ff = true,
		.leave_ff = true,
	};
	char out_prefix[4096] = {0};
	int reencode_quality = -1;

	for (int i = 2; i < argc; ++i) {
		const char *a = argv[i];
		if ((!strcmp(a, "-o") || !strcmp(a, "--out")) && i + 1 < argc) {
			snprintf(out_prefix, sizeof(out_prefix), "%s", argv[++i]);
		} else if (!strcmp(a, "--start") && i + 1 < argc) {
			params.start = atof(argv[++i]);
		} else if (!strcmp(a, "--defects") && i + 1 < argc) {
			params.defects = atoi(argv[++i]);
		} else if (!strcmp(a, "--quality") && i + 1 < argc) {
			reencode_quality = atoi(argv[++i]);
		} else if (!strcmp(a, "--algorithm") && i + 1 < argc) {
			const char *v = argv[++i];
			if (!strcmp(v, "one"))
				params.algorithm = JPEGLITCH_ALG_ONE;
			else if (!strcmp(v, "zero"))
				params.algorithm = JPEGLITCH_ALG_ZERO;
			else
				params.algorithm = JPEGLITCH_ALG_XOR;
		} else if (!strcmp(a, "--seed") && i + 1 < argc) {
			params.fixed_seed = true;
			params.seed = (uint32_t)strtoul(argv[++i], NULL, 10);
		} else if (!strcmp(a, "--no-prevent-ff")) {
			params.prevent_ff = false;
		} else if (!strcmp(a, "--no-leave-ff")) {
			params.leave_ff = false;
		} else if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
			usage(argv[0]);
			return 0;
		} else {
			fprintf(stderr, "error: unknown option '%s'\n", a);
			usage(argv[0]);
			return 2;
		}
	}

	if (out_prefix[0] == '\0') {
		snprintf(out_prefix, sizeof(out_prefix), "%s-glitched", input_path);
		char *dot = strrchr(out_prefix, '.');
		/* strip a trailing extension copied from input_path, if any */
		if (dot && dot > out_prefix + strlen(input_path) - 10)
			*dot = '\0';
	}

	uint8_t *input_data = NULL;
	size_t input_size = 0;
	if (read_file(input_path, &input_data, &input_size) != 0)
		return 1;

	tjhandle dec = tj3Init(TJINIT_DECOMPRESS);
	tjhandle enc = tj3Init(TJINIT_COMPRESS);
	if (!dec || !enc) {
		fprintf(stderr, "error: tj3Init failed\n");
		return 1;
	}

	uint8_t *source_jpeg = input_data;
	size_t source_size = input_size;
	uint8_t *reencoded = NULL;

	if (reencode_quality >= 0) {
		int w, h;
		uint8_t *rgb = NULL;
		if (decode_rgb(dec, input_data, input_size, &rgb, &w, &h) != 0) {
			fprintf(stderr, "error: could not decode input for re-encoding\n");
			return 1;
		}
		/* TurboJPEG's TJPARAM_QUALITY only accepts 1..100 (not 0..100 as the
		 * project spec's UI range suggests); clamp here, and clamp the same
		 * way in the OBS filter's "quality" property. */
		int tj_quality = reencode_quality < 1 ? 1 : (reencode_quality > 100 ? 100 : reencode_quality);
		tj3Set(enc, TJPARAM_QUALITY, tj_quality);
		tj3Set(enc, TJPARAM_SUBSAMP, TJSAMP_420);
		size_t jpeg_size = 0;
		if (tj3Compress8(enc, rgb, w, 0, h, TJPF_RGB, &reencoded, &jpeg_size) < 0) {
			fprintf(stderr, "error: re-encode failed: %s\n", tj3GetErrorStr(enc));
			free(rgb);
			return 1;
		}
		free(rgb);
		source_jpeg = reencoded;
		source_size = jpeg_size;
	}

	uint8_t *raw = malloc(source_size);
	if (!raw) {
		fprintf(stderr, "error: out of memory\n");
		return 1;
	}
	memcpy(raw, source_jpeg, source_size);

	jpeglitch_state state;
	/* No fixed seed requested -> seed once from a time-derived value so
	 * repeated CLI invocations don't all produce the exact same pattern. */
	jpeglitch_state_init(&state, params.fixed_seed ? params.seed : (uint32_t)time(NULL));

	size_t header_size = jpeglitch_find_header_size(raw, source_size);
	jpeglitch_result res = jpeglitch_glitch(raw, source_size, &params, &state);
	if (res != JPEGLITCH_OK) {
		fprintf(stderr, "error: input too small to glitch (%zu bytes, header at %zu)\n", source_size,
			header_size);
		return 1;
	}

	char jpg_path[4160], png_path[4160];
	snprintf(jpg_path, sizeof(jpg_path), "%s.jpg", out_prefix);
	snprintf(png_path, sizeof(png_path), "%s.png", out_prefix);

	if (write_file(jpg_path, raw, source_size) != 0)
		return 1;
	printf("wrote %s (%zu bytes)\n", jpg_path, source_size);

	int w = 0, h = 0;
	uint8_t *rgb = NULL;
	if (decode_rgb(dec, raw, source_size, &rgb, &w, &h) != 0) {
		fprintf(stderr,
			"warning: glitched JPEG could not be decoded at all, falling back to the un-glitched source\n");
		if (decode_rgb(dec, source_jpeg, source_size, &rgb, &w, &h) != 0) {
			fprintf(stderr, "error: fallback decode also failed\n");
			return 1;
		}
	}

	if (!stbi_write_png(png_path, w, h, 3, rgb, w * 3)) {
		fprintf(stderr, "error: could not write PNG '%s'\n", png_path);
		return 1;
	}
	printf("wrote %s (%dx%d)\n", png_path, w, h);

	free(rgb);
	free(raw);
	free(reencoded);
	free(input_data);
	tj3Destroy(dec);
	tj3Destroy(enc);
	return 0;
}
