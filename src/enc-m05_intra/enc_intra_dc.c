#include "enc_intra_dc.h"

#include "enc_transform.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

static uint8_t dc_value(const uint8_t* left, const uint8_t* top, int size, int round, int shift) {
	int dc = 0;
	if (top) {
		for (int j = 0; j < size; ++j) dc += top[j];
		if (left) {
			for (int j = 0; j < size; ++j) dc += left[j];
		} else {
			dc += dc;
		}
		dc = (dc + round) >> shift;
	} else if (left) {
		for (int j = 0; j < size; ++j) dc += left[j];
		dc += dc;
		dc = (dc + round) >> shift;
	} else {
		dc = 0x80;
	}
	if (dc < 0) dc = 0;
	if (dc > 255) dc = 255;
	return (uint8_t)dc;
}

static uint8_t load_clamped(const uint8_t* plane, uint32_t stride, uint32_t w, uint32_t h, uint32_t x, uint32_t y) {
	if (w == 0 || h == 0) return 0;
	if (x >= w) x = w - 1;
	if (y >= h) y = h - 1;
	return plane[(size_t)y * (size_t)stride + (size_t)x];
}

static void fill4x4_clamped(uint8_t out4x4[16],
                           const uint8_t* plane,
                           uint32_t stride,
                           uint32_t w,
                           uint32_t h,
                           uint32_t x0,
                           uint32_t y0) {
	for (uint32_t dy = 0; dy < 4; dy++) {
		for (uint32_t dx = 0; dx < 4; dx++) {
			out4x4[dy * 4 + dx] = load_clamped(plane, stride, w, h, x0 + dx, y0 + dy);
		}
	}
}

static void fill4x4_const(uint8_t out4x4[16], uint8_t v) {
	for (int i = 0; i < 16; i++) out4x4[i] = v;
}

static void store_i16le(uint8_t* dst, int16_t v) {
	uint16_t u = (uint16_t)v;
	dst[0] = (uint8_t)(u & 0xFFu);
	dst[1] = (uint8_t)((u >> 8) & 0xFFu);
}

