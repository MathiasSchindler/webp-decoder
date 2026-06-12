#include "vp8_yuv_rgb_x86.h"

#if defined(VP8_YUV_RGB_HAVE_SSE2)
#include "vp8_yuv_rgb.h"

#include <emmintrin.h>
#if defined(VP8_YUV_RGB_HAVE_SSSE3)
#include <tmmintrin.h>
#endif
#if defined(VP8_YUV_RGB_HAVE_AVX2)
#include <immintrin.h>
#endif

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

#if defined(VP8_YUV_RGB_HAVE_SSSE3)
static inline void store_rgb4_ssse3(uint8_t* dst, __m128i r, __m128i g, __m128i b) {
	const __m128i mr = _mm_setr_epi8(0, (char)0x80, (char)0x80, 1, (char)0x80, (char)0x80, 2, (char)0x80,
	                                (char)0x80, 3, (char)0x80, (char)0x80, (char)0x80, (char)0x80,
	                                (char)0x80, (char)0x80);
	const __m128i mg = _mm_setr_epi8((char)0x80, 0, (char)0x80, (char)0x80, 1, (char)0x80, (char)0x80, 2,
	                                (char)0x80, (char)0x80, 3, (char)0x80, (char)0x80, (char)0x80,
	                                (char)0x80, (char)0x80);
	const __m128i mb = _mm_setr_epi8((char)0x80, (char)0x80, 0, (char)0x80, (char)0x80, 1, (char)0x80,
	                                (char)0x80, 2, (char)0x80, (char)0x80, 3, (char)0x80, (char)0x80,
	                                (char)0x80, (char)0x80);
	const __m128i out = _mm_or_si128(_mm_or_si128(_mm_shuffle_epi8(r, mr), _mm_shuffle_epi8(g, mg)),
	                                _mm_shuffle_epi8(b, mb));
	_mm_storel_epi64((__m128i*)dst, out);
	store_u32(dst + 8, (uint32_t)_mm_cvtsi128_si32(_mm_srli_si128(out, 8)));
}

static inline void yuv_to_rgb4_row_ssse3(const uint8_t* y,
                                         uint8_t u0,
                                         uint8_t v0,
                                         uint8_t u1,
                                         uint8_t v1,
                                         uint8_t u2,
                                         uint8_t v2,
                                         uint8_t u3,
                                         uint8_t v3,
                                         uint8_t* dst) {
	const __m128i yy = _mm_set_epi32(y[3], y[2], y[1], y[0]);
	const __m128i uu = _mm_set_epi32(u3, u2, u1, u0);
	const __m128i vv = _mm_set_epi32(v3, v2, v1, v0);
	const __m128i y_mul = mult_hi_u8_sse2(yy, 19077);
	const __m128i r = _mm_sub_epi32(_mm_add_epi32(y_mul, mult_hi_u8_sse2(vv, 26149)), _mm_set1_epi32(14234));
	const __m128i g = _mm_add_epi32(_mm_sub_epi32(_mm_sub_epi32(y_mul, mult_hi_u8_sse2(uu, 6419)),
	                                             mult_hi_u8_sse2(vv, 13320)),
	                               _mm_set1_epi32(8708));
	const __m128i b = _mm_sub_epi32(_mm_add_epi32(y_mul, mult_hi_u8_sse2(uu, 33050)), _mm_set1_epi32(17685));
	store_rgb4_ssse3(dst, clip_fixed6_to_u8_sse2(r), clip_fixed6_to_u8_sse2(g), clip_fixed6_to_u8_sse2(b));
}
#endif

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
#if defined(VP8_YUV_RGB_HAVE_SSSE3)
	yuv_to_rgb4_row_ssse3(y, u0, v0, u1, v1, u2, v2, u3, v3, dst);
#else
	uint32_t r4;
	uint32_t g4;
	uint32_t b4;
	yuv_to_rgb4_sse2(_mm_set_epi32(y[3], y[2], y[1], y[0]), _mm_set_epi32(u3, u2, u1, u0),
	                 _mm_set_epi32(v3, v2, v1, v0), &r4, &g4, &b4);
	store_rgb4(dst, r4, g4, b4);
