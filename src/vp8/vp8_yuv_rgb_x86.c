#include "vp8_yuv_rgb_x86.h"

#if defined(VP8_YUV_RGB_HAVE_SSE2)
#include "vp8_yuv_rgb.h"

#include <emmintrin.h>

enum {
	YUV_SIMD_FIX = 6,
	YUV_SIMD_MASK = (256 << YUV_SIMD_FIX) - 1
};

static inline __m128i mult_hi_u8_sse2(__m128i values, int coeff) {
	const __m128i zero = _mm_setzero_si128();
	__m128i v16 = _mm_packs_epi32(values, zero);
	v16 = _mm_unpacklo_epi16(v16, zero);

	__m128i product = _mm_madd_epi16(v16, _mm_set1_epi32((uint16_t)coeff));
	// Coefficients above INT16_MAX are represented as coeff - 65536 for pmaddwd.
	if (coeff > 32767) product = _mm_add_epi32(product, _mm_slli_epi32(values, 16));
	return _mm_srai_epi32(product, 8);
}

static inline __m128i clip_fixed6_to_u8_sse2(__m128i v) {
	const __m128i zero = _mm_setzero_si128();
	const __m128i max = _mm_set1_epi32(YUV_SIMD_MASK);
	const __m128i neg = _mm_cmpgt_epi32(zero, v);
	const __m128i over = _mm_cmpgt_epi32(v, max);

	v = _mm_andnot_si128(neg, v);
	v = _mm_or_si128(_mm_and_si128(over, max), _mm_andnot_si128(over, v));
	v = _mm_srli_epi32(v, YUV_SIMD_FIX);

	const __m128i v16 = _mm_packs_epi32(v, zero);
	return _mm_packus_epi16(v16, zero);
}

static inline void yuv_to_rgb4_sse2(__m128i y, __m128i u, __m128i v, uint32_t* r4, uint32_t* g4, uint32_t* b4) {
	const __m128i y_mul = mult_hi_u8_sse2(y, 19077);
	const __m128i r = _mm_sub_epi32(_mm_add_epi32(y_mul, mult_hi_u8_sse2(v, 26149)), _mm_set1_epi32(14234));
	const __m128i g = _mm_add_epi32(_mm_sub_epi32(_mm_sub_epi32(y_mul, mult_hi_u8_sse2(u, 6419)),
	                                             mult_hi_u8_sse2(v, 13320)),
	                               _mm_set1_epi32(8708));
	const __m128i b = _mm_sub_epi32(_mm_add_epi32(y_mul, mult_hi_u8_sse2(u, 33050)), _mm_set1_epi32(17685));

	*r4 = (uint32_t)_mm_cvtsi128_si32(clip_fixed6_to_u8_sse2(r));
	*g4 = (uint32_t)_mm_cvtsi128_si32(clip_fixed6_to_u8_sse2(g));
	*b4 = (uint32_t)_mm_cvtsi128_si32(clip_fixed6_to_u8_sse2(b));
}

static inline void store_u32(uint8_t* dst, uint32_t v) {
#if defined(__GNUC__) || defined(__clang__)
	__builtin_memcpy(dst, &v, sizeof(v));
#else
	dst[0] = (uint8_t)v;
	dst[1] = (uint8_t)(v >> 8);
	dst[2] = (uint8_t)(v >> 16);
	dst[3] = (uint8_t)(v >> 24);
#endif
}

static inline void store_rgb4(uint8_t* dst, uint32_t r4, uint32_t g4, uint32_t b4) {
	const uint32_t out0 = (r4 & 0x000000FFu) | ((g4 & 0x000000FFu) << 8) | ((b4 & 0x000000FFu) << 16) |
	                      ((r4 & 0x0000FF00u) << 16);
	const uint32_t out1 = ((g4 & 0x0000FF00u) >> 8) | (b4 & 0x0000FF00u) | (r4 & 0x00FF0000u) |
	                      ((g4 & 0x00FF0000u) << 8);
	const uint32_t out2 = ((b4 & 0x00FF0000u) >> 16) | ((r4 & 0xFF000000u) >> 16) |
	                      ((g4 & 0xFF000000u) >> 8) | (b4 & 0xFF000000u);

	store_u32(dst + 0, out0);
	store_u32(dst + 4, out1);
	store_u32(dst + 8, out2);
}