int enc_vp8_dc_transformdump(const EncYuv420Image* yuv,
                            uint32_t mb_cols,
                            uint32_t mb_rows,
                            uint8_t** out_bytes,
                            size_t* out_size) {
	if (!out_bytes || !out_size) {
		errno = EINVAL;
		return -1;
	}
	*out_bytes = NULL;
	*out_size = 0;
	if (!yuv || !yuv->y || !yuv->u || !yuv->v || yuv->width == 0 || yuv->height == 0) {
		errno = EINVAL;
		return -1;
	}
	if (mb_cols == 0 || mb_rows == 0) {
		errno = EINVAL;
		return -1;
	}
	if (mb_cols > (1u << 20) || mb_rows > (1u << 20)) {
		errno = EOVERFLOW;
		return -1;
	}

	const uint32_t w = yuv->width;
	const uint32_t h = yuv->height;
	const uint32_t uv_w = (w + 1u) >> 1;
	const uint32_t uv_h = (h + 1u) >> 1;

	const uint64_t mb_total64 = (uint64_t)mb_cols * (uint64_t)mb_rows;
	if (mb_total64 == 0 || mb_total64 > (1u << 20)) {
		errno = EOVERFLOW;
		return -1;
	}
	const uint32_t mb_total = (uint32_t)mb_total64;

	// bytes per macroblock dump
	const size_t coeffs_per_mb = 16 + (16 * 16) + (4 * 16) + (4 * 16);
	const size_t bytes_per_mb = coeffs_per_mb * 2;
	if (mb_total > SIZE_MAX / bytes_per_mb) {
		errno = EOVERFLOW;
		return -1;
	}
	const size_t total_bytes = (size_t)mb_total * bytes_per_mb;
	uint8_t* buf = (uint8_t*)malloc(total_bytes);
	if (!buf) {
		errno = ENOMEM;
		return -1;
	}

	size_t off = 0;
	for (uint32_t mby = 0; mby < mb_rows; mby++) {
		for (uint32_t mbx = 0; mbx < mb_cols; mbx++) {
			const uint32_t x0 = mbx * 16u;
			const uint32_t y0 = mby * 16u;

			uint8_t top16[16];
			uint8_t left16[16];
			const uint8_t* top_ptr = NULL;
			const uint8_t* left_ptr = NULL;

			if (mby > 0) {
				for (uint32_t i = 0; i < 16; i++) {
					top16[i] = load_clamped(yuv->y, yuv->y_stride, w, h, x0 + i, y0 - 1);
				}
				top_ptr = top16;
			}
			if (mbx > 0) {
				for (uint32_t i = 0; i < 16; i++) {
					left16[i] = load_clamped(yuv->y, yuv->y_stride, w, h, x0 - 1, y0 + i);
				}
				left_ptr = left16;
			}
			const uint8_t dc_y = dc_value(left_ptr, top_ptr, 16, 16, 5);

			// U/V
			const uint32_t ux0 = mbx * 8u;
			const uint32_t uy0 = mby * 8u;
			uint8_t top8_u[8];
			uint8_t left8_u[8];
			uint8_t top8_v[8];
			uint8_t left8_v[8];
			const uint8_t* top_u = NULL;
			const uint8_t* left_u = NULL;
			const uint8_t* top_v = NULL;
			const uint8_t* left_v = NULL;

			if (mby > 0) {
				for (uint32_t i = 0; i < 8; i++) {
					top8_u[i] = load_clamped(yuv->u, yuv->uv_stride, uv_w, uv_h, ux0 + i, uy0 - 1);
					top8_v[i] = load_clamped(yuv->v, yuv->uv_stride, uv_w, uv_h, ux0 + i, uy0 - 1);
				}
				top_u = top8_u;
				top_v = top8_v;
			}
			if (mbx > 0) {
				for (uint32_t i = 0; i < 8; i++) {
					left8_u[i] = load_clamped(yuv->u, yuv->uv_stride, uv_w, uv_h, ux0 - 1, uy0 + i);
					left8_v[i] = load_clamped(yuv->v, yuv->uv_stride, uv_w, uv_h, ux0 - 1, uy0 + i);
				}
				left_u = left8_u;
				left_v = left8_v;
			}
			const uint8_t dc_u = dc_value(left_u, top_u, 8, 8, 4);
			const uint8_t dc_v = dc_value(left_v, top_v, 8, 8, 4);

			int16_t tmp[16][16];
			memset(tmp, 0, sizeof(tmp));
			uint8_t src4[16];
			uint8_t ref4[16];
			fill4x4_const(ref4, dc_y);

			for (uint32_t n = 0; n < 16; n++) {
				const uint32_t bx = (n & 3u) * 4u;
				const uint32_t by = (n >> 2) * 4u;
				fill4x4_clamped(src4, yuv->y, yuv->y_stride, w, h, x0 + bx, y0 + by);
				enc_vp8_ftransform4x4(src4, 4, ref4, 4, tmp[n]);
			}

			int16_t dc_tmp[16];
			enc_vp8_ftransform_wht(&tmp[0][0], dc_tmp);

			// Zero per-block DC (goes to Y2).
			for (int n = 0; n < 16; n++) tmp[n][0] = 0;

			// Store Y2.
			for (int i = 0; i < 16; i++) {
				store_i16le(buf + off, dc_tmp[i]);
				off += 2;
			}
			// Store Y blocks.
			for (int n = 0; n < 16; n++) {
				for (int i = 0; i < 16; i++) {
					store_i16le(buf + off, tmp[n][i]);
					off += 2;
				}
			}

			// U blocks.
			fill4x4_const(ref4, dc_u);
			for (uint32_t n = 0; n < 4; n++) {
				const uint32_t bx = (n & 1u) * 4u;
				const uint32_t by = (n >> 1) * 4u;
				fill4x4_clamped(src4, yuv->u, yuv->uv_stride, uv_w, uv_h, ux0 + bx, uy0 + by);
				int16_t out16[16];
				enc_vp8_ftransform4x4(src4, 4, ref4, 4, out16);
				for (int i = 0; i < 16; i++) {
					store_i16le(buf + off, out16[i]);
					off += 2;
				}
			}

			// V blocks.
			fill4x4_const(ref4, dc_v);
			for (uint32_t n = 0; n < 4; n++) {
				const uint32_t bx = (n & 1u) * 4u;
				const uint32_t by = (n >> 1) * 4u;
				fill4x4_clamped(src4, yuv->v, yuv->uv_stride, uv_w, uv_h, ux0 + bx, uy0 + by);
				int16_t out16[16];
				enc_vp8_ftransform4x4(src4, 4, ref4, 4, out16);
				for (int i = 0; i < 16; i++) {
					store_i16le(buf + off, out16[i]);
					off += 2;
				}
			}
		}
	}

	if (off != total_bytes) {
		free(buf);
		errno = EINVAL;
		return -1;
	}

	*out_bytes = buf;
	*out_size = total_bytes;
	return 0;
}
