#include "vp8_loopfilter_x86.h"

#if defined(VP8_LOOPFILTER_HAVE_SSE2)
#include <emmintrin.h>
#include <stddef.h>

static inline __m128i absdiff_u8_sse2(__m128i a, __m128i b) {
	return _mm_or_si128(_mm_subs_epu8(a, b), _mm_subs_epu8(b, a));
}

static inline __m128i clamp_i8range_epi16_sse2(__m128i v) {
	v = _mm_min_epi16(v, _mm_set1_epi16(127));
	return _mm_max_epi16(v, _mm_set1_epi16(-128));
}

static inline __m128i simple_threshold_mask_sse2(__m128i p1, __m128i p0, __m128i q0, __m128i q1, int filter_limit) {
	const __m128i zero = _mm_setzero_si128();
	const __m128i limit = _mm_set1_epi16((short)filter_limit);
	const __m128i all = _mm_cmpeq_epi8(zero, zero);
	const __m128i d0 = absdiff_u8_sse2(p0, q0);
	const __m128i d1 = absdiff_u8_sse2(p1, q1);

	const __m128i d0_lo = _mm_unpacklo_epi8(d0, zero);
	const __m128i d0_hi = _mm_unpackhi_epi8(d0, zero);
	const __m128i d1_lo = _mm_unpacklo_epi8(d1, zero);
	const __m128i d1_hi = _mm_unpackhi_epi8(d1, zero);
	const __m128i sum_lo = _mm_add_epi16(_mm_slli_epi16(d0_lo, 1), _mm_srli_epi16(d1_lo, 1));
	const __m128i sum_hi = _mm_add_epi16(_mm_slli_epi16(d0_hi, 1), _mm_srli_epi16(d1_hi, 1));
	const __m128i pass_lo = _mm_andnot_si128(_mm_cmpgt_epi16(sum_lo, limit), all);
	const __m128i pass_hi = _mm_andnot_si128(_mm_cmpgt_epi16(sum_hi, limit), all);
	return _mm_packs_epi16(pass_lo, pass_hi);
}

static inline __m128i le_u8_mask_sse2(__m128i v, int limit) {
	const __m128i over = _mm_subs_epu8(v, _mm_set1_epi8((char)limit));
	return _mm_cmpeq_epi8(over, _mm_setzero_si128());
}

static inline __m128i normal_threshold_mask_sse2(__m128i p3,
                                                 __m128i p2,
                                                 __m128i p1,
                                                 __m128i p0,
                                                 __m128i q0,
                                                 __m128i q1,
                                                 __m128i q2,
                                                 __m128i q3,
                                                 int edge_limit,
                                                 int interior_limit) {
	__m128i mask = simple_threshold_mask_sse2(p1, p0, q0, q1, 2 * edge_limit + interior_limit);
	mask = _mm_and_si128(mask, le_u8_mask_sse2(absdiff_u8_sse2(p3, p2), interior_limit));
	mask = _mm_and_si128(mask, le_u8_mask_sse2(absdiff_u8_sse2(p2, p1), interior_limit));
	mask = _mm_and_si128(mask, le_u8_mask_sse2(absdiff_u8_sse2(p1, p0), interior_limit));
	mask = _mm_and_si128(mask, le_u8_mask_sse2(absdiff_u8_sse2(q3, q2), interior_limit));
	mask = _mm_and_si128(mask, le_u8_mask_sse2(absdiff_u8_sse2(q2, q1), interior_limit));
	mask = _mm_and_si128(mask, le_u8_mask_sse2(absdiff_u8_sse2(q1, q0), interior_limit));
	return mask;
}

static inline __m128i hev_mask_sse2(__m128i p1, __m128i p0, __m128i q0, __m128i q1, int hev_threshold) {
	const __m128i p_ok = le_u8_mask_sse2(absdiff_u8_sse2(p1, p0), hev_threshold);
	const __m128i q_ok = le_u8_mask_sse2(absdiff_u8_sse2(q1, q0), hev_threshold);
	const __m128i all = _mm_cmpeq_epi8(p_ok, p_ok);
	return _mm_andnot_si128(_mm_and_si128(p_ok, q_ok), all);
}

static inline __m128i select_u8_sse2(__m128i on_mask, __m128i on_value, __m128i off_value) {
	return _mm_or_si128(_mm_and_si128(on_mask, on_value), _mm_andnot_si128(on_mask, off_value));
}

