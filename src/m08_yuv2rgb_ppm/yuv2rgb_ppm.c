#include "yuv2rgb_ppm.h"

#include <errno.h>
#ifndef NO_LIBC
#include <stdio.h>
#endif
#include <stdlib.h>
#include <string.h>

#include "../common/fmt.h"
#include "../common/os.h"

// Bit-exact VP8/WebP YUV->RGB conversion (matches libwebp's VP8YuvToRgb).
enum {
	YUV_FIX2 = 6,
	YUV_MASK2 = (256 << YUV_FIX2) - 1
};

static inline int mult_hi(int v, int coeff) {
	// _mm_mulhi_epu16 emulation used by libwebp.
	return (v * coeff) >> 8;
}

static inline uint8_t vp8_clip8(int v) {
	// The (v & ~YUV_MASK2) fast-path is safe: it checks whether v is in [0, 255<<6].
	if ((v & ~YUV_MASK2) == 0) return (uint8_t)(v >> YUV_FIX2);
	return (v < 0) ? 0u : 255u;
}

static inline void vp8_yuv_to_rgb(uint8_t y, uint8_t u, uint8_t v, uint8_t* dst3) {
	// These coefficients bake in the (Y-16), (U-128), (V-128) offsets.
	const int Y = (int)y;
	const int U = (int)u;
	const int V = (int)v;
	const int r = mult_hi(Y, 19077) + mult_hi(V, 26149) - 14234;
	const int g = mult_hi(Y, 19077) - mult_hi(U, 6419) - mult_hi(V, 13320) + 8708;
	const int b = mult_hi(Y, 19077) + mult_hi(U, 33050) - 17685;
	dst3[0] = vp8_clip8(r);
	dst3[1] = vp8_clip8(g);
	dst3[2] = vp8_clip8(b);
}

