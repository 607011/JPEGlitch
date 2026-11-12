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

---

Architecture notes (see jpeglitch-obs-prompt.md for the full spec):

* filter_video() is called by libobs's async_tick() on `source->cur_async_frame`,
  the frame picked from the source's own cache by closest timestamp. That same
  cached frame object can be handed to filter_video() again on a later tick if
  no newer capture frame has arrived yet (e.g. display refresh rate > capture
  frame rate). Mutating `frame`'s pixel buffer in place would therefore
  re-glitch an already-glitched frame on every repeat tick, compounding
  corruption instead of reapplying a clean one-shot glitch.
  -> This filter NEVER writes to the incoming `frame`. It only reads from it,
     and returns either its own, separately owned `out_frame` (the glitched
     result) or the original `frame` pointer unmodified (pass-through). This
     also makes the "last good result, else pass through" fallback trivial:
     since `frame` is never touched, pass-through is just returning it as-is.
  -> `out_frame->timestamp` is compared against the incoming frame's
     timestamp: if they match, the exact same source frame was already
     glitched on a previous tick, and the cached `out_frame` is returned
     without redoing any work.

* update() (settings/UI thread) and filter_video() (video thread) can run
  concurrently. `param_mutex` protects the `jpeglitch_params` snapshot; everything
  else (`state`, TurboJPEG handles, scratch buffers, `out_frame`) is only ever
  touched from filter_video() on the video thread and needs no locking.
*/

#include <obs-module.h>
#include <util/platform.h>
#include <util/threading.h>

#include <turbojpeg.h>

#include "jpeglitch-filter.h"
#include "jpeglitch_core.h"
#include "plugin-support.h"

#define S_START "start"
#define S_DEFECTS "defects"
#define S_QUALITY "quality"
#define S_ALGORITHM "algorithm"
#define S_FIXED_SEED "fixed_seed"
#define S_SEED "seed"
#define S_PREVENT_FF "prevent_ff"
#define S_LEAVE_FF "leave_ff"
#define S_PATTERN_HOLD "pattern_hold"

#define ALG_XOR "xor"
#define ALG_ONE "one"
#define ALG_ZERO "zero"

/* All filter-level settings, i.e. the core glitch parameters plus the
 * JPEG quality and the "new pattern every N frames" setting (the latter two
 * are filter/codec concerns, not part of the OBS-free glitch core). */
struct jpeglitch_filter_params {
	jpeglitch_params core;
	int quality; /* already clamped to TurboJPEG's 1..100 */
	int pattern_hold_frames;
};

struct jpeglitch_filter {
	obs_source_t *source;

	/* Protected by param_mutex; written only by jpeglitch_filter_update()
	 * (settings thread), read only by jpeglitch_filter_video() (video
	 * thread), which takes a cheap snapshot at the start of every call. */
	pthread_mutex_t param_mutex;
	struct jpeglitch_filter_params params;

	/* Video-thread-only below: libobs serializes filter_video() calls for
	 * a given filter instance, so none of this needs locking. */
	jpeglitch_state state;
	jpeglitch_mt19937 pattern_snapshot_rng;
	int pattern_counter;

	tjhandle enc;
	tjhandle dec;

	uint32_t cur_w, cur_h;
	enum video_format cur_format;
	uint8_t *chroma_a; /* de-interleaved U (encode) / decoded U (decode), size chroma_capacity */
	uint8_t *chroma_b; /* same for V */
	size_t chroma_capacity;
	uint8_t *luma_scratch; /* de-interleaved Y for packed formats (UYVY); unused for planar-Y formats (NV12) */
	size_t luma_capacity;

	uint8_t *jpeg_buf; /* allocated/reallocated by TurboJPEG itself (tj3Alloc); free with tj3Free */
	size_t jpeg_size;

	struct obs_source_frame *out_frame; /* owned; reused while cur_w/cur_h don't change */
	bool have_output;
	uint64_t last_input_timestamp;

	bool logged_unsupported_format;
};

static const char *jpeglitch_filter_get_name(void *type_data)
{
	UNUSED_PARAMETER(type_data);
	return obs_module_text("JPEGlitch.FilterName");
}

