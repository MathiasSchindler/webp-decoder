#include "enc_rgb_to_yuv.h"

#include <errno.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#ifndef NO_LIBC
#include <math.h>
#endif

enum {
	YUV_FIX = 16,
	YUV_HALF = 1 << (YUV_FIX - 1),
};

static int g_gamma_tables_ok = 0;

#ifndef NO_LIBC
enum {
	GAMMA_FIX = 12,
	GAMMA_TAB_FIX = 7,
	GAMMA_TAB_SIZE = 1 << (GAMMA_FIX - GAMMA_TAB_FIX),
};

static const double kGamma = 0.80;
static const int kGammaScale = (1 << GAMMA_FIX) - 1;
static const int kGammaTabScale = 1 << GAMMA_TAB_FIX;
static const int kGammaTabRounder = 1 << (GAMMA_TAB_FIX - 1);

static int g_linear_to_gamma_tab[GAMMA_TAB_SIZE + 1];
static uint16_t g_gamma_to_linear_tab[256];
#endif

static void enc_init_gamma_tables(void) {
#ifdef NO_LIBC
	// Ultra/nolibc build: skip gamma tables (would require pow/libm).
	// We'll use plain sRGB averaging for chroma.
	g_gamma_tables_ok = 1;
	return;
#else
	if (g_gamma_tables_ok) return;

	const double norm = 1.0 / 255.0;
	const double scale = (double)(1 << GAMMA_TAB_FIX) / (double)kGammaScale;
	for (int v = 0; v <= 255; v++) {
		g_gamma_to_linear_tab[v] =
			(uint16_t)(pow(norm * (double)v, kGamma) * (double)kGammaScale + 0.5);
	}
	for (int v = 0; v <= GAMMA_TAB_SIZE; v++) {
		g_linear_to_gamma_tab[v] =
			(int)(255.0 * pow(scale * (double)v, 1.0 / kGamma) + 0.5);
	}
	g_gamma_tables_ok = 1;
#endif
}

#ifndef NO_LIBC
static inline uint32_t gamma_to_linear(uint8_t v) {
	return g_gamma_to_linear_tab[v];
}

static inline int interpolate(int v) {
	const int tab_pos = v >> (GAMMA_TAB_FIX + 2);
	const int x = v & ((kGammaTabScale << 2) - 1);
	if (tab_pos < 0) return 0;
	if (tab_pos >= GAMMA_TAB_SIZE) return g_linear_to_gamma_tab[GAMMA_TAB_SIZE] * (kGammaTabScale << 2);
	const int v0 = g_linear_to_gamma_tab[tab_pos];
	const int v1 = g_linear_to_gamma_tab[tab_pos + 1];
	return v1 * x + v0 * ((kGammaTabScale << 2) - x);
}

static inline int linear_to_gamma(uint32_t base_value, int shift) {
	const int y = interpolate((int)(base_value << shift));
	return (y + kGammaTabRounder) >> GAMMA_TAB_FIX;
}
#endif

static inline int clip_u8(int v) {
	return (v < 0) ? 0 : (v > 255) ? 255 : v;
}

static inline int vp8_clip_uv(int uv, int rounding) {
	uv = (uv + rounding + (128 << (YUV_FIX + 2))) >> (YUV_FIX + 2);
	return clip_u8(uv);
}

static inline int vp8_rgb_to_y(int r, int g, int b, int rounding) {
	const int luma = 16839 * r + 33059 * g + 6420 * b;
	return (luma + rounding + (16 << YUV_FIX)) >> YUV_FIX;
}

static inline int vp8_rgb_to_u(int r, int g, int b, int rounding) {
	const int u = -9719 * r - 19081 * g + 28800 * b;
	return vp8_clip_uv(u, rounding);
}

static inline int vp8_rgb_to_v(int r, int g, int b, int rounding) {
	const int v = 28800 * r - 24116 * g - 4684 * b;
	return vp8_clip_uv(v, rounding);
}

static int alloc_planes(uint32_t width, uint32_t height, EncYuv420Image* out) {
	const uint32_t uv_width = (width + 1u) >> 1;
	const uint32_t uv_height = (height + 1u) >> 1;
	const size_t y_bytes = (size_t)width * (size_t)height;
	const size_t uv_bytes = (size_t)uv_width * (size_t)uv_height;

	if (width == 0 || height == 0) {
		errno = EINVAL;
		return -1;
	}
	if (y_bytes / width != height) {
		errno = EOVERFLOW;
		return -1;
	}
	if (uv_width && uv_bytes / uv_width != uv_height) {
		errno = EOVERFLOW;
		return -1;
	}

	uint8_t* y = (uint8_t*)malloc(y_bytes);
	uint8_t* u = (uint8_t*)malloc(uv_bytes);
	uint8_t* v = (uint8_t*)malloc(uv_bytes);
	if (!y || !u || !v) {
		free(y);
		free(u);
		free(v);
		errno = ENOMEM;
		return -1;
	}

	out->width = width;
	out->height = height;
	out->y_stride = width;
	out->uv_stride = uv_width;
	out->y = y;
	out->u = u;
	out->v = v;
	return 0;
}