static inline __m128i load_u8_width_sse2(const uint8_t* p, int width) {
	return (width == 8) ? _mm_loadl_epi64((const __m128i*)p) : _mm_loadu_si128((const __m128i*)p);
}

static inline void store_u8_width_sse2(uint8_t* p, __m128i v, int width) {
	if (width == 8)
		_mm_storel_epi64((__m128i*)p, v);
	else
		_mm_storeu_si128((__m128i*)p, v);
}

static inline void transpose_8x8_low_sse2(const __m128i in[8], __m128i out[8]) {
	const __m128i t0 = _mm_unpacklo_epi8(in[0], in[1]);
	const __m128i t1 = _mm_unpacklo_epi8(in[2], in[3]);
	const __m128i t2 = _mm_unpacklo_epi8(in[4], in[5]);
	const __m128i t3 = _mm_unpacklo_epi8(in[6], in[7]);
	const __m128i u0 = _mm_unpacklo_epi16(t0, t1);
	const __m128i u1 = _mm_unpackhi_epi16(t0, t1);
	const __m128i u2 = _mm_unpacklo_epi16(t2, t3);
	const __m128i u3 = _mm_unpackhi_epi16(t2, t3);
	const __m128i v0 = _mm_unpacklo_epi32(u0, u2);
	const __m128i v1 = _mm_unpackhi_epi32(u0, u2);
	const __m128i v2 = _mm_unpacklo_epi32(u1, u3);
	const __m128i v3 = _mm_unpackhi_epi32(u1, u3);

	out[0] = v0;
	out[1] = _mm_srli_si128(v0, 8);
	out[2] = v1;
	out[3] = _mm_srli_si128(v1, 8);
	out[4] = v2;
	out[5] = _mm_srli_si128(v2, 8);
	out[6] = v3;
	out[7] = _mm_srli_si128(v3, 8);
}

static inline void load_vertical_edge8_sse2(const uint8_t* src_q0, int stride, __m128i cols[8]) {
	__m128i rows[8];
	for (int i = 0; i < 8; i++) rows[i] = _mm_loadl_epi64((const __m128i*)(src_q0 + (ptrdiff_t)i * stride - 4));
	transpose_8x8_low_sse2(rows, cols);
}

static inline void store_vertical_edge8_sse2(uint8_t* src_q0, int stride, const __m128i cols[8]) {
	__m128i rows[8];
	transpose_8x8_low_sse2(cols, rows);
	for (int i = 0; i < 8; i++) _mm_storel_epi64((__m128i*)(src_q0 + (ptrdiff_t)i * stride - 4), rows[i]);
}

static inline __m128i clip_u8_from_epi16_sse2(__m128i lo, __m128i hi) {
	const __m128i zero = _mm_setzero_si128();
	const __m128i max = _mm_set1_epi16(255);
	lo = _mm_min_epi16(_mm_max_epi16(lo, zero), max);
	hi = _mm_min_epi16(_mm_max_epi16(hi, zero), max);
	return _mm_packus_epi16(lo, hi);
}