#endif
}

#if defined(VP8_YUV_RGB_HAVE_AVX2)
static inline __m256i mult_hi_u8_avx2(__m256i values, int coeff) {
	return _mm256_srai_epi32(_mm256_mullo_epi32(values, _mm256_set1_epi32(coeff)), 8);
}

static inline __m128i clip_fixed6_to_u8_avx2(__m256i v) {
	const __m256i zero = _mm256_setzero_si256();
	const __m256i max = _mm256_set1_epi32(YUV_SIMD_MASK);
	const __m256i neg = _mm256_cmpgt_epi32(zero, v);
	const __m256i over = _mm256_cmpgt_epi32(v, max);

	v = _mm256_andnot_si256(neg, v);
	v = _mm256_or_si256(_mm256_and_si256(over, max), _mm256_andnot_si256(over, v));
	v = _mm256_srli_epi32(v, YUV_SIMD_FIX);

	const __m128i lo = _mm256_castsi256_si128(v);
	const __m128i hi = _mm256_extracti128_si256(v, 1);
	const __m128i v16 = _mm_packs_epi32(lo, hi);
	return _mm_packus_epi16(v16, _mm_setzero_si128());
}

static inline void store_rgb8_avx2(uint8_t* dst, __m128i r, __m128i g, __m128i b) {
	const __m128i r0 = _mm_setr_epi8(0, (char)0x80, (char)0x80, 1, (char)0x80, (char)0x80, 2, (char)0x80,
	                                (char)0x80, 3, (char)0x80, (char)0x80, 4, (char)0x80, (char)0x80, 5);
	const __m128i g0 = _mm_setr_epi8((char)0x80, 0, (char)0x80, (char)0x80, 1, (char)0x80, (char)0x80, 2,
	                                (char)0x80, (char)0x80, 3, (char)0x80, (char)0x80, 4, (char)0x80,
	                                (char)0x80);
	const __m128i b0 = _mm_setr_epi8((char)0x80, (char)0x80, 0, (char)0x80, (char)0x80, 1, (char)0x80,
	                                (char)0x80, 2, (char)0x80, (char)0x80, 3, (char)0x80, (char)0x80, 4,
	                                (char)0x80);
	const __m128i r1 = _mm_setr_epi8((char)0x80, (char)0x80, 6, (char)0x80, (char)0x80, 7, (char)0x80,
	                                (char)0x80, (char)0x80, (char)0x80, (char)0x80, (char)0x80, (char)0x80,
	                                (char)0x80, (char)0x80, (char)0x80);
	const __m128i g1 = _mm_setr_epi8(5, (char)0x80, (char)0x80, 6, (char)0x80, (char)0x80, 7, (char)0x80,
	                                (char)0x80, (char)0x80, (char)0x80, (char)0x80, (char)0x80, (char)0x80,
	                                (char)0x80, (char)0x80);
	const __m128i b1 = _mm_setr_epi8((char)0x80, 5, (char)0x80, (char)0x80, 6, (char)0x80, (char)0x80, 7,
	                                (char)0x80, (char)0x80, (char)0x80, (char)0x80, (char)0x80, (char)0x80,
	                                (char)0x80, (char)0x80);
	const __m128i out0 = _mm_or_si128(_mm_or_si128(_mm_shuffle_epi8(r, r0), _mm_shuffle_epi8(g, g0)),
	                                 _mm_shuffle_epi8(b, b0));
	const __m128i out1 = _mm_or_si128(_mm_or_si128(_mm_shuffle_epi8(r, r1), _mm_shuffle_epi8(g, g1)),
	                                 _mm_shuffle_epi8(b, b1));
	_mm_storeu_si128((__m128i*)dst, out0);
	_mm_storel_epi64((__m128i*)(dst + 16), out1);
}