int enc_yuv420_from_rgb_libwebp(const uint8_t* rgb,
                               uint32_t width,
                               uint32_t height,
                               uint32_t rgb_stride,
                               uint32_t rgb_step,
                               EncYuv420Image* out) {
	if (!out) {
		errno = EINVAL;
		return -1;
	}
	memset(out, 0, sizeof(*out));

	if (!rgb || width == 0 || height == 0) {
		errno = EINVAL;
		return -1;
	}
	if (!(rgb_step == 3 || rgb_step == 4)) {
		errno = EINVAL;
		return -1;
	}
	if (rgb_stride < width * rgb_step) {
		errno = EINVAL;
		return -1;
	}

	enc_init_gamma_tables();
	if (!g_gamma_tables_ok) {
		errno = EINVAL;
		return -1;
	}

	if (alloc_planes(width, height, out) != 0) return -1;

	// Y plane.
	for (uint32_t y = 0; y < height; y++) {
		const uint8_t* row = rgb + (size_t)y * (size_t)rgb_stride;
		uint8_t* dst_y = out->y + (size_t)y * (size_t)out->y_stride;
		for (uint32_t x = 0; x < width; x++) {
			const uint8_t* p = row + (size_t)x * (size_t)rgb_step;
			dst_y[x] = (uint8_t)vp8_rgb_to_y((int)p[0], (int)p[1], (int)p[2], YUV_HALF);
		}
	}

	// U/V planes (4:2:0), with edge replication for odd sizes.
	const uint32_t uv_width = out->uv_stride;
	const uint32_t uv_height = (height + 1u) >> 1;
	for (uint32_t uy = 0; uy < uv_height; uy++) {
		const uint32_t y0 = 2u * uy;
		const uint32_t y1 = (y0 + 1u < height) ? (y0 + 1u) : y0;
		const uint8_t* row0 = rgb + (size_t)y0 * (size_t)rgb_stride;
		const uint8_t* row1 = rgb + (size_t)y1 * (size_t)rgb_stride;

		uint8_t* dst_u = out->u + (size_t)uy * (size_t)out->uv_stride;
		uint8_t* dst_v = out->v + (size_t)uy * (size_t)out->uv_stride;

		for (uint32_t ux = 0; ux < uv_width; ux++) {
			const uint32_t x0 = 2u * ux;
			const uint32_t x1 = (x0 + 1u < width) ? (x0 + 1u) : x0;

			const uint8_t* p00 = row0 + (size_t)x0 * (size_t)rgb_step;
			const uint8_t* p01 = row0 + (size_t)x1 * (size_t)rgb_step;
			const uint8_t* p10 = row1 + (size_t)x0 * (size_t)rgb_step;
			const uint8_t* p11 = row1 + (size_t)x1 * (size_t)rgb_step;

#ifdef NO_LIBC
			const int r_sum = ((int)p00[0] + (int)p01[0] + (int)p10[0] + (int)p11[0] + 2) >> 2;
			const int g_sum = ((int)p00[1] + (int)p01[1] + (int)p10[1] + (int)p11[1] + 2) >> 2;
			const int b_sum = ((int)p00[2] + (int)p01[2] + (int)p10[2] + (int)p11[2] + 2) >> 2;
#else
			const uint32_t r_lin = gamma_to_linear(p00[0]) + gamma_to_linear(p01[0]) +
			                   gamma_to_linear(p10[0]) + gamma_to_linear(p11[0]);
			const uint32_t g_lin = gamma_to_linear(p00[1]) + gamma_to_linear(p01[1]) +
			                   gamma_to_linear(p10[1]) + gamma_to_linear(p11[1]);
			const uint32_t b_lin = gamma_to_linear(p00[2]) + gamma_to_linear(p01[2]) +
			                   gamma_to_linear(p10[2]) + gamma_to_linear(p11[2]);

			const int r_sum = linear_to_gamma(r_lin, 0);
			const int g_sum = linear_to_gamma(g_lin, 0);
			const int b_sum = linear_to_gamma(b_lin, 0);
#endif

			dst_u[ux] = (uint8_t)vp8_rgb_to_u(r_sum, g_sum, b_sum, YUV_HALF << 2);
			dst_v[ux] = (uint8_t)vp8_rgb_to_v(r_sum, g_sum, b_sum, YUV_HALF << 2);
		}
	}

	return 0;
}

void enc_yuv420_free(EncYuv420Image* img) {
	if (!img) return;
	free(img->y);
	free(img->u);
	free(img->v);
	memset(img, 0, sizeof(*img));
}