static inline void filter_common_outer_sse2(__m128i p1,
                                            __m128i p0,
                                            __m128i q0,
                                            __m128i q1,
                                            __m128i* out_p0,
                                            __m128i* out_q0) {
	const __m128i zero = _mm_setzero_si128();
	const __m128i min_u8 = _mm_setzero_si128();
	const __m128i max_u8 = _mm_set1_epi16(255);
	const __m128i c3 = _mm_set1_epi16(3);

	__m128i p1_lo = _mm_unpacklo_epi8(p1, zero);
	__m128i p1_hi = _mm_unpackhi_epi8(p1, zero);
	__m128i p0_lo = _mm_unpacklo_epi8(p0, zero);
	__m128i p0_hi = _mm_unpackhi_epi8(p0, zero);
	__m128i q0_lo = _mm_unpacklo_epi8(q0, zero);
	__m128i q0_hi = _mm_unpackhi_epi8(q0, zero);
	__m128i q1_lo = _mm_unpacklo_epi8(q1, zero);
	__m128i q1_hi = _mm_unpackhi_epi8(q1, zero);

	__m128i a_lo = _mm_add_epi16(_mm_mullo_epi16(_mm_sub_epi16(q0_lo, p0_lo), c3),
	                             clamp_i8range_epi16_sse2(_mm_sub_epi16(p1_lo, q1_lo)));
	__m128i a_hi = _mm_add_epi16(_mm_mullo_epi16(_mm_sub_epi16(q0_hi, p0_hi), c3),
	                             clamp_i8range_epi16_sse2(_mm_sub_epi16(p1_hi, q1_hi)));
	a_lo = clamp_i8range_epi16_sse2(a_lo);
	a_hi = clamp_i8range_epi16_sse2(a_hi);

	const __m128i f1_lo = _mm_srai_epi16(clamp_i8range_epi16_sse2(_mm_add_epi16(a_lo, _mm_set1_epi16(4))), 3);
	const __m128i f1_hi = _mm_srai_epi16(clamp_i8range_epi16_sse2(_mm_add_epi16(a_hi, _mm_set1_epi16(4))), 3);
	const __m128i f2_lo = _mm_srai_epi16(clamp_i8range_epi16_sse2(_mm_add_epi16(a_lo, _mm_set1_epi16(3))), 3);
	const __m128i f2_hi = _mm_srai_epi16(clamp_i8range_epi16_sse2(_mm_add_epi16(a_hi, _mm_set1_epi16(3))), 3);

	__m128i new_p0_lo = _mm_add_epi16(p0_lo, f2_lo);
	__m128i new_p0_hi = _mm_add_epi16(p0_hi, f2_hi);
	__m128i new_q0_lo = _mm_sub_epi16(q0_lo, f1_lo);
	__m128i new_q0_hi = _mm_sub_epi16(q0_hi, f1_hi);
	new_p0_lo = _mm_min_epi16(_mm_max_epi16(new_p0_lo, min_u8), max_u8);
	new_p0_hi = _mm_min_epi16(_mm_max_epi16(new_p0_hi, min_u8), max_u8);
	new_q0_lo = _mm_min_epi16(_mm_max_epi16(new_q0_lo, min_u8), max_u8);
	new_q0_hi = _mm_min_epi16(_mm_max_epi16(new_q0_hi, min_u8), max_u8);

	*out_p0 = _mm_packus_epi16(new_p0_lo, new_p0_hi);
	*out_q0 = _mm_packus_epi16(new_q0_lo, new_q0_hi);
}

static inline void calc_w_sse2(__m128i p1,
                               __m128i p0,
                               __m128i q0,
                               __m128i q1,
                               __m128i* w_lo,
                               __m128i* w_hi,
                               __m128i* p2_unused) {
	(void)p2_unused;
	const __m128i zero = _mm_setzero_si128();
	const __m128i c3 = _mm_set1_epi16(3);
	const __m128i p1_lo = _mm_unpacklo_epi8(p1, zero);
	const __m128i p1_hi = _mm_unpackhi_epi8(p1, zero);
	const __m128i p0_lo = _mm_unpacklo_epi8(p0, zero);
	const __m128i p0_hi = _mm_unpackhi_epi8(p0, zero);
	const __m128i q0_lo = _mm_unpacklo_epi8(q0, zero);
	const __m128i q0_hi = _mm_unpackhi_epi8(q0, zero);
	const __m128i q1_lo = _mm_unpacklo_epi8(q1, zero);
	const __m128i q1_hi = _mm_unpackhi_epi8(q1, zero);
	*w_lo = clamp_i8range_epi16_sse2(_mm_add_epi16(_mm_mullo_epi16(_mm_sub_epi16(q0_lo, p0_lo), c3),
	                                              clamp_i8range_epi16_sse2(_mm_sub_epi16(p1_lo, q1_lo))));
	*w_hi = clamp_i8range_epi16_sse2(_mm_add_epi16(_mm_mullo_epi16(_mm_sub_epi16(q0_hi, p0_hi), c3),
	                                              clamp_i8range_epi16_sse2(_mm_sub_epi16(p1_hi, q1_hi))));
}