static inline void yuv_to_rgb4_row_sse2(const uint8_t* y,
                                        uint8_t u0,
                                        uint8_t v0,
                                        uint8_t u1,
                                        uint8_t v1,
                                        uint8_t u2,
                                        uint8_t v2,
                                        uint8_t u3,
                                        uint8_t v3,
                                        uint8_t* dst) {
	uint32_t r4;
	uint32_t g4;
	uint32_t b4;
	yuv_to_rgb4_sse2(_mm_set_epi32(y[3], y[2], y[1], y[0]), _mm_set_epi32(u3, u2, u1, u0),
	                 _mm_set_epi32(v3, v2, v1, v0), &r4, &g4, &b4);
	store_rgb4(dst, r4, g4, b4);
}

void vp8_yuv_to_rgb4_sse2(uint8_t y0,
                          uint8_t u0,
                          uint8_t v0,
                          uint8_t y1,
                          uint8_t u1,
                          uint8_t v1,
                          uint8_t y2,
                          uint8_t u2,
                          uint8_t v2,
                          uint8_t y3,
                          uint8_t u3,
                          uint8_t v3,
                          uint8_t* dst0,
                          uint8_t* dst1,
                          uint8_t* dst2,
                          uint8_t* dst3) {
	uint32_t r4;
	uint32_t g4;
	uint32_t b4;
	const __m128i y = _mm_set_epi32(y3, y2, y1, y0);
	const __m128i u = _mm_set_epi32(u3, u2, u1, u0);
	const __m128i v = _mm_set_epi32(v3, v2, v1, v0);
	yuv_to_rgb4_sse2(y, u, v, &r4, &g4, &b4);

	dst0[0] = (uint8_t)r4;
	dst0[1] = (uint8_t)g4;
	dst0[2] = (uint8_t)b4;
	dst1[0] = (uint8_t)(r4 >> 8);
	dst1[1] = (uint8_t)(g4 >> 8);
	dst1[2] = (uint8_t)(b4 >> 8);
	dst2[0] = (uint8_t)(r4 >> 16);
	dst2[1] = (uint8_t)(g4 >> 16);
	dst2[2] = (uint8_t)(b4 >> 16);
	dst3[0] = (uint8_t)(r4 >> 24);
	dst3[1] = (uint8_t)(g4 >> 24);
	dst3[2] = (uint8_t)(b4 >> 24);
}

