#include "vp8_yuv_rgb_x86.h"

#if defined(VP8_YUV_RGB_HAVE_SSE2)
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
	const __m128i y = _mm_set_epi32(y3, y2, y1, y0);
	const __m128i u = _mm_set_epi32(u3, u2, u1, u0);
	const __m128i v = _mm_set_epi32(v3, v2, v1, v0);
	const __m128i y_mul = mult_hi_u8_sse2(y, 19077);
	const __m128i r = _mm_sub_epi32(_mm_add_epi32(y_mul, mult_hi_u8_sse2(v, 26149)), _mm_set1_epi32(14234));
	const __m128i g = _mm_add_epi32(_mm_sub_epi32(_mm_sub_epi32(y_mul, mult_hi_u8_sse2(u, 6419)),
	                                             mult_hi_u8_sse2(v, 13320)),
	                               _mm_set1_epi32(8708));
	const __m128i b = _mm_sub_epi32(_mm_add_epi32(y_mul, mult_hi_u8_sse2(u, 33050)), _mm_set1_epi32(17685));

	const uint32_t r4 = (uint32_t)_mm_cvtsi128_si32(clip_fixed6_to_u8_sse2(r));
	const uint32_t g4 = (uint32_t)_mm_cvtsi128_si32(clip_fixed6_to_u8_sse2(g));
	const uint32_t b4 = (uint32_t)_mm_cvtsi128_si32(clip_fixed6_to_u8_sse2(b));

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
#else
typedef int vp8_yuv_rgb_x86_fallback_translation_unit;
#endif