static inline __m128i apply_delta_sse2(__m128i pix, __m128i delta_lo, __m128i delta_hi, int subtract_delta) {
	const __m128i zero = _mm_setzero_si128();
	__m128i lo = _mm_unpacklo_epi8(pix, zero);
	__m128i hi = _mm_unpackhi_epi8(pix, zero);
	if (subtract_delta) {
		lo = _mm_sub_epi16(lo, delta_lo);
		hi = _mm_sub_epi16(hi, delta_hi);
	} else {
		lo = _mm_add_epi16(lo, delta_lo);
		hi = _mm_add_epi16(hi, delta_hi);
	}
	return clip_u8_from_epi16_sse2(lo, hi);
}

static void filter_mb_h_edge_width_sse2(uint8_t* src_q0,
                                        int stride,
                                        int edge_limit,
                                        int interior_limit,
                                        int hev_threshold,
                                        int width) {
	uint8_t* p2_ptr = src_q0 - 3 * stride;
	uint8_t* p1_ptr = src_q0 - 2 * stride;
	uint8_t* p0_ptr = src_q0 - stride;
	uint8_t* q0_ptr = src_q0;
	uint8_t* q1_ptr = src_q0 + stride;
	uint8_t* q2_ptr = src_q0 + 2 * stride;

	const __m128i p3 = load_u8_width_sse2(src_q0 - 4 * stride, width);
	const __m128i p2 = load_u8_width_sse2(p2_ptr, width);
	const __m128i p1 = load_u8_width_sse2(p1_ptr, width);
	const __m128i p0 = load_u8_width_sse2(p0_ptr, width);
	const __m128i q0 = load_u8_width_sse2(q0_ptr, width);
	const __m128i q1 = load_u8_width_sse2(q1_ptr, width);
	const __m128i q2 = load_u8_width_sse2(q2_ptr, width);
	const __m128i q3 = load_u8_width_sse2(src_q0 + 3 * stride, width);
	const __m128i threshold = normal_threshold_mask_sse2(p3, p2, p1, p0, q0, q1, q2, q3, edge_limit, interior_limit);
	const __m128i hev = hev_mask_sse2(p1, p0, q0, q1, hev_threshold);

	__m128i outer_p0;
	__m128i outer_q0;
	filter_common_outer_sse2(p1, p0, q0, q1, &outer_p0, &outer_q0);

	__m128i w_lo;
	__m128i w_hi;
	calc_w_sse2(p1, p0, q0, q1, &w_lo, &w_hi, 0);
	const __m128i a27_lo = _mm_srai_epi16(_mm_add_epi16(_mm_mullo_epi16(w_lo, _mm_set1_epi16(27)), _mm_set1_epi16(63)), 7);
	const __m128i a27_hi = _mm_srai_epi16(_mm_add_epi16(_mm_mullo_epi16(w_hi, _mm_set1_epi16(27)), _mm_set1_epi16(63)), 7);
	const __m128i a18_lo = _mm_srai_epi16(_mm_add_epi16(_mm_mullo_epi16(w_lo, _mm_set1_epi16(18)), _mm_set1_epi16(63)), 7);
	const __m128i a18_hi = _mm_srai_epi16(_mm_add_epi16(_mm_mullo_epi16(w_hi, _mm_set1_epi16(18)), _mm_set1_epi16(63)), 7);
	const __m128i a9_lo = _mm_srai_epi16(_mm_add_epi16(_mm_mullo_epi16(w_lo, _mm_set1_epi16(9)), _mm_set1_epi16(63)), 7);
	const __m128i a9_hi = _mm_srai_epi16(_mm_add_epi16(_mm_mullo_epi16(w_hi, _mm_set1_epi16(9)), _mm_set1_epi16(63)), 7);

	const __m128i mb_p0 = apply_delta_sse2(p0, a27_lo, a27_hi, 0);
	const __m128i mb_q0 = apply_delta_sse2(q0, a27_lo, a27_hi, 1);
	const __m128i mb_p1 = apply_delta_sse2(p1, a18_lo, a18_hi, 0);
	const __m128i mb_q1 = apply_delta_sse2(q1, a18_lo, a18_hi, 1);
	const __m128i mb_p2 = apply_delta_sse2(p2, a9_lo, a9_hi, 0);
	const __m128i mb_q2 = apply_delta_sse2(q2, a9_lo, a9_hi, 1);

	store_u8_width_sse2(p2_ptr, select_u8_sse2(threshold, select_u8_sse2(hev, p2, mb_p2), p2), width);
	store_u8_width_sse2(p1_ptr, select_u8_sse2(threshold, select_u8_sse2(hev, p1, mb_p1), p1), width);
	store_u8_width_sse2(p0_ptr, select_u8_sse2(threshold, select_u8_sse2(hev, outer_p0, mb_p0), p0), width);
	store_u8_width_sse2(q0_ptr, select_u8_sse2(threshold, select_u8_sse2(hev, outer_q0, mb_q0), q0), width);
	store_u8_width_sse2(q1_ptr, select_u8_sse2(threshold, select_u8_sse2(hev, q1, mb_q1), q1), width);
	store_u8_width_sse2(q2_ptr, select_u8_sse2(threshold, select_u8_sse2(hev, q2, mb_q2), q2), width);
}

