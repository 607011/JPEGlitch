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

#ifndef JPEGLITCH_CODEC_H
#define JPEGLITCH_CODEC_H

#include <stddef.h>
#include <stdint.h>

#include <turbojpeg.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Decodes jpeg[0..jpeg_size) into a packed-pixel buffer, tolerating broken
 * (glitched) data: TJPARAM_STOPONWARNING is left off, and a TJERR_WARNING is
 * treated as success -- only a TJERR_FATAL error is reported as a real
 * failure (return -1). On success, *out points at a buffer of
 * w * h * tjPixelSize[pixel_format] bytes owned by the caller (free() it, or
 * reuse it across calls and pass its current capacity via *out_capacity to
 * avoid reallocating every frame; pass 0 to always (re)allocate fresh).
 *
 * Returns 0 on clean success, 1 if it succeeded despite a (non-fatal)
 * warning -- expected for glitched data -- or -1 on a fatal decode error
 * (out_w/out_h are left untouched in that case). */
int jpeglitch_codec_decode(tjhandle handle, const uint8_t *jpeg, size_t jpeg_size, int pixel_format, uint8_t **out,
			   size_t *out_capacity, int *out_w, int *out_h);

#ifdef __cplusplus
}
#endif

#endif /* JPEGLITCH_CODEC_H */