static inline void yuv_to_rgb8_row_avx2(const uint8_t* y,
                                        uint8_t u0,
                                        uint8_t v0,
                                        uint8_t u1,
                                        uint8_t v1,
                                        uint8_t u2,
                                        uint8_t v2,
                                        uint8_t u3,
                                        uint8_t v3,
                                        uint8_t u4,
                                        uint8_t v4,
                                        uint8_t u5,
                                        uint8_t v5,
                                        uint8_t u6,
                                        uint8_t v6,
                                        uint8_t u7,
                                        uint8_t v7,
                                        uint8_t* dst) {
	const __m256i yy = _mm256_setr_epi32(y[0], y[1], y[2], y[3], y[4], y[5], y[6], y[7]);
	const __m256i uu = _mm256_setr_epi32(u0, u1, u2, u3, u4, u5, u6, u7);
	const __m256i vv = _mm256_setr_epi32(v0, v1, v2, v3, v4, v5, v6, v7);
	const __m256i y_mul = mult_hi_u8_avx2(yy, 19077);
	const __m256i r = _mm256_sub_epi32(_mm256_add_epi32(y_mul, mult_hi_u8_avx2(vv, 26149)),
	                                  _mm256_set1_epi32(14234));
	const __m256i g = _mm256_add_epi32(_mm256_sub_epi32(_mm256_sub_epi32(y_mul, mult_hi_u8_avx2(uu, 6419)),
	                                                   mult_hi_u8_avx2(vv, 13320)),
	                                  _mm256_set1_epi32(8708));
	const __m256i b = _mm256_sub_epi32(_mm256_add_epi32(y_mul, mult_hi_u8_avx2(uu, 33050)),
	                                  _mm256_set1_epi32(17685));
	store_rgb8_avx2(dst, clip_fixed6_to_u8_avx2(r), clip_fixed6_to_u8_avx2(g), clip_fixed6_to_u8_avx2(b));
}

static inline __m128i load_u8x8(const uint8_t* p) {
	return _mm_loadl_epi64((const __m128i*)p);
}

static inline __m128i load_u8x8_to_u16(const uint8_t* p) {
	return _mm_unpacklo_epi8(load_u8x8(p), _mm_setzero_si128());
}

static inline __m128i pack_interleaved_u8(__m128i a, __m128i b) {
	const __m128i zero = _mm_setzero_si128();
	return _mm_unpacklo_epi8(_mm_packus_epi16(a, zero), _mm_packus_epi16(b, zero));
}

static inline void yuv_to_rgb8_vec_avx2(__m128i y8, __m128i u8, __m128i v8, uint8_t* dst) {
	const __m256i yy = _mm256_cvtepu8_epi32(y8);
	const __m256i uu = _mm256_cvtepu8_epi32(u8);
	const __m256i vv = _mm256_cvtepu8_epi32(v8);
	const __m256i y_mul = mult_hi_u8_avx2(yy, 19077);
	const __m256i r = _mm256_sub_epi32(_mm256_add_epi32(y_mul, mult_hi_u8_avx2(vv, 26149)),
	                                  _mm256_set1_epi32(14234));
	const __m256i g = _mm256_add_epi32(_mm256_sub_epi32(_mm256_sub_epi32(y_mul, mult_hi_u8_avx2(uu, 6419)),
	                                                   mult_hi_u8_avx2(vv, 13320)),
	                                  _mm256_set1_epi32(8708));
	const __m256i b = _mm256_sub_epi32(_mm256_add_epi32(y_mul, mult_hi_u8_avx2(uu, 33050)),
	                                  _mm256_set1_epi32(17685));
	store_rgb8_avx2(dst, clip_fixed6_to_u8_avx2(r), clip_fixed6_to_u8_avx2(g), clip_fixed6_to_u8_avx2(b));
}

typedef struct {
	uint8_t top0_u, top0_v, top1_u, top1_v;
	uint8_t bottom0_u, bottom0_v, bottom1_u, bottom1_v;
} Vp8UvEdge;

typedef struct {
	__m128i top_u;
	__m128i top_v;
	__m128i bottom_u;
	__m128i bottom_v;
} Vp8UvEdge8;