void vp8_filter_mb_h_edge_sse2(uint8_t* src_q0, int stride, int edge_limit, int interior_limit, int hev_threshold) {
	filter_mb_h_edge_width_sse2(src_q0, stride, edge_limit, interior_limit, hev_threshold, 16);
}

void vp8_filter_mb_h_edge8_sse2(uint8_t* src_q0, int stride, int edge_limit, int interior_limit, int hev_threshold) {
	filter_mb_h_edge_width_sse2(src_q0, stride, edge_limit, interior_limit, hev_threshold, 8);
}

static void filter_subblock_h_edge_width_sse2(uint8_t* src_q0,
                                              int stride,
                                              int edge_limit,
                                              int interior_limit,
                                              int hev_threshold,
                                              int width) {
	uint8_t* p1_ptr = src_q0 - 2 * stride;
	uint8_t* p0_ptr = src_q0 - stride;
	uint8_t* q0_ptr = src_q0;
	uint8_t* q1_ptr = src_q0 + stride;

	const __m128i p3 = load_u8_width_sse2(src_q0 - 4 * stride, width);
	const __m128i p2 = load_u8_width_sse2(src_q0 - 3 * stride, width);
	const __m128i p1 = load_u8_width_sse2(p1_ptr, width);
	const __m128i p0 = load_u8_width_sse2(p0_ptr, width);
	const __m128i q0 = load_u8_width_sse2(q0_ptr, width);
	const __m128i q1 = load_u8_width_sse2(q1_ptr, width);
	const __m128i q2 = load_u8_width_sse2(src_q0 + 2 * stride, width);
	const __m128i q3 = load_u8_width_sse2(src_q0 + 3 * stride, width);
	const __m128i threshold = normal_threshold_mask_sse2(p3, p2, p1, p0, q0, q1, q2, q3, edge_limit, interior_limit);
	const __m128i hev = hev_mask_sse2(p1, p0, q0, q1, hev_threshold);

	__m128i outer_p0;
	__m128i outer_q0;
	filter_common_outer_sse2(p1, p0, q0, q1, &outer_p0, &outer_q0);

	const __m128i zero = _mm_setzero_si128();
	const __m128i c3 = _mm_set1_epi16(3);
	const __m128i p0_lo = _mm_unpacklo_epi8(p0, zero);
	const __m128i p0_hi = _mm_unpackhi_epi8(p0, zero);
	const __m128i q0_lo = _mm_unpacklo_epi8(q0, zero);
	const __m128i q0_hi = _mm_unpackhi_epi8(q0, zero);
	__m128i a_lo = clamp_i8range_epi16_sse2(_mm_mullo_epi16(_mm_sub_epi16(q0_lo, p0_lo), c3));
	__m128i a_hi = clamp_i8range_epi16_sse2(_mm_mullo_epi16(_mm_sub_epi16(q0_hi, p0_hi), c3));
	const __m128i f1_lo = _mm_srai_epi16(clamp_i8range_epi16_sse2(_mm_add_epi16(a_lo, _mm_set1_epi16(4))), 3);
	const __m128i f1_hi = _mm_srai_epi16(clamp_i8range_epi16_sse2(_mm_add_epi16(a_hi, _mm_set1_epi16(4))), 3);
	const __m128i f2_lo = _mm_srai_epi16(clamp_i8range_epi16_sse2(_mm_add_epi16(a_lo, _mm_set1_epi16(3))), 3);
	const __m128i f2_hi = _mm_srai_epi16(clamp_i8range_epi16_sse2(_mm_add_epi16(a_hi, _mm_set1_epi16(3))), 3);
	const __m128i a2_lo = _mm_srai_epi16(_mm_add_epi16(f1_lo, _mm_set1_epi16(1)), 1);
	const __m128i a2_hi = _mm_srai_epi16(_mm_add_epi16(f1_hi, _mm_set1_epi16(1)), 1);

	const __m128i no_p0 = apply_delta_sse2(p0, f2_lo, f2_hi, 0);
	const __m128i no_q0 = apply_delta_sse2(q0, f1_lo, f1_hi, 1);
	const __m128i no_p1 = apply_delta_sse2(p1, a2_lo, a2_hi, 0);
	const __m128i no_q1 = apply_delta_sse2(q1, a2_lo, a2_hi, 1);

	store_u8_width_sse2(p1_ptr, select_u8_sse2(threshold, select_u8_sse2(hev, p1, no_p1), p1), width);
	store_u8_width_sse2(p0_ptr, select_u8_sse2(threshold, select_u8_sse2(hev, outer_p0, no_p0), p0), width);
	store_u8_width_sse2(q0_ptr, select_u8_sse2(threshold, select_u8_sse2(hev, outer_q0, no_q0), q0), width);
	store_u8_width_sse2(q1_ptr, select_u8_sse2(threshold, select_u8_sse2(hev, q1, no_q1), q1), width);
}