// Fancy 4:2:0 upsampler (matches libwebp's DSP path):
// Given chroma samples laid out as:
//   [a b]
//   [c d]
// it interpolates the 2x2 luma chroma values as:
//   top:    ([9a+3b+3c+1d, 3a+9b+3c+1d] + 8) / 16
//   bottom: ([3a+1b+9c+3d, 1a+3b+3c+9d] + 8) / 16
static void upsample_rgb_line_pair(const uint8_t* top_y, const uint8_t* bottom_y, const uint8_t* top_u,
							   const uint8_t* top_v, const uint8_t* cur_u, const uint8_t* cur_v,
							   uint8_t* top_dst, uint8_t* bottom_dst, uint32_t len) {
	if (len == 0) return;

	const uint32_t last_pixel_pair = (len - 1u) >> 1;
	uint32_t tl_u = top_u[0];
	uint32_t tl_v = top_v[0];
	uint32_t l_u = cur_u[0];
	uint32_t l_v = cur_v[0];

	{
		const uint8_t u0 = (uint8_t)((3u * tl_u + l_u + 2u) >> 2);
		const uint8_t v0 = (uint8_t)((3u * tl_v + l_v + 2u) >> 2);
		vp8_yuv_to_rgb(top_y[0], u0, v0, top_dst + 0);
	}
	if (bottom_y != NULL) {
		const uint8_t u0 = (uint8_t)((3u * l_u + tl_u + 2u) >> 2);
		const uint8_t v0 = (uint8_t)((3u * l_v + tl_v + 2u) >> 2);
		vp8_yuv_to_rgb(bottom_y[0], u0, v0, bottom_dst + 0);
	}

	for (uint32_t x = 1; x <= last_pixel_pair; ++x) {
		const uint32_t t_u = top_u[x];
		const uint32_t t_v = top_v[x];
		const uint32_t u = cur_u[x];
		const uint32_t v = cur_v[x];

		const uint32_t avg_u = tl_u + t_u + l_u + u + 8u;
		const uint32_t avg_v = tl_v + t_v + l_v + v + 8u;
		const uint32_t diag_12_u = (avg_u + 2u * (t_u + l_u)) >> 3;
		const uint32_t diag_12_v = (avg_v + 2u * (t_v + l_v)) >> 3;
		const uint32_t diag_03_u = (avg_u + 2u * (tl_u + u)) >> 3;
		const uint32_t diag_03_v = (avg_v + 2u * (tl_v + v)) >> 3;

		{
			const uint8_t u0 = (uint8_t)((diag_12_u + tl_u) >> 1);
			const uint8_t v0 = (uint8_t)((diag_12_v + tl_v) >> 1);
			const uint8_t u1 = (uint8_t)((diag_03_u + t_u) >> 1);
			const uint8_t v1 = (uint8_t)((diag_03_v + t_v) >> 1);
			vp8_yuv_to_rgb(top_y[2u * x - 1u], u0, v0, top_dst + (2u * x - 1u) * 3u);
			vp8_yuv_to_rgb(top_y[2u * x + 0u], u1, v1, top_dst + (2u * x + 0u) * 3u);
		}
		if (bottom_y != NULL) {
			const uint8_t u0 = (uint8_t)((diag_03_u + l_u) >> 1);
			const uint8_t v0 = (uint8_t)((diag_03_v + l_v) >> 1);
			const uint8_t u1 = (uint8_t)((diag_12_u + u) >> 1);
			const uint8_t v1 = (uint8_t)((diag_12_v + v) >> 1);
			vp8_yuv_to_rgb(bottom_y[2u * x - 1u], u0, v0, bottom_dst + (2u * x - 1u) * 3u);
			vp8_yuv_to_rgb(bottom_y[2u * x + 0u], u1, v1, bottom_dst + (2u * x + 0u) * 3u);
		}

		tl_u = t_u;
		tl_v = t_v;
		l_u = u;
		l_v = v;
	}

	if ((len & 1u) == 0u) {
		const uint32_t idx = len - 1u;
		{
			const uint8_t u0 = (uint8_t)((3u * tl_u + l_u + 2u) >> 2);
			const uint8_t v0 = (uint8_t)((3u * tl_v + l_v + 2u) >> 2);
			vp8_yuv_to_rgb(top_y[idx], u0, v0, top_dst + idx * 3u);
		}
		if (bottom_y != NULL) {
			const uint8_t u0 = (uint8_t)((3u * l_u + tl_u + 2u) >> 2);
			const uint8_t v0 = (uint8_t)((3u * l_v + tl_v + 2u) >> 2);
			vp8_yuv_to_rgb(bottom_y[idx], u0, v0, bottom_dst + idx * 3u);
		}
	}
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

	uint8_t* top_row = (uint8_t*)malloc((size_t)img->width * 3u);
	uint8_t* bottom_row = (uint8_t*)malloc((size_t)img->width * 3u);
	if (!top_row || !bottom_row) {
		free(top_row);
		free(bottom_row);
		errno = ENOMEM;
		return -1;
	}

	const uint32_t cw = (img->width + 1u) >> 1;
	const uint32_t ch = (img->height + 1u) >> 1;
	(void)cw;

	// Row 0 is special-cased: mirror the chroma samples at boundary.
	{
		const uint8_t* y0 = img->y;
		const uint8_t* u0 = img->u;
		const uint8_t* v0 = img->v;
		upsample_rgb_line_pair(y0, NULL, u0, v0, u0, v0, top_row, NULL, img->width);
		if (os_write_all(fd, top_row, (size_t)img->width * 3u) != 0) {
			free(top_row);
			free(bottom_row);
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
		if (os_write_all(fd, top_row, (size_t)img->width * 3u) != 0) {
			free(top_row);
			free(bottom_row);
			return -1;
		}
		if (bottom_y != NULL) {
			if (os_write_all(fd, bottom_row, (size_t)img->width * 3u) != 0) {
				free(top_row);
				free(bottom_row);
				return -1;
			}
		}
	}

	free(top_row);
	free(bottom_row);
	return 0;
}