static inline Vp8UvEdge8 make_uv_edges8(const uint8_t* top_u,
                                        const uint8_t* top_v,
                                        const uint8_t* cur_u,
                                        const uint8_t* cur_v,
                                        uint32_t x) {
	const __m128i tl_u = load_u8x8_to_u16(top_u + x - 1u);
	const __m128i tl_v = load_u8x8_to_u16(top_v + x - 1u);
	const __m128i l_u = load_u8x8_to_u16(cur_u + x - 1u);
	const __m128i l_v = load_u8x8_to_u16(cur_v + x - 1u);
	const __m128i t_u = load_u8x8_to_u16(top_u + x);
	const __m128i t_v = load_u8x8_to_u16(top_v + x);
	const __m128i u = load_u8x8_to_u16(cur_u + x);
	const __m128i v = load_u8x8_to_u16(cur_v + x);
	const __m128i bias = _mm_set1_epi16(8);

	const __m128i diag_12_u =
	    _mm_srli_epi16(_mm_add_epi16(_mm_add_epi16(_mm_add_epi16(tl_u, u),
	                                              _mm_add_epi16(t_u, _mm_slli_epi16(t_u, 1))),
	                                  _mm_add_epi16(_mm_add_epi16(l_u, _mm_slli_epi16(l_u, 1)), bias)),
	                  3);
	const __m128i diag_12_v =
	    _mm_srli_epi16(_mm_add_epi16(_mm_add_epi16(_mm_add_epi16(tl_v, v),
	                                              _mm_add_epi16(t_v, _mm_slli_epi16(t_v, 1))),
	                                  _mm_add_epi16(_mm_add_epi16(l_v, _mm_slli_epi16(l_v, 1)), bias)),
	                  3);
	const __m128i diag_03_u =
	    _mm_srli_epi16(_mm_add_epi16(_mm_add_epi16(_mm_add_epi16(t_u, l_u),
	                                              _mm_add_epi16(tl_u, _mm_slli_epi16(tl_u, 1))),
	                                  _mm_add_epi16(_mm_add_epi16(u, _mm_slli_epi16(u, 1)), bias)),
	                  3);
	const __m128i diag_03_v =
	    _mm_srli_epi16(_mm_add_epi16(_mm_add_epi16(_mm_add_epi16(t_v, l_v),
	                                              _mm_add_epi16(tl_v, _mm_slli_epi16(tl_v, 1))),
	                                  _mm_add_epi16(_mm_add_epi16(v, _mm_slli_epi16(v, 1)), bias)),
	                  3);

	Vp8UvEdge8 e;
	e.top_u = pack_interleaved_u8(_mm_srli_epi16(_mm_add_epi16(diag_12_u, tl_u), 1),
	                             _mm_srli_epi16(_mm_add_epi16(diag_03_u, t_u), 1));
	e.top_v = pack_interleaved_u8(_mm_srli_epi16(_mm_add_epi16(diag_12_v, tl_v), 1),
	                             _mm_srli_epi16(_mm_add_epi16(diag_03_v, t_v), 1));
	e.bottom_u = pack_interleaved_u8(_mm_srli_epi16(_mm_add_epi16(diag_03_u, l_u), 1),
	                                _mm_srli_epi16(_mm_add_epi16(diag_12_u, u), 1));
	e.bottom_v = pack_interleaved_u8(_mm_srli_epi16(_mm_add_epi16(diag_03_v, l_v), 1),
	                                _mm_srli_epi16(_mm_add_epi16(diag_12_v, v), 1));
	return e;
}