void vp8_filter_subblock_h_edge_sse2(uint8_t* src_q0,
                                     int stride,
                                     int edge_limit,
                                     int interior_limit,
                                     int hev_threshold) {
	filter_subblock_h_edge_width_sse2(src_q0, stride, edge_limit, interior_limit, hev_threshold, 16);
}

void vp8_filter_subblock_h_edge8_sse2(uint8_t* src_q0,
                                      int stride,
                                      int edge_limit,
                                      int interior_limit,
                                      int hev_threshold) {
	filter_subblock_h_edge_width_sse2(src_q0, stride, edge_limit, interior_limit, hev_threshold, 8);
}

static void filter_mb_v_edge_8rows_sse2(uint8_t* src_q0, int stride, int edge_limit, int interior_limit, int hev_threshold) {
	__m128i cols[8];
	load_vertical_edge8_sse2(src_q0, stride, cols);

	const __m128i p3 = cols[0];
	const __m128i p2 = cols[1];
	const __m128i p1 = cols[2];
	const __m128i p0 = cols[3];
	const __m128i q0 = cols[4];
	const __m128i q1 = cols[5];
	const __m128i q2 = cols[6];
	const __m128i q3 = cols[7];
	const __m128i threshold = normal_threshold_mask_sse2(p3, p2, p1, p0, q0, q1, q2, q3, edge_limit, interior_limit);
	const __m128i hev = hev_mask_sse2(p1, p0, q0, q1, hev_threshold);

	__m128i outer_p0;
	__m128i outer_q0;
	filter_common_outer_sse2(p1, p0, q0, q1, &outer_p0, &outer_q0);

	__m128i w_lo;
	__m128i w_hi;
	calc_w_sse2(p1, p0, q0, q1, &w_lo, &w_hi, 0);
	const __m128i a27_lo = _mm_srai_epi16(_mm_add_epi16(_mm_mullo_epi16(w_lo, _mm_set1_epi16(27)), _mm_set1_epi16(63)), 7);
	const __m128i a27_hi = _mm_srai_epi16(_mm_add_epi16(_mm_mullo_epi16(w_hi, _mm_set1_epi16(27)), _mm_set1_epi16(63)), 7);
	const __m128i a18_lo = _mm_srai_epi16(_mm_add_epi16(_mm_mullo_epi16(w_lo, _mm_set1_epi16(18)), _mm_set1_epi16(63)), 7);
	const __m128i a18_hi = _mm_srai_epi16(_mm_add_epi16(_mm_mullo_epi16(w_hi, _mm_set1_epi16(18)), _mm_set1_epi16(63)), 7);
	const __m128i a9_lo = _mm_srai_epi16(_mm_add_epi16(_mm_mullo_epi16(w_lo, _mm_set1_epi16(9)), _mm_set1_epi16(63)), 7);
	const __m128i a9_hi = _mm_srai_epi16(_mm_add_epi16(_mm_mullo_epi16(w_hi, _mm_set1_epi16(9)), _mm_set1_epi16(63)), 7);

	const __m128i mb_p0 = apply_delta_sse2(p0, a27_lo, a27_hi, 0);
	const __m128i mb_q0 = apply_delta_sse2(q0, a27_lo, a27_hi, 1);
	const __m128i mb_p1 = apply_delta_sse2(p1, a18_lo, a18_hi, 0);
	const __m128i mb_q1 = apply_delta_sse2(q1, a18_lo, a18_hi, 1);
	const __m128i mb_p2 = apply_delta_sse2(p2, a9_lo, a9_hi, 0);
	const __m128i mb_q2 = apply_delta_sse2(q2, a9_lo, a9_hi, 1);

	cols[1] = select_u8_sse2(threshold, select_u8_sse2(hev, p2, mb_p2), p2);
	cols[2] = select_u8_sse2(threshold, select_u8_sse2(hev, p1, mb_p1), p1);
	cols[3] = select_u8_sse2(threshold, select_u8_sse2(hev, outer_p0, mb_p0), p0);
	cols[4] = select_u8_sse2(threshold, select_u8_sse2(hev, outer_q0, mb_q0), q0);
	cols[5] = select_u8_sse2(threshold, select_u8_sse2(hev, q1, mb_q1), q1);
	cols[6] = select_u8_sse2(threshold, select_u8_sse2(hev, q2, mb_q2), q2);
	store_vertical_edge8_sse2(src_q0, stride, cols);
}

