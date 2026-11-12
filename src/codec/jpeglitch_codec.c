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

#include "jpeglitch_codec.h"

#include <stdbool.h>
#include <stdlib.h>

int jpeglitch_codec_decode(tjhandle handle, const uint8_t *jpeg, size_t jpeg_size, int pixel_format, uint8_t **out,
			   size_t *out_capacity, int *out_w, int *out_h)
{
	tj3Set(handle, TJPARAM_STOPONWARNING, 0);

	bool warned = false;
	if (tj3DecompressHeader(handle, jpeg, jpeg_size) < 0) {
		if (tj3GetErrorCode(handle) == TJERR_FATAL)
			return -1;
		warned = true;
	}

	int w = tj3Get(handle, TJPARAM_JPEGWIDTH);
	int h = tj3Get(handle, TJPARAM_JPEGHEIGHT);
	if (w <= 0 || h <= 0)
		return -1;

	size_t needed = (size_t)w * (size_t)h * (size_t)tjPixelSize[pixel_format];
	uint8_t *buf = *out;
	if (!buf || (out_capacity && *out_capacity < needed)) {
		buf = realloc(*out, needed);
		if (!buf)
			return -1;
	}

	int rc = tj3Decompress8(handle, jpeg, jpeg_size, buf, 0, pixel_format);
	*out = buf;
	if (out_capacity)
		*out_capacity = needed;
	if (rc < 0) {
		if (tj3GetErrorCode(handle) == TJERR_FATAL)
			return -1; /* out/out_capacity kept for reuse; caller must not read the pixels */
		warned = true;
	}

	*out_w = w;
	*out_h = h;
	return warned ? 1 : 0;
}
