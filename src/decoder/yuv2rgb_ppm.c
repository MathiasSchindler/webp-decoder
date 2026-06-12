#include "yuv2rgb_ppm.h"

#include <errno.h>
#ifndef NO_LIBC
#include <stdio.h>
#endif
#include <stdlib.h>
#include <string.h>

#include "../common/fmt.h"
#include "../common/os.h"
#include "../vp8/vp8_yuv_rgb.h"

static void upsample_rgb_line_pair(const uint8_t* top_y, const uint8_t* bottom_y, const uint8_t* top_u,
							   const uint8_t* top_v, const uint8_t* cur_u, const uint8_t* cur_v,
							   uint8_t* top_dst, uint8_t* bottom_dst, uint32_t len) {
	vp8_upsample_rgb_line_pair(top_y, bottom_y, top_u, top_v, cur_u, cur_v, top_dst, bottom_dst, len);
}

int yuv420_write_ppm_fd(int fd, const Yuv420Image* img) {
	if (fd < 0 || !img || !img->y || !img->u || !img->v) {
		errno = EINVAL;
		return -1;
	}
	if (img->width == 0 || img->height == 0) {
		errno = EINVAL;
		return -1;
	}
	if (img->width > UINT32_MAX / 3u) {
		errno = EFBIG;
		return -1;
	}
	const uint32_t row_bytes = img->width * 3u;

#ifdef NO_LIBC
	// Avoid stdio/snprintf in the no-libc build.
	if (os_write_all(fd, "P6\n", 3) != 0) return -1;
	fmt_write_u32(fd, img->width);
	if (os_write_all(fd, " ", 1) != 0) return -1;
	fmt_write_u32(fd, img->height);
	if (os_write_all(fd, "\n255\n", 5) != 0) return -1;
#else
	char header[64];
	int n = snprintf(header, sizeof(header), "P6\n%u %u\n255\n", img->width, img->height);
	if (n <= 0 || (size_t)n >= sizeof(header)) {
		errno = EINVAL;
		return -1;
	}
	if (os_write_all(fd, header, (size_t)n) != 0) return -1;
#endif

	uint8_t* rows = (uint8_t*)malloc((size_t)row_bytes * 2u);
	if (!rows) {
		errno = ENOMEM;
		return -1;
	}
	uint8_t* top_row = rows;
	uint8_t* bottom_row = rows + row_bytes;

	const uint32_t cw = (img->width + 1u) >> 1;
	const uint32_t ch = (img->height + 1u) >> 1;
	(void)cw;

	// Row 0 is special-cased: mirror the chroma samples at boundary.
	{
		const uint8_t* y0 = img->y;
		const uint8_t* u0 = img->u;
		const uint8_t* v0 = img->v;
		upsample_rgb_line_pair(y0, NULL, u0, v0, u0, v0, top_row, NULL, img->width);
		if (os_write_all(fd, top_row, row_bytes) != 0) {
			free(rows);
			return -1;
		}
	}

	// Process pairs of rows (1,2), (3,4), ... like libwebp's fancy upsampler.
	for (uint32_t y = 1; y < img->height; y += 2u) {
		const uint8_t* top_y = img->y + (size_t)y * img->stride_y;
		const uint8_t* bottom_y = (y + 1u < img->height) ? (img->y + (size_t)(y + 1u) * img->stride_y) : NULL;

		const uint32_t top_cy = y >> 1;
		const uint32_t cur_cy = (top_cy + 1u < ch) ? (top_cy + 1u) : (ch - 1u);
		const uint8_t* top_u = img->u + (size_t)top_cy * img->stride_uv;
		const uint8_t* top_v = img->v + (size_t)top_cy * img->stride_uv;
		const uint8_t* cur_u = img->u + (size_t)cur_cy * img->stride_uv;
		const uint8_t* cur_v = img->v + (size_t)cur_cy * img->stride_uv;

		upsample_rgb_line_pair(top_y, bottom_y, top_u, top_v, cur_u, cur_v, top_row, bottom_row, img->width);
		if (bottom_y != NULL) {
			if (os_write_all(fd, top_row, (size_t)row_bytes * 2u) != 0) goto write_error;
		} else {
			if (os_write_all(fd, top_row, row_bytes) != 0) goto write_error;
		}
	}

	free(rows);
	return 0;

write_error:
	free(rows);
	return -1;
}