static void filter_subblock_v_edge_8rows_sse2(uint8_t* src_q0,
                                              int stride,
                                              int edge_limit,
                                              int interior_limit,
                                              int hev_threshold) {
	__m128i cols[8];
	load_vertical_edge8_sse2(src_q0, stride, cols);

	const __m128i p3 = cols[0];
	const __m128i p2 = cols[1];
	const __m128i p1 = cols[2];
	const __m128i p0 = cols[3];
	const __m128i q0 = cols[4];
	const __m128i q1 = cols[5];
	const __m128i q2 = cols[6];
	const __m128i q3 = cols[7];
	const __m128i threshold = normal_threshold_mask_sse2(p3, p2, p1, p0, q0, q1, q2, q3, edge_limit, interior_limit);
	const __m128i hev = hev_mask_sse2(p1, p0, q0, q1, hev_threshold);

	__m128i outer_p0;
	__m128i outer_q0;
	filter_common_outer_sse2(p1, p0, q0, q1, &outer_p0, &outer_q0);

	const __m128i zero = _mm_setzero_si128();
	const __m128i c3 = _mm_set1_epi16(3);
	const __m128i p0_lo = _mm_unpacklo_epi8(p0, zero);
	const __m128i p0_hi = _mm_unpackhi_epi8(p0, zero);
	const __m128i q0_lo = _mm_unpacklo_epi8(q0, zero);
	const __m128i q0_hi = _mm_unpackhi_epi8(q0, zero);
	__m128i a_lo = clamp_i8range_epi16_sse2(_mm_mullo_epi16(_mm_sub_epi16(q0_lo, p0_lo), c3));
	__m128i a_hi = clamp_i8range_epi16_sse2(_mm_mullo_epi16(_mm_sub_epi16(q0_hi, p0_hi), c3));
	const __m128i f1_lo = _mm_srai_epi16(clamp_i8range_epi16_sse2(_mm_add_epi16(a_lo, _mm_set1_epi16(4))), 3);
	const __m128i f1_hi = _mm_srai_epi16(clamp_i8range_epi16_sse2(_mm_add_epi16(a_hi, _mm_set1_epi16(4))), 3);
	const __m128i f2_lo = _mm_srai_epi16(clamp_i8range_epi16_sse2(_mm_add_epi16(a_lo, _mm_set1_epi16(3))), 3);
	const __m128i f2_hi = _mm_srai_epi16(clamp_i8range_epi16_sse2(_mm_add_epi16(a_hi, _mm_set1_epi16(3))), 3);
	const __m128i a2_lo = _mm_srai_epi16(_mm_add_epi16(f1_lo, _mm_set1_epi16(1)), 1);
	const __m128i a2_hi = _mm_srai_epi16(_mm_add_epi16(f1_hi, _mm_set1_epi16(1)), 1);

	const __m128i no_p0 = apply_delta_sse2(p0, f2_lo, f2_hi, 0);
	const __m128i no_q0 = apply_delta_sse2(q0, f1_lo, f1_hi, 1);
	const __m128i no_p1 = apply_delta_sse2(p1, a2_lo, a2_hi, 0);
	const __m128i no_q1 = apply_delta_sse2(q1, a2_lo, a2_hi, 1);

	cols[2] = select_u8_sse2(threshold, select_u8_sse2(hev, p1, no_p1), p1);
	cols[3] = select_u8_sse2(threshold, select_u8_sse2(hev, outer_p0, no_p0), p0);
	cols[4] = select_u8_sse2(threshold, select_u8_sse2(hev, outer_q0, no_q0), q0);
	cols[5] = select_u8_sse2(threshold, select_u8_sse2(hev, q1, no_q1), q1);
	store_vertical_edge8_sse2(src_q0, stride, cols);
}