static jpeglitch_algorithm algorithm_from_string(const char *s)
{
	if (s && strcmp(s, ALG_ONE) == 0)
		return JPEGLITCH_ALG_ONE;
	if (s && strcmp(s, ALG_ZERO) == 0)
		return JPEGLITCH_ALG_ZERO;
	return JPEGLITCH_ALG_XOR;
}

static void jpeglitch_filter_read_settings(struct jpeglitch_filter_params *params, obs_data_t *settings)
{
	double start_percent = obs_data_get_double(settings, S_START);
	params->core.start = start_percent / 100.0;
	params->core.defects = (int)obs_data_get_int(settings, S_DEFECTS);

	/* TurboJPEG's TJPARAM_QUALITY only accepts 1..100, not the 0..100
	 * range the project spec's UI exposes; clamp at the boundary instead
	 * of at the OBS property (same deviation as in tools/jpeglitch-cli). */
	long long quality = obs_data_get_int(settings, S_QUALITY);
	if (quality < 1)
		quality = 1;
	if (quality > 100)
		quality = 100;
	params->quality = (int)quality;

	params->core.algorithm = algorithm_from_string(obs_data_get_string(settings, S_ALGORITHM));
	params->core.fixed_seed = obs_data_get_bool(settings, S_FIXED_SEED);
	params->core.seed = (uint32_t)obs_data_get_int(settings, S_SEED);
	params->core.prevent_ff = obs_data_get_bool(settings, S_PREVENT_FF);
	params->core.leave_ff = obs_data_get_bool(settings, S_LEAVE_FF);

	int hold = (int)obs_data_get_int(settings, S_PATTERN_HOLD);
	if (hold < 1)
		hold = 1;
	if (hold > 300)
		hold = 300;
	params->pattern_hold_frames = hold;
}

static void jpeglitch_filter_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_double(settings, S_START, 0.0);
	obs_data_set_default_int(settings, S_DEFECTS, 1);
	obs_data_set_default_int(settings, S_QUALITY, 50);
	obs_data_set_default_string(settings, S_ALGORITHM, ALG_ONE);
	obs_data_set_default_bool(settings, S_FIXED_SEED, false);
	obs_data_set_default_int(settings, S_SEED, 10000);
	obs_data_set_default_bool(settings, S_PREVENT_FF, true);
	obs_data_set_default_bool(settings, S_LEAVE_FF, true);
	obs_data_set_default_int(settings, S_PATTERN_HOLD, 1);
}

static bool jpeglitch_filter_fixed_seed_modified(obs_properties_t *props, obs_property_t *property,
						 obs_data_t *settings)
{
	UNUSED_PARAMETER(property);
	obs_property_t *seed_prop = obs_properties_get(props, S_SEED);
	obs_property_set_enabled(seed_prop, obs_data_get_bool(settings, S_FIXED_SEED));
	return true;
}

