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

* filter_video() must return either the exact frame object it was given
  (mutated or not) or NULL. Returning a different, filter-owned frame
  object -- which an earlier version of this filter did, to avoid
  re-glitching an already-glitched frame on a repeat tick -- breaks
  libobs's own refcounting: `filter_frame()` in libobs/obs-source.c
  increments the *incoming* frame's refs before calling the filter chain
  and decrements whatever the chain *returns*. If that's a different
  object, the incoming frame's refcount is permanently bumped and never
  settles back down, which stops libobs from ever reclaiming that cache
  slot -- a severe, fast memory leak (confirmed live: multiple GB per
  minute). So: this filter mutates `frame`'s own pixel buffers in place
  and always returns `frame` itself.
  -> To still avoid double-glitching the same cached frame on a repeat
     tick (same object handed back by async_tick() before a new capture
     frame arrives), `last_processed_timestamp` remembers the timestamp
     of the frame we last mutated; if the incoming frame's timestamp
     matches, it's that same already-processed object, so we return it
     untouched instead of glitching it again.
  -> To still support "fatal decode error -> show the last good frame"
     (the new frame may come from a different, freshly-cached slot by
     then), we keep our own small fallback copy of the last successfully
     glitched planes (`last_good_*`) and write that into `frame` instead,
     rather than owning a separate `obs_source_frame`.

* update() (settings/UI thread) and filter_video() (video thread) can run
  concurrently. `param_mutex` protects the `jpeglitch_params` snapshot; everything
  else (`state`, TurboJPEG handles, scratch buffers) is only ever touched
  from filter_video() on the video thread and needs no locking.
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
	uint8_t *luma_scratch; /* de-interleaved/decoded Y scratch, size luma_capacity */
	size_t luma_capacity;

	/* A copy of the last successfully glitched planes, same layout/sizing
	 * as chroma_a/chroma_b/luma_scratch above, used to re-fill `frame` on
	 * a fatal encode/decode error instead of owning a separate frame. */
	uint8_t *last_good_luma;
	uint8_t *last_good_chroma_a;
	uint8_t *last_good_chroma_b;
	bool have_last_good;

	uint8_t *jpeg_buf; /* allocated/reallocated by TurboJPEG itself (tj3Alloc); free with tj3Free */
	size_t jpeg_size;

	bool last_processed_valid;
	uint64_t last_processed_timestamp;

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
	bfree(filter->chroma_a);
	bfree(filter->chroma_b);
	bfree(filter->luma_scratch);
	bfree(filter->last_good_chroma_a);
	bfree(filter->last_good_chroma_b);
	bfree(filter->last_good_luma);
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

/* Chroma plane dimensions for the formats this filter supports. */
static void chroma_dims(enum video_format format, int w, int h, int *cw, int *ch)
{
	int subsamp = subsamp_for_format(format);
	*cw = tjPlaneWidth(1, w, subsamp);
	*ch = tjPlaneHeight(1, h, subsamp);
}

static bool ensure_buffers(struct jpeglitch_filter *filter, enum video_format format, uint32_t w, uint32_t h)
{
	if (filter->cur_format == format && filter->cur_w == w && filter->cur_h == h)
		return true;

	int cw, ch;
	chroma_dims(format, (int)w, (int)h, &cw, &ch);
	size_t chroma_needed = (size_t)cw * (size_t)ch;
	if (chroma_needed > filter->chroma_capacity) {
		filter->chroma_a = brealloc(filter->chroma_a, chroma_needed);
		filter->chroma_b = brealloc(filter->chroma_b, chroma_needed);
		filter->last_good_chroma_a = brealloc(filter->last_good_chroma_a, chroma_needed);
		filter->last_good_chroma_b = brealloc(filter->last_good_chroma_b, chroma_needed);
		filter->chroma_capacity = chroma_needed;
	}

	size_t luma_needed = (size_t)w * (size_t)h;
	if (luma_needed > filter->luma_capacity) {
		filter->luma_scratch = brealloc(filter->luma_scratch, luma_needed);
		filter->last_good_luma = brealloc(filter->last_good_luma, luma_needed);
		filter->luma_capacity = luma_needed;
	}

	filter->cur_format = format;
	filter->cur_w = w;
	filter->cur_h = h;
	filter->have_last_good = false;
	filter->last_processed_valid = false;
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
 * back into scratch buffers. Returns true on success (including "succeeded
 * despite a decode warning", expected for glitched data); false only on a
 * fatal encode/decode error. Either way, `frame` itself is never touched
 * here -- the result lands in filter->luma_scratch/chroma_a/chroma_b, and
 * the caller decides whether/how to commit that into `frame`. */
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

	return true;
}

/* Writes luma/chroma_a/chroma_b (whichever triple is passed in -- the fresh
 * scratch buffers on success, or the cached last-good copy on fallback)
 * into frame's own planes, honoring its real format/linesize. */
static void commit_to_frame(struct obs_source_frame *frame, enum video_format format, const uint8_t *luma,
			    const uint8_t *chroma_a, const uint8_t *chroma_b)
{
	int w = (int)frame->width, h = (int)frame->height;
	int cw, ch;
	chroma_dims(format, w, h, &cw, &ch);

	if (format == VIDEO_FORMAT_NV12) {
		for (int y = 0; y < h; ++y)
			memcpy(frame->data[0] + (size_t)y * frame->linesize[0], luma + (size_t)y * w, (size_t)w);
		interleave_nv12_uv(chroma_a, chroma_b, cw, ch, frame->data[1], frame->linesize[1]);
	} else {
		interleave_uyvy(luma, chroma_a, chroma_b, w, h, frame->data[0], frame->linesize[0]);
	}
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

	/* Same cached frame object as last tick -> we already mutated it in
	 * place on a previous call; return it as-is instead of glitching it
	 * again. */
	if (filter->last_processed_valid && frame->timestamp == filter->last_processed_timestamp)
		return frame;

	struct jpeglitch_filter_params params;
	pthread_mutex_lock(&filter->param_mutex);
	params = filter->params;
	pthread_mutex_unlock(&filter->param_mutex);

	if (!ensure_buffers(filter, frame->format, frame->width, frame->height))
		return frame;

	bool ok = frame->format == VIDEO_FORMAT_NV12 ? process_nv12_frame(filter, frame, &params)
						     : process_uyvy_frame(filter, frame, &params);

	int w = (int)frame->width, h = (int)frame->height;
	int cw, ch;
	chroma_dims(frame->format, w, h, &cw, &ch);

	if (ok) {
		commit_to_frame(frame, frame->format, filter->luma_scratch, filter->chroma_a, filter->chroma_b);

		/* Cache this result for the next fatal-error fallback. */
		memcpy(filter->last_good_luma, filter->luma_scratch, (size_t)w * (size_t)h);
		memcpy(filter->last_good_chroma_a, filter->chroma_a, (size_t)cw * (size_t)ch);
		memcpy(filter->last_good_chroma_b, filter->chroma_b, (size_t)cw * (size_t)ch);
		filter->have_last_good = true;
	} else if (filter->have_last_good) {
		commit_to_frame(frame, frame->format, filter->last_good_luma, filter->last_good_chroma_a,
				filter->last_good_chroma_b);
	}
	/* else: no glitched or cached-good result available yet -- leave
	 * frame's own (unglitched) content untouched, i.e. plain pass-through. */

	filter->last_processed_timestamp = frame->timestamp;
	filter->last_processed_valid = true;
	return frame;
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