static void filter_v_edge_simple_8rows_sse2(uint8_t* src_q0, int stride, int filter_limit) {
	__m128i cols[8];
	load_vertical_edge8_sse2(src_q0, stride, cols);

	const __m128i p1 = cols[2];
	const __m128i p0 = cols[3];
	const __m128i q0 = cols[4];
	const __m128i q1 = cols[5];
	const __m128i mask = simple_threshold_mask_sse2(p1, p0, q0, q1, filter_limit);

	__m128i new_p0;
	__m128i new_q0;
	filter_common_outer_sse2(p1, p0, q0, q1, &new_p0, &new_q0);

	cols[3] = select_u8_sse2(mask, new_p0, p0);
	cols[4] = select_u8_sse2(mask, new_q0, q0);
	store_vertical_edge8_sse2(src_q0, stride, cols);
}

void vp8_filter_mb_v_edge_sse2(uint8_t* src_q0, int stride, int edge_limit, int interior_limit, int hev_threshold, int rows) {
	for (int i = 0; i < rows; i += 8) {
		filter_mb_v_edge_8rows_sse2(src_q0 + (ptrdiff_t)i * stride, stride, edge_limit, interior_limit, hev_threshold);
	}
}

void vp8_filter_subblock_v_edge_sse2(uint8_t* src_q0,
                                     int stride,
                                     int edge_limit,
                                     int interior_limit,
                                     int hev_threshold,
                                     int rows) {
	for (int i = 0; i < rows; i += 8) {
		filter_subblock_v_edge_8rows_sse2(src_q0 + (ptrdiff_t)i * stride, stride, edge_limit, interior_limit, hev_threshold);
	}
}

void vp8_filter_v_edge_simple_sse2(uint8_t* src_q0, int stride, int filter_limit) {
	filter_v_edge_simple_8rows_sse2(src_q0, stride, filter_limit);
	filter_v_edge_simple_8rows_sse2(src_q0 + (ptrdiff_t)8 * stride, stride, filter_limit);
}

void vp8_filter_h_edge_simple_sse2(uint8_t* src_q0, int stride, int filter_limit) {
	uint8_t* p1_ptr = src_q0 - 2 * stride;
	uint8_t* p0_ptr = src_q0 - stride;
	uint8_t* q0_ptr = src_q0;
	uint8_t* q1_ptr = src_q0 + stride;

	const __m128i p1 = _mm_loadu_si128((const __m128i*)p1_ptr);
	const __m128i p0 = _mm_loadu_si128((const __m128i*)p0_ptr);
	const __m128i q0 = _mm_loadu_si128((const __m128i*)q0_ptr);
	const __m128i q1 = _mm_loadu_si128((const __m128i*)q1_ptr);
	const __m128i mask = simple_threshold_mask_sse2(p1, p0, q0, q1, filter_limit);

	__m128i new_p0;
	__m128i new_q0;
	filter_common_outer_sse2(p1, p0, q0, q1, &new_p0, &new_q0);

	_mm_storeu_si128((__m128i*)p0_ptr, _mm_or_si128(_mm_and_si128(mask, new_p0), _mm_andnot_si128(mask, p0)));
	_mm_storeu_si128((__m128i*)q0_ptr, _mm_or_si128(_mm_and_si128(mask, new_q0), _mm_andnot_si128(mask, q0)));
}
#else
typedef int vp8_loopfilter_x86_fallback_translation_unit;
#endif