static obs_properties_t *jpeglitch_filter_get_properties(void *data)
{
	UNUSED_PARAMETER(data);
	obs_properties_t *props = obs_properties_create();

	obs_property_t *p;

	p = obs_properties_add_float_slider(props, S_START, obs_module_text("JPEGlitch.Start"), 0.0, 100.0, 0.01);
	obs_property_float_set_suffix(p, "%");

	obs_properties_add_int_slider(props, S_DEFECTS, obs_module_text("JPEGlitch.Defects"), 0, 200, 1);
	obs_properties_add_int_slider(props, S_QUALITY, obs_module_text("JPEGlitch.Quality"), 0, 100, 1);

	p = obs_properties_add_list(props, S_ALGORITHM, obs_module_text("JPEGlitch.Algorithm"), OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(p, obs_module_text("JPEGlitch.Algorithm.Xor"), ALG_XOR);
	obs_property_list_add_string(p, obs_module_text("JPEGlitch.Algorithm.One"), ALG_ONE);
	obs_property_list_add_string(p, obs_module_text("JPEGlitch.Algorithm.Zero"), ALG_ZERO);

	p = obs_properties_add_bool(props, S_FIXED_SEED, obs_module_text("JPEGlitch.FixedSeed"));
	obs_property_set_modified_callback(p, jpeglitch_filter_fixed_seed_modified);

	obs_properties_add_int(props, S_SEED, obs_module_text("JPEGlitch.Seed"), 0, 100000000, 1);

	obs_properties_add_bool(props, S_PREVENT_FF, obs_module_text("JPEGlitch.PreventFF"));
	obs_properties_add_bool(props, S_LEAVE_FF, obs_module_text("JPEGlitch.LeaveFF"));

	obs_properties_add_int(props, S_PATTERN_HOLD, obs_module_text("JPEGlitch.PatternHold"), 1, 300, 1);

	return props;
}

static void jpeglitch_filter_update(void *data, obs_data_t *settings)
{
	struct jpeglitch_filter *filter = data;

	struct jpeglitch_filter_params params;
	jpeglitch_filter_read_settings(&params, settings);

	pthread_mutex_lock(&filter->param_mutex);
	filter->params = params;
	pthread_mutex_unlock(&filter->param_mutex);
}

static void *jpeglitch_filter_create(obs_data_t *settings, obs_source_t *source)
{
	struct jpeglitch_filter *filter = bzalloc(sizeof(struct jpeglitch_filter));
	filter->source = source;
	pthread_mutex_init(&filter->param_mutex, NULL);

	filter->enc = tj3Init(TJINIT_COMPRESS);
	filter->dec = tj3Init(TJINIT_DECOMPRESS);
	if (filter->dec)
		tj3Set(filter->dec, TJPARAM_STOPONWARNING, 0);

	/* Seed the free-running RNG from a value that differs between filter
	 * instances/sessions; fixed-seed mode overrides this every frame
	 * anyway. */
	jpeglitch_state_init(&filter->state, (uint32_t)(uintptr_t)filter ^ (uint32_t)os_gettime_ns());

	jpeglitch_filter_update(filter, settings);

	return filter;
}

static void jpeglitch_filter_destroy(void *data)
{
	struct jpeglitch_filter *filter = data;

	if (filter->enc)
		tj3Destroy(filter->enc);
	if (filter->dec)
		tj3Destroy(filter->dec);
	if (filter->out_frame)
		obs_source_frame_destroy(filter->out_frame);
	bfree(filter->chroma_a);
	bfree(filter->chroma_b);
	bfree(filter->luma_scratch);
	if (filter->jpeg_buf)
		tj3Free(filter->jpeg_buf);
	pthread_mutex_destroy(&filter->param_mutex);
	bfree(filter);
}

/* NV12 is 4:2:0 (chroma subsampled both ways); UYVY is 4:2:2 (chroma
 * subsampled horizontally only). Returns -1 for an unsupported format. */
static int subsamp_for_format(enum video_format format)
{
	switch (format) {
	case VIDEO_FORMAT_NV12:
		return TJSAMP_420;
	case VIDEO_FORMAT_UYVY:
		return TJSAMP_422;
	default:
		return -1;
	}
}

static bool ensure_buffers(struct jpeglitch_filter *filter, enum video_format format, uint32_t w, uint32_t h)
{
	if (filter->out_frame && filter->cur_format == format && filter->cur_w == w && filter->cur_h == h)
		return true;

	if (filter->out_frame) {
		obs_source_frame_destroy(filter->out_frame);
		filter->out_frame = NULL;
	}
	filter->out_frame = obs_source_frame_create(format, w, h);
	if (!filter->out_frame)
		return false;

	int subsamp = subsamp_for_format(format);
	int cw = tjPlaneWidth(1, (int)w, subsamp);
	int ch = tjPlaneHeight(1, (int)h, subsamp);
	size_t needed = (size_t)cw * (size_t)ch;
	if (needed > filter->chroma_capacity) {
		filter->chroma_a = brealloc(filter->chroma_a, needed);
		filter->chroma_b = brealloc(filter->chroma_b, needed);
		filter->chroma_capacity = needed;
	}

	/* Only packed formats (UYVY) need a separate Y scratch buffer; planar
	 * formats (NV12) read/write Y directly from/to the frame's own plane. */
	if (format == VIDEO_FORMAT_UYVY) {
		size_t luma_needed = (size_t)w * (size_t)h;
		if (luma_needed > filter->luma_capacity) {
			filter->luma_scratch = brealloc(filter->luma_scratch, luma_needed);
			filter->luma_capacity = luma_needed;
		}
	}

	filter->cur_format = format;
	filter->cur_w = w;
	filter->cur_h = h;
	filter->have_output = false;
	return true;
}

static void deinterleave_nv12_uv(const uint8_t *uv, uint32_t uv_linesize, int cw, int ch, uint8_t *u_out,
				 uint8_t *v_out)
{
	for (int y = 0; y < ch; ++y) {
		const uint8_t *row = uv + (size_t)y * uv_linesize;
		uint8_t *ur = u_out + (size_t)y * cw;
		uint8_t *vr = v_out + (size_t)y * cw;
		for (int x = 0; x < cw; ++x) {
			ur[x] = row[2 * x];
			vr[x] = row[2 * x + 1];
		}
	}
}

static void interleave_nv12_uv(const uint8_t *u_in, const uint8_t *v_in, int cw, int ch, uint8_t *uv,
			       uint32_t uv_linesize)
{
	for (int y = 0; y < ch; ++y) {
		const uint8_t *ur = u_in + (size_t)y * cw;
		const uint8_t *vr = v_in + (size_t)y * cw;
		uint8_t *row = uv + (size_t)y * uv_linesize;
		for (int x = 0; x < cw; ++x) {
			row[2 * x] = ur[x];
			row[2 * x + 1] = vr[x];
		}
	}
}

/* UYVY: packed 4:2:2, 2 bytes/pixel, groups of 4 bytes covering 2 pixels as
 * U Y0 V Y1. `w` must be even (true for every realistic capture width). */
static void deinterleave_uyvy(const uint8_t *uyvy, uint32_t linesize, int w, int h, uint8_t *y_out, uint8_t *u_out,
			      uint8_t *v_out)
{
	int cw = w / 2;
	for (int y = 0; y < h; ++y) {
		const uint8_t *row = uyvy + (size_t)y * linesize;
		uint8_t *yr = y_out + (size_t)y * w;
		uint8_t *ur = u_out + (size_t)y * cw;
		uint8_t *vr = v_out + (size_t)y * cw;
		for (int x2 = 0; x2 < cw; ++x2) {
			const uint8_t *px = row + x2 * 4;
			ur[x2] = px[0];
			yr[2 * x2] = px[1];
			vr[x2] = px[2];
			yr[2 * x2 + 1] = px[3];
		}
	}
}

static void interleave_uyvy(const uint8_t *y_in, const uint8_t *u_in, const uint8_t *v_in, int w, int h, uint8_t *uyvy,
			    uint32_t linesize)
{
	int cw = w / 2;
	for (int y = 0; y < h; ++y) {
		const uint8_t *yr = y_in + (size_t)y * w;
		const uint8_t *ur = u_in + (size_t)y * cw;
		const uint8_t *vr = v_in + (size_t)y * cw;
		uint8_t *row = uyvy + (size_t)y * linesize;
		for (int x2 = 0; x2 < cw; ++x2) {
			uint8_t *px = row + x2 * 4;
			px[0] = ur[x2];
			px[1] = yr[2 * x2];
			px[2] = vr[x2];
			px[3] = yr[2 * x2 + 1];
		}
	}
}

/* Applies the glitch core, honoring the "new pattern every N frames"
 * property: while not using a fixed seed, the RNG is snapshotted every Nth
 * frame and restored on the frames in between, so the exact same sequence of
 * random draws -- and therefore the same defect positions -- is produced
 * until the next snapshot. With a fixed seed, jpeglitch_glitch() already
 * reseeds every call, making this a no-op. */
static void glitch_with_pattern_hold(struct jpeglitch_filter *filter, uint8_t *raw, size_t n,
				     const struct jpeglitch_filter_params *params)
{
	if (!params->core.fixed_seed) {
		if (filter->pattern_counter == 0) {
			filter->pattern_snapshot_rng = filter->state.rng;
		} else {
			filter->state.rng = filter->pattern_snapshot_rng;
		}
		int hold = params->pattern_hold_frames > 0 ? params->pattern_hold_frames : 1;
		filter->pattern_counter = (filter->pattern_counter + 1) % hold;
	}

	jpeglitch_glitch(raw, n, &params->core, &filter->state);
}

/* Encodes frame's Y/UV planes to JPEG, glitches the bitstream, and decodes
 * back into filter->out_frame. Returns true on success (including "succeeded
 * despite a decode warning", expected for glitched data); false only on a
 * fatal encode/decode error, in which case out_frame's content is unchanged
 * (the caller falls back to the last good frame, or to pass-through). */
static bool process_nv12_frame(struct jpeglitch_filter *filter, const struct obs_source_frame *frame,
			       const struct jpeglitch_filter_params *params)
{
	int w = (int)frame->width, h = (int)frame->height;
	int cw = tjPlaneWidth(1, w, TJSAMP_420);
	int ch = tjPlaneHeight(1, h, TJSAMP_420);

	deinterleave_nv12_uv(frame->data[1], frame->linesize[1], cw, ch, filter->chroma_a, filter->chroma_b);

	const uint8_t *src_planes[3] = {frame->data[0], filter->chroma_a, filter->chroma_b};
	int src_strides[3] = {(int)frame->linesize[0], cw, cw};

	tj3Set(filter->enc, TJPARAM_QUALITY, params->quality);
	tj3Set(filter->enc, TJPARAM_SUBSAMP, TJSAMP_420);

	/* TurboJPEG tracks the destination buffer's real capacity internally
	 * as long as the same buffer pointer keeps being passed in, reusing
	 * (or growing) it as needed; the size we pass in is only meaningful
	 * the first time (buf == NULL). */
	if (tj3CompressFromYUVPlanes8(filter->enc, src_planes, w, src_strides, h, &filter->jpeg_buf,
				      &filter->jpeg_size) < 0) {
		obs_log(LOG_WARNING, "JPEG encode failed: %s", tj3GetErrorStr(filter->enc));
		return false;
	}

	glitch_with_pattern_hold(filter, filter->jpeg_buf, filter->jpeg_size, params);

	uint8_t *dst_planes[3] = {filter->out_frame->data[0], filter->chroma_a, filter->chroma_b};
	int dst_strides[3] = {(int)filter->out_frame->linesize[0], cw, cw};

	tj3Set(filter->dec, TJPARAM_STOPONWARNING, 0);
	if (tj3DecompressHeader(filter->dec, filter->jpeg_buf, filter->jpeg_size) < 0 &&
	    tj3GetErrorCode(filter->dec) == TJERR_FATAL) {
		obs_log(LOG_DEBUG, "glitched JPEG header decode failed fatally: %s", tj3GetErrorStr(filter->dec));
		return false;
	}
	if (tj3DecompressToYUVPlanes8(filter->dec, filter->jpeg_buf, filter->jpeg_size, dst_planes, dst_strides) < 0 &&
	    tj3GetErrorCode(filter->dec) == TJERR_FATAL) {
		obs_log(LOG_DEBUG, "glitched JPEG decode failed fatally: %s", tj3GetErrorStr(filter->dec));
		return false;
	}

	interleave_nv12_uv(filter->chroma_a, filter->chroma_b, cw, ch, filter->out_frame->data[1],
			   filter->out_frame->linesize[1]);
	return true;
}

/* Same as process_nv12_frame(), but for UYVY's packed 4:2:2 layout: Y has to
 * be de-/re-interleaved too (unlike NV12, where it's already a separate
 * plane), via filter->luma_scratch. */
static bool process_uyvy_frame(struct jpeglitch_filter *filter, const struct obs_source_frame *frame,
			       const struct jpeglitch_filter_params *params)
{
	int w = (int)frame->width, h = (int)frame->height;
	int cw = tjPlaneWidth(1, w, TJSAMP_422); /* 4:2:2 only subsamples horizontally, so chroma height == h */

	deinterleave_uyvy(frame->data[0], frame->linesize[0], w, h, filter->luma_scratch, filter->chroma_a,
			  filter->chroma_b);

	const uint8_t *src_planes[3] = {filter->luma_scratch, filter->chroma_a, filter->chroma_b};
	int src_strides[3] = {w, cw, cw};

	tj3Set(filter->enc, TJPARAM_QUALITY, params->quality);
	tj3Set(filter->enc, TJPARAM_SUBSAMP, TJSAMP_422);

	if (tj3CompressFromYUVPlanes8(filter->enc, src_planes, w, src_strides, h, &filter->jpeg_buf,
				      &filter->jpeg_size) < 0) {
		obs_log(LOG_WARNING, "JPEG encode failed: %s", tj3GetErrorStr(filter->enc));
		return false;
	}

	glitch_with_pattern_hold(filter, filter->jpeg_buf, filter->jpeg_size, params);

	uint8_t *dst_planes[3] = {filter->luma_scratch, filter->chroma_a, filter->chroma_b};
	int dst_strides[3] = {w, cw, cw};

	tj3Set(filter->dec, TJPARAM_STOPONWARNING, 0);
	if (tj3DecompressHeader(filter->dec, filter->jpeg_buf, filter->jpeg_size) < 0 &&
	    tj3GetErrorCode(filter->dec) == TJERR_FATAL) {
		obs_log(LOG_DEBUG, "glitched JPEG header decode failed fatally: %s", tj3GetErrorStr(filter->dec));
		return false;
	}
	if (tj3DecompressToYUVPlanes8(filter->dec, filter->jpeg_buf, filter->jpeg_size, dst_planes, dst_strides) < 0 &&
	    tj3GetErrorCode(filter->dec) == TJERR_FATAL) {
		obs_log(LOG_DEBUG, "glitched JPEG decode failed fatally: %s", tj3GetErrorStr(filter->dec));
		return false;
	}

	interleave_uyvy(filter->luma_scratch, filter->chroma_a, filter->chroma_b, w, h, filter->out_frame->data[0],
			filter->out_frame->linesize[0]);
	return true;
}

static struct obs_source_frame *jpeglitch_filter_video(void *data, struct obs_source_frame *frame)
{
	struct jpeglitch_filter *filter = data;
	if (!frame)
		return frame;

	if (subsamp_for_format(frame->format) < 0) {
		if (!filter->logged_unsupported_format) {
			obs_log(LOG_WARNING, "unsupported pixel format %d, passing frames through unmodified",
				(int)frame->format);
			filter->logged_unsupported_format = true;
		}
		return frame;
	}

	/* Same cached frame as last tick -> already glitched, return the
	 * cached result instead of corrupting it further. */
	if (filter->have_output && frame->timestamp == filter->last_input_timestamp)
		return filter->out_frame;

	struct jpeglitch_filter_params params;
	pthread_mutex_lock(&filter->param_mutex);
	params = filter->params;
	pthread_mutex_unlock(&filter->param_mutex);

	if (!ensure_buffers(filter, frame->format, frame->width, frame->height))
		return frame;

	bool ok = frame->format == VIDEO_FORMAT_NV12 ? process_nv12_frame(filter, frame, &params)
						     : process_uyvy_frame(filter, frame, &params);
	if (!ok)
		return filter->have_output ? filter->out_frame : frame;

	/* Metadata that isn't part of the YUV content itself stays untouched,
	 * per spec: only the pixel content goes through the codec. */
	filter->out_frame->timestamp = frame->timestamp;
	filter->out_frame->flip = frame->flip;
	filter->out_frame->flags = frame->flags;
	filter->out_frame->full_range = frame->full_range;
	filter->out_frame->trc = frame->trc;
	filter->out_frame->max_luminance = frame->max_luminance;
	memcpy(filter->out_frame->color_matrix, frame->color_matrix, sizeof(frame->color_matrix));
	memcpy(filter->out_frame->color_range_min, frame->color_range_min, sizeof(frame->color_range_min));
	memcpy(filter->out_frame->color_range_max, frame->color_range_max, sizeof(frame->color_range_max));

	filter->last_input_timestamp = frame->timestamp;
	filter->have_output = true;
	return filter->out_frame;
}

static struct obs_source_info jpeglitch_filter_source_info = {
	.id = "jpeglitch_filter",
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_ASYNC,
	.get_name = jpeglitch_filter_get_name,
	.create = jpeglitch_filter_create,
	.destroy = jpeglitch_filter_destroy,
	.get_defaults = jpeglitch_filter_get_defaults,
	.get_properties = jpeglitch_filter_get_properties,
	.update = jpeglitch_filter_update,
	.filter_video = jpeglitch_filter_video,
};

struct obs_source_info *jpeglitch_filter_info(void)
{
	return &jpeglitch_filter_source_info;
}