static inline Vp8UvEdge make_uv_edge(uint32_t tl_u,
                                     uint32_t tl_v,
                                     uint32_t l_u,
                                     uint32_t l_v,
                                     uint32_t t_u,
                                     uint32_t t_v,
                                     uint32_t u,
                                     uint32_t v) {
	const uint32_t avg_u = tl_u + t_u + l_u + u + 8u;
	const uint32_t avg_v = tl_v + t_v + l_v + v + 8u;
	const uint32_t diag_12_u = (avg_u + 2u * (t_u + l_u)) >> 3;
	const uint32_t diag_12_v = (avg_v + 2u * (t_v + l_v)) >> 3;
	const uint32_t diag_03_u = (avg_u + 2u * (tl_u + u)) >> 3;
	const uint32_t diag_03_v = (avg_v + 2u * (tl_v + v)) >> 3;
	Vp8UvEdge e;
	e.top0_u = (uint8_t)((diag_12_u + tl_u) >> 1);
	e.top0_v = (uint8_t)((diag_12_v + tl_v) >> 1);
	e.top1_u = (uint8_t)((diag_03_u + t_u) >> 1);
	e.top1_v = (uint8_t)((diag_03_v + t_v) >> 1);
	e.bottom0_u = (uint8_t)((diag_03_u + l_u) >> 1);
	e.bottom0_v = (uint8_t)((diag_03_v + l_v) >> 1);
	e.bottom1_u = (uint8_t)((diag_12_u + u) >> 1);
	e.bottom1_v = (uint8_t)((diag_12_v + v) >> 1);
	return e;
}
#endif

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
#if defined(VP8_YUV_RGB_HAVE_AVX2)
	for (; x + 7u <= last_pixel_pair; x += 8u) {
		const Vp8UvEdge8 e = make_uv_edges8(top_u, top_v, cur_u, cur_v, x);
		const __m128i y = _mm_loadu_si128((const __m128i*)(top_y + 2u * x - 1u));
		yuv_to_rgb8_vec_avx2(y, e.top_u, e.top_v, top_dst + (2u * x - 1u) * 3u);
		yuv_to_rgb8_vec_avx2(_mm_srli_si128(y, 8), _mm_srli_si128(e.top_u, 8), _mm_srli_si128(e.top_v, 8),
		                     top_dst + (2u * x - 1u) * 3u + 24u);

		tl_u = top_u[x + 7u];
		tl_v = top_v[x + 7u];
		l_u = cur_u[x + 7u];
		l_v = cur_v[x + 7u];
	}
	for (; x + 3u <= last_pixel_pair; x += 4u) {
		const uint32_t t0_u = top_u[x];
		const uint32_t t0_v = top_v[x];
		const uint32_t u0 = cur_u[x];
		const uint32_t v0 = cur_v[x];
		const uint32_t t1_u = top_u[x + 1u];
		const uint32_t t1_v = top_v[x + 1u];
		const uint32_t u1 = cur_u[x + 1u];
		const uint32_t v1 = cur_v[x + 1u];
		const uint32_t t2_u = top_u[x + 2u];
		const uint32_t t2_v = top_v[x + 2u];
		const uint32_t u2 = cur_u[x + 2u];
		const uint32_t v2 = cur_v[x + 2u];
		const uint32_t t3_u = top_u[x + 3u];
		const uint32_t t3_v = top_v[x + 3u];
		const uint32_t u3 = cur_u[x + 3u];
		const uint32_t v3 = cur_v[x + 3u];

		const Vp8UvEdge e0 = make_uv_edge(tl_u, tl_v, l_u, l_v, t0_u, t0_v, u0, v0);
		const Vp8UvEdge e1 = make_uv_edge(t0_u, t0_v, u0, v0, t1_u, t1_v, u1, v1);
		const Vp8UvEdge e2 = make_uv_edge(t1_u, t1_v, u1, v1, t2_u, t2_v, u2, v2);
		const Vp8UvEdge e3 = make_uv_edge(t2_u, t2_v, u2, v2, t3_u, t3_v, u3, v3);

		yuv_to_rgb8_row_avx2(top_y + 2u * x - 1u, e0.top0_u, e0.top0_v, e0.top1_u, e0.top1_v,
		                     e1.top0_u, e1.top0_v, e1.top1_u, e1.top1_v, e2.top0_u, e2.top0_v,
		                     e2.top1_u, e2.top1_v, e3.top0_u, e3.top0_v, e3.top1_u, e3.top1_v,
		                     top_dst + (2u * x - 1u) * 3u);

		tl_u = t3_u;
		tl_v = t3_v;
		l_u = u3;
		l_v = v3;
	}