void vp8_upsample_rgb_line_sse2(const uint8_t* top_y,
                                const uint8_t* top_u,
                                const uint8_t* top_v,
                                const uint8_t* cur_u,
                                const uint8_t* cur_v,
                                uint8_t* top_dst,
                                uint32_t len) {
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

	uint32_t x = 1;
	for (; x + 1u <= last_pixel_pair; x += 2u) {
		const uint32_t t0_u = top_u[x];
		const uint32_t t0_v = top_v[x];
		const uint32_t u0 = cur_u[x];
		const uint32_t v0 = cur_v[x];
		const uint32_t t1_u = top_u[x + 1u];
		const uint32_t t1_v = top_v[x + 1u];
		const uint32_t u1 = cur_u[x + 1u];
		const uint32_t v1 = cur_v[x + 1u];

		const uint32_t avg0_u = tl_u + t0_u + l_u + u0 + 8u;
		const uint32_t avg0_v = tl_v + t0_v + l_v + v0 + 8u;
		const uint32_t diag0_12_u = (avg0_u + 2u * (t0_u + l_u)) >> 3;
		const uint32_t diag0_12_v = (avg0_v + 2u * (t0_v + l_v)) >> 3;
		const uint32_t diag0_03_u = (avg0_u + 2u * (tl_u + u0)) >> 3;
		const uint32_t diag0_03_v = (avg0_v + 2u * (tl_v + v0)) >> 3;

		const uint32_t avg1_u = t0_u + t1_u + u0 + u1 + 8u;
		const uint32_t avg1_v = t0_v + t1_v + v0 + v1 + 8u;
		const uint32_t diag1_12_u = (avg1_u + 2u * (t1_u + u0)) >> 3;
		const uint32_t diag1_12_v = (avg1_v + 2u * (t1_v + v0)) >> 3;
		const uint32_t diag1_03_u = (avg1_u + 2u * (t0_u + u1)) >> 3;
		const uint32_t diag1_03_v = (avg1_v + 2u * (t0_v + v1)) >> 3;

		yuv_to_rgb4_row_sse2(top_y + 2u * x - 1u, (uint8_t)((diag0_12_u + tl_u) >> 1),
		                     (uint8_t)((diag0_12_v + tl_v) >> 1), (uint8_t)((diag0_03_u + t0_u) >> 1),
		                     (uint8_t)((diag0_03_v + t0_v) >> 1), (uint8_t)((diag1_12_u + t0_u) >> 1),
		                     (uint8_t)((diag1_12_v + t0_v) >> 1), (uint8_t)((diag1_03_u + t1_u) >> 1),
		                     (uint8_t)((diag1_03_v + t1_v) >> 1), top_dst + (2u * x - 1u) * 3u);

		tl_u = t1_u;
		tl_v = t1_v;
		l_u = u1;
		l_v = v1;
	}

	for (; x <= last_pixel_pair; ++x) {
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

		tl_u = t_u;
		tl_v = t_v;
		l_u = u;
		l_v = v;
	}

	if ((len & 1u) == 0u) {
		const uint32_t idx = len - 1u;
		const uint8_t u0 = (uint8_t)((3u * tl_u + l_u + 2u) >> 2);
		const uint8_t v0 = (uint8_t)((3u * tl_v + l_v + 2u) >> 2);
		vp8_yuv_to_rgb(top_y[idx], u0, v0, top_dst + idx * 3u);
	}
}

void vp8_upsample_rgb_line_pair_sse2(const uint8_t* top_y,
                                     const uint8_t* bottom_y,
                                     const uint8_t* top_u,
                                     const uint8_t* top_v,
                                     const uint8_t* cur_u,
                                     const uint8_t* cur_v,
                                     uint8_t* top_dst,
                                     uint8_t* bottom_dst,
                                     uint32_t len) {
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
	{
		const uint8_t u0 = (uint8_t)((3u * l_u + tl_u + 2u) >> 2);
		const uint8_t v0 = (uint8_t)((3u * l_v + tl_v + 2u) >> 2);
		vp8_yuv_to_rgb(bottom_y[0], u0, v0, bottom_dst + 0);
	}

	uint32_t x = 1;
	for (; x + 1u <= last_pixel_pair; x += 2u) {
		const uint32_t t0_u = top_u[x];
		const uint32_t t0_v = top_v[x];
		const uint32_t u0 = cur_u[x];
		const uint32_t v0 = cur_v[x];
		const uint32_t t1_u = top_u[x + 1u];
		const uint32_t t1_v = top_v[x + 1u];
		const uint32_t u1 = cur_u[x + 1u];
		const uint32_t v1 = cur_v[x + 1u];

		const uint32_t avg0_u = tl_u + t0_u + l_u + u0 + 8u;
		const uint32_t avg0_v = tl_v + t0_v + l_v + v0 + 8u;
		const uint32_t diag0_12_u = (avg0_u + 2u * (t0_u + l_u)) >> 3;
		const uint32_t diag0_12_v = (avg0_v + 2u * (t0_v + l_v)) >> 3;
		const uint32_t diag0_03_u = (avg0_u + 2u * (tl_u + u0)) >> 3;
		const uint32_t diag0_03_v = (avg0_v + 2u * (tl_v + v0)) >> 3;

		const uint32_t avg1_u = t0_u + t1_u + u0 + u1 + 8u;
		const uint32_t avg1_v = t0_v + t1_v + v0 + v1 + 8u;
		const uint32_t diag1_12_u = (avg1_u + 2u * (t1_u + u0)) >> 3;
		const uint32_t diag1_12_v = (avg1_v + 2u * (t1_v + v0)) >> 3;
		const uint32_t diag1_03_u = (avg1_u + 2u * (t0_u + u1)) >> 3;
		const uint32_t diag1_03_v = (avg1_v + 2u * (t0_v + v1)) >> 3;

		yuv_to_rgb4_row_sse2(top_y + 2u * x - 1u, (uint8_t)((diag0_12_u + tl_u) >> 1),
		                     (uint8_t)((diag0_12_v + tl_v) >> 1), (uint8_t)((diag0_03_u + t0_u) >> 1),
		                     (uint8_t)((diag0_03_v + t0_v) >> 1), (uint8_t)((diag1_12_u + t0_u) >> 1),
		                     (uint8_t)((diag1_12_v + t0_v) >> 1), (uint8_t)((diag1_03_u + t1_u) >> 1),
		                     (uint8_t)((diag1_03_v + t1_v) >> 1), top_dst + (2u * x - 1u) * 3u);
		yuv_to_rgb4_row_sse2(bottom_y + 2u * x - 1u, (uint8_t)((diag0_03_u + l_u) >> 1),
		                     (uint8_t)((diag0_03_v + l_v) >> 1), (uint8_t)((diag0_12_u + u0) >> 1),
		                     (uint8_t)((diag0_12_v + v0) >> 1), (uint8_t)((diag1_03_u + u0) >> 1),
		                     (uint8_t)((diag1_03_v + v0) >> 1), (uint8_t)((diag1_12_u + u1) >> 1),
		                     (uint8_t)((diag1_12_v + v1) >> 1), bottom_dst + (2u * x - 1u) * 3u);

		tl_u = t1_u;
		tl_v = t1_v;
		l_u = u1;
		l_v = v1;
	}

	for (; x <= last_pixel_pair; ++x) {
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
			const uint32_t pix0 = 2u * x - 1u;
			const uint32_t pix1 = 2u * x + 0u;
			const uint8_t tu0 = (uint8_t)((diag_12_u + tl_u) >> 1);
			const uint8_t tv0 = (uint8_t)((diag_12_v + tl_v) >> 1);
			const uint8_t tu1 = (uint8_t)((diag_03_u + t_u) >> 1);
			const uint8_t tv1 = (uint8_t)((diag_03_v + t_v) >> 1);
			const uint8_t bu0 = (uint8_t)((diag_03_u + l_u) >> 1);
			const uint8_t bv0 = (uint8_t)((diag_03_v + l_v) >> 1);
			const uint8_t bu1 = (uint8_t)((diag_12_u + u) >> 1);
			const uint8_t bv1 = (uint8_t)((diag_12_v + v) >> 1);
			vp8_yuv_to_rgb4_sse2(top_y[pix0], tu0, tv0, top_y[pix1], tu1, tv1, bottom_y[pix0], bu0, bv0,
			                     bottom_y[pix1], bu1, bv1, top_dst + pix0 * 3u, top_dst + pix1 * 3u,
			                     bottom_dst + pix0 * 3u, bottom_dst + pix1 * 3u);
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
		{
			const uint8_t u0 = (uint8_t)((3u * l_u + tl_u + 2u) >> 2);
			const uint8_t v0 = (uint8_t)((3u * l_v + tl_v + 2u) >> 2);
			vp8_yuv_to_rgb(bottom_y[idx], u0, v0, bottom_dst + idx * 3u);
		}
	}
}
#else
typedef int vp8_yuv_rgb_x86_fallback_translation_unit;
#endif