#endif
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
#if defined(VP8_YUV_RGB_HAVE_AVX2)
	for (; x + 7u <= last_pixel_pair; x += 8u) {
		const Vp8UvEdge8 e = make_uv_edges8(top_u, top_v, cur_u, cur_v, x);
		const __m128i ty = _mm_loadu_si128((const __m128i*)(top_y + 2u * x - 1u));
		const __m128i by = _mm_loadu_si128((const __m128i*)(bottom_y + 2u * x - 1u));
		yuv_to_rgb8_vec_avx2(ty, e.top_u, e.top_v, top_dst + (2u * x - 1u) * 3u);
		yuv_to_rgb8_vec_avx2(_mm_srli_si128(ty, 8), _mm_srli_si128(e.top_u, 8), _mm_srli_si128(e.top_v, 8),
		                     top_dst + (2u * x - 1u) * 3u + 24u);
		yuv_to_rgb8_vec_avx2(by, e.bottom_u, e.bottom_v, bottom_dst + (2u * x - 1u) * 3u);
		yuv_to_rgb8_vec_avx2(_mm_srli_si128(by, 8), _mm_srli_si128(e.bottom_u, 8), _mm_srli_si128(e.bottom_v, 8),
		                     bottom_dst + (2u * x - 1u) * 3u + 24u);

		tl_u = top_u[x + 7u];
		tl_v = top_v[x + 7u];
		l_u = cur_u[x + 7u];
		l_v = cur_v[x + 7u];
	}
	for (; x + 3u <= last_pixel_pair; x += 4u) {
		const uint32_t t0_u = top_u[x];
		const uint32_t t0_v = top_v[x];
		const uint32_t u0 = cur_u[x];
		const uint32_t v0 = cur_v[x];
		const uint32_t t1_u = top_u[x + 1u];
		const uint32_t t1_v = top_v[x + 1u];
		const uint32_t u1 = cur_u[x + 1u];
		const uint32_t v1 = cur_v[x + 1u];
		const uint32_t t2_u = top_u[x + 2u];
		const uint32_t t2_v = top_v[x + 2u];
		const uint32_t u2 = cur_u[x + 2u];
		const uint32_t v2 = cur_v[x + 2u];
		const uint32_t t3_u = top_u[x + 3u];
		const uint32_t t3_v = top_v[x + 3u];
		const uint32_t u3 = cur_u[x + 3u];
		const uint32_t v3 = cur_v[x + 3u];

		const Vp8UvEdge e0 = make_uv_edge(tl_u, tl_v, l_u, l_v, t0_u, t0_v, u0, v0);
		const Vp8UvEdge e1 = make_uv_edge(t0_u, t0_v, u0, v0, t1_u, t1_v, u1, v1);
		const Vp8UvEdge e2 = make_uv_edge(t1_u, t1_v, u1, v1, t2_u, t2_v, u2, v2);
		const Vp8UvEdge e3 = make_uv_edge(t2_u, t2_v, u2, v2, t3_u, t3_v, u3, v3);

		yuv_to_rgb8_row_avx2(top_y + 2u * x - 1u, e0.top0_u, e0.top0_v, e0.top1_u, e0.top1_v,
		                     e1.top0_u, e1.top0_v, e1.top1_u, e1.top1_v, e2.top0_u, e2.top0_v,
		                     e2.top1_u, e2.top1_v, e3.top0_u, e3.top0_v, e3.top1_u, e3.top1_v,
		                     top_dst + (2u * x - 1u) * 3u);
		yuv_to_rgb8_row_avx2(bottom_y + 2u * x - 1u, e0.bottom0_u, e0.bottom0_v, e0.bottom1_u,
		                     e0.bottom1_v, e1.bottom0_u, e1.bottom0_v, e1.bottom1_u, e1.bottom1_v,
		                     e2.bottom0_u, e2.bottom0_v, e2.bottom1_u, e2.bottom1_v, e3.bottom0_u,
		                     e3.bottom0_v, e3.bottom1_u, e3.bottom1_v, bottom_dst + (2u * x - 1u) * 3u);

		tl_u = t3_u;
		tl_v = t3_v;
		l_u = u3;
		l_v = v3;
	}
#endif
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
