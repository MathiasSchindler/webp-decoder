#include "vp8_recon.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#if defined(__SSE2__) && (defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86))
#include <emmintrin.h>
#define VP8_RECON_SSE2 1
#endif

#include "vp8_loopfilter.h"
#include "../vp8/vp8_pred.h"
#include "../vp8/vp8_quant.h"
#include "../vp8/vp8_transform.h"

// --- Helpers ---

static inline uint8_t clamp255_i32(int32_t v) {
	if (v < 0) return 0;
	if (v > 255) return 255;
	return (uint8_t)v;
}

#if VP8_RECON_SSE2
static inline uint32_t load_u32le(const uint8_t* p) {
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline void store_u32le(uint8_t* p, uint32_t v) {
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}
#endif

static inline int dc_q(int q) { return vp8_dc_q(q); }
static inline int ac_q(int q) { return vp8_ac_q(q); }

typedef enum {
	TOKEN_BLOCK_Y1 = 0,
	TOKEN_BLOCK_UV = 1,
	TOKEN_BLOCK_Y2 = 2,
} TokenBlock;

typedef struct {
	int quant_idx;
	int factor[3][2];
} DequantFactors;

static void dequant_init(DequantFactors* dqf, const Vp8DecodedFrame* decoded) {
	// Mirrors RFC 6386 reference dequant_init().
	int seg_count = decoded->segmentation_enabled ? 4 : 1;
	for (int i = 0; i < seg_count; i++) {
		int q = (int)decoded->q_index;
		if (decoded->segmentation_enabled) {
			q = decoded->segmentation_abs ? decoded->seg_quant_idx[i] : (q + decoded->seg_quant_idx[i]);
		}
		dqf[i].quant_idx = q;

		dqf[i].factor[TOKEN_BLOCK_Y1][0] = dc_q(q + decoded->y1_dc_delta_q);
		dqf[i].factor[TOKEN_BLOCK_Y1][1] = ac_q(q);
		dqf[i].factor[TOKEN_BLOCK_UV][0] = dc_q(q + decoded->uv_dc_delta_q);
		dqf[i].factor[TOKEN_BLOCK_UV][1] = ac_q(q + decoded->uv_ac_delta_q);
		dqf[i].factor[TOKEN_BLOCK_Y2][0] = dc_q(q + decoded->y2_dc_delta_q) * 2;
		dqf[i].factor[TOKEN_BLOCK_Y2][1] = ac_q(q + decoded->y2_ac_delta_q) * 155 / 100;
		if (dqf[i].factor[TOKEN_BLOCK_Y2][1] < 8) dqf[i].factor[TOKEN_BLOCK_Y2][1] = 8;
		if (dqf[i].factor[TOKEN_BLOCK_UV][0] > 132) dqf[i].factor[TOKEN_BLOCK_UV][0] = 132;
	}
}

// --- Inverse transforms from RFC 6386 ---

static void inv_wht4x4(const int16_t* in, int16_t* out) { vp8_inv_wht4x4(in, out); }

static void inv_dct4x4(const int16_t* input, int16_t* output) { vp8_inv_dct4x4(input, output); }

static void inv_wht4x4_dc_only(int16_t dc, int16_t* out) { vp8_inv_wht4x4_dc_only(dc, out); }

static int inv_dct4x4_dc_value(int16_t dc) { return ((int)dc + 4) >> 3; }

typedef enum {
	COEFFS_ZERO = 0,
	COEFFS_DC_ONLY = 1,
	COEFFS_HAS_AC = 2,
} CoeffClass;

static inline int coeffs_have_nonzero_ac(const int16_t* coeffs) {
#if VP8_RECON_SSE2
	const __m128i zero = _mm_setzero_si128();
	__m128i a = _mm_loadu_si128((const __m128i*)(const void*)(coeffs + 1));
	__m128i b = _mm_loadu_si128((const __m128i*)(const void*)(coeffs + 8));
	__m128i z = _mm_cmpeq_epi16(_mm_or_si128(a, b), zero);
	return _mm_movemask_epi8(z) != 0xffff;
#else
	for (int i = 1; i < 16; i++) {
		if (coeffs[i] != 0) return 1;
	}
	return 0;
#endif
}

static CoeffClass coeffs_classify(const int16_t* coeffs, int first) {
	if (first == 1) {
		return coeffs_have_nonzero_ac(coeffs) ? COEFFS_HAS_AC : (coeffs[0] == 0 ? COEFFS_ZERO : COEFFS_DC_ONLY);
	}
	for (int i = first; i < 16; i++) {
		if (coeffs[i] != 0) return COEFFS_HAS_AC;
	}
	return (first == 0 || coeffs[0] == 0) ? COEFFS_ZERO : COEFFS_DC_ONLY;
}

static void copy_block4(uint8_t* dst, uint32_t dst_stride, const uint8_t* pred, uint32_t pred_stride) {
	if (dst == pred && dst_stride == pred_stride) return;
	for (uint32_t rr = 0; rr < 4; rr++) {
		const uint8_t* s = pred + (size_t)rr * pred_stride;
		uint8_t* d = dst + (size_t)rr * dst_stride;
		d[0] = s[0];
		d[1] = s[1];
		d[2] = s[2];
		d[3] = s[3];
	}
}

static void add_block4_constant(uint8_t* dst, uint32_t dst_stride, const uint8_t* pred, uint32_t pred_stride,
                                int delta) {
#if VP8_RECON_SSE2
	const __m128i zero = _mm_setzero_si128();
	const __m128i d = _mm_set1_epi16((int16_t)delta);
	for (uint32_t rr = 0; rr < 4; rr++) {
		__m128i p = _mm_cvtsi32_si128((int)load_u32le(pred + (size_t)rr * pred_stride));
		p = _mm_unpacklo_epi8(p, zero);
		p = _mm_adds_epi16(p, d);
		p = _mm_packus_epi16(p, zero);
		store_u32le(dst + (size_t)rr * dst_stride, (uint32_t)_mm_cvtsi128_si32(p));
	}
	return;
#endif
	for (uint32_t rr = 0; rr < 4; rr++) {
		for (uint32_t cc = 0; cc < 4; cc++) {
			dst[(size_t)rr * dst_stride + cc] = clamp255_i32((int32_t)pred[(size_t)rr * pred_stride + cc] + delta);
		}
	}
}

static void add_block4_residue(uint8_t* dst, uint32_t dst_stride, const uint8_t* pred, uint32_t pred_stride,
                               const int16_t res[16]) {
#if VP8_RECON_SSE2
	const __m128i zero = _mm_setzero_si128();
	for (uint32_t rr = 0; rr < 4; rr++) {
		__m128i p = _mm_cvtsi32_si128((int)load_u32le(pred + (size_t)rr * pred_stride));
		__m128i r = _mm_loadl_epi64((const __m128i*)(const void*)(res + (size_t)rr * 4u));
		p = _mm_unpacklo_epi8(p, zero);
		p = _mm_adds_epi16(p, r);
		p = _mm_packus_epi16(p, zero);
		store_u32le(dst + (size_t)rr * dst_stride, (uint32_t)_mm_cvtsi128_si32(p));
	}
	return;
#endif
	for (uint32_t rr = 0; rr < 4; rr++) {
		for (uint32_t cc = 0; cc < 4; cc++) {
			dst[(size_t)rr * dst_stride + cc] =
			    clamp255_i32((int32_t)pred[(size_t)rr * pred_stride + cc] + (int32_t)res[(int)rr * 4 + (int)cc]);
		}
	}
}

static void reconstruct_block4(uint8_t* dst, uint32_t dst_stride, const uint8_t* pred, uint32_t pred_stride,
                               const int16_t* coeffs, int dc_factor, int ac_factor) {
	CoeffClass cc = coeffs_classify(coeffs, 1);
	if (cc == COEFFS_ZERO && coeffs[0] == 0) {
		copy_block4(dst, dst_stride, pred, pred_stride);
		return;
	}

	if (cc != COEFFS_HAS_AC) {
		int16_t dc = (int16_t)(coeffs[0] * dc_factor);
		add_block4_constant(dst, dst_stride, pred, pred_stride, inv_dct4x4_dc_value(dc));
		return;
	}

	int16_t cdeq[16];
	for (int i = 0; i < 16; i++) {
		int fct = (i == 0) ? dc_factor : ac_factor;
		cdeq[i] = (int16_t)(coeffs[i] * fct);
	}
	int16_t res[16];
	inv_dct4x4(cdeq, res);
	add_block4_residue(dst, dst_stride, pred, pred_stride, res);
}

static void reconstruct_block4_with_dc(uint8_t* dst, uint32_t dst_stride, const uint8_t* pred, uint32_t pred_stride,
                                       int16_t dc, const int16_t* coeffs, int ac_factor) {
	if (coeffs_classify(coeffs, 1) != COEFFS_HAS_AC) {
		if (dc == 0) {
			copy_block4(dst, dst_stride, pred, pred_stride);
		} else {
			add_block4_constant(dst, dst_stride, pred, pred_stride, inv_dct4x4_dc_value(dc));
		}
		return;
	}

	int16_t cdeq[16];
	cdeq[0] = dc;
	for (int i = 1; i < 16; i++) cdeq[i] = (int16_t)(coeffs[i] * ac_factor);
	int16_t res[16];
	inv_dct4x4(cdeq, res);
	add_block4_residue(dst, dst_stride, pred, pred_stride, res);
}

// --- Prediction ---

static void pred_dc(uint8_t* dst, uint32_t stride, const uint8_t* A, const uint8_t* L, uint32_t n, int have_above,
                    int have_left, uint8_t above_oob, uint8_t left_oob) {
	if (!have_above && !have_left) {
		for (uint32_t r = 0; r < n; r++) {
			uint8_t* row = dst + (size_t)r * stride;
			for (uint32_t c = 0; c < n; c++) row[c] = 128;
		}
		return;
	}
	int sum = 0;
	int shf = 0;
	if (have_above && have_left) {
		for (uint32_t i = 0; i < n; i++) sum += (int)A[i] + (int)L[i];
		shf = (n == 16) ? 5 : (n == 8) ? 4 : 3;
	} else if (have_left) {
		for (uint32_t i = 0; i < n; i++) sum += (int)L[i];
		shf = (n == 16) ? 4 : (n == 8) ? 3 : 2;
	} else {
		for (uint32_t i = 0; i < n; i++) sum += (int)A[i];
		shf = (n == 16) ? 4 : (n == 8) ? 3 : 2;
	}
	uint8_t v = (uint8_t)((sum + (1 << (shf - 1))) >> shf);
	(void)above_oob;
	(void)left_oob;
	for (uint32_t r = 0; r < n; r++) {
		uint8_t* row = dst + (size_t)r * stride;
		for (uint32_t c = 0; c < n; c++) row[c] = v;
	}
}

static void pred_v(uint8_t* dst, uint32_t stride, const uint8_t* A, uint32_t n, int have_above, uint8_t above_oob) {
	for (uint32_t r = 0; r < n; r++) {
		if (have_above) {
			uint8_t* row = dst + (size_t)r * stride;
			for (uint32_t c = 0; c < n; c++) row[c] = A[c];
		} else {
			memset(dst + (size_t)r * stride, above_oob, n);
		}
	}
}

static void pred_h(uint8_t* dst, uint32_t stride, const uint8_t* L, uint32_t n, int have_left, uint8_t left_oob) {
	for (uint32_t r = 0; r < n; r++) {
		uint8_t v = have_left ? L[r] : left_oob;
		uint8_t* row = dst + (size_t)r * stride;
		for (uint32_t c = 0; c < n; c++) row[c] = v;
	}
}

static void pred_tm(uint8_t* dst, uint32_t stride, const uint8_t* A, const uint8_t* L, uint32_t n, int have_above,
                    int have_left, uint8_t above_oob, uint8_t left_oob) {
	uint8_t P = 128;
	if (have_above && have_left) {
		P = A[-1];
	} else if (!have_above && have_left) {
		P = above_oob; // A[-1]
	} else if (have_above && !have_left) {
		P = left_oob; // L[-1]
	} else {
		// For TM_PRED, use the out-of-bounds value for the pixel above-left.
		// RFC 6386 Section 12: pixels above the top row (including above-left) are 127.
		P = above_oob;
	}
	for (uint32_t r = 0; r < n; r++) {
		uint8_t Lv = have_left ? L[r] : left_oob;
		for (uint32_t c = 0; c < n; c++) {
			uint8_t Av = have_above ? A[c] : above_oob;
			dst[r * stride + c] = clamp255_i32((int32_t)Lv + (int32_t)Av - (int32_t)P);
		}
	}
}

static int checked_mul_size(size_t a, size_t b, size_t* out) {
	if (!out) return -1;
	if (a != 0 && b > SIZE_MAX / a) return -1;
	*out = a * b;
	return 0;
}

static int checked_add_size(size_t a, size_t b, size_t* out) {
	if (!out) return -1;
	if (a > SIZE_MAX - b) return -1;
	*out = a + b;
	return 0;
}

static int yuv420_alloc_internal(Yuv420Image* img, uint32_t width, uint32_t height, int initialize) {
	if (!img || width == 0 || height == 0) {
		errno = EINVAL;
		return -1;
	}
	*img = (Yuv420Image){0};
	img->width = width;
	img->height = height;
	img->stride_y = width;
	img->stride_uv = width / 2u + (width & 1u);

	size_t ysz = 0;
	size_t uvsz = 0;
	size_t uv_total = 0;
	size_t total = 0;
	size_t uvh = (size_t)(height / 2u + (height & 1u));
	if (checked_mul_size((size_t)img->stride_y, (size_t)height, &ysz) != 0 ||
	    checked_mul_size((size_t)img->stride_uv, uvh, &uvsz) != 0 ||
	    checked_mul_size(uvsz, 2u, &uv_total) != 0 ||
	    checked_add_size(ysz, uv_total, &total) != 0) {
		*img = (Yuv420Image){0};
		errno = ENOMEM;
		return -1;
	}

	uint8_t* base = (uint8_t*)malloc(total);
	if (!base) {
		*img = (Yuv420Image){0};
		errno = ENOMEM;
		return -1;
	}
	img->y = base;
	img->u = base + ysz;
	img->v = img->u + uvsz;
	if (initialize) {
		memset(img->y, 0, ysz);
		memset(img->u, 128, uvsz);
		memset(img->v, 128, uvsz);
	}
	return 0;
}

int yuv420_alloc(Yuv420Image* img, uint32_t width, uint32_t height) {
	return yuv420_alloc_internal(img, width, height, 1);
}

void yuv420_free(Yuv420Image* img) {
	if (!img) return;
	free(img->y);
	*img = (Yuv420Image){0};
}

static void get_above_row(const uint8_t* plane, uint32_t stride, uint32_t width, uint32_t x, uint32_t y, uint32_t n,
                          uint8_t fill, uint8_t* out) {
	(void)width;
	if (y == 0) {
		for (uint32_t i = 0; i < n; i++) out[i] = fill;
		return;
	}
	const uint8_t* src = plane + (size_t)(y - 1u) * stride + x;
	for (uint32_t i = 0; i < n; i++) out[i] = src[i];
}

static void get_left_col(const uint8_t* plane, uint32_t stride, uint32_t height, uint32_t x, uint32_t y, uint32_t n,
                         uint8_t fill, uint8_t* out) {
	(void)height;
	if (x == 0) {
		for (uint32_t i = 0; i < n; i++) out[i] = fill;
		return;
	}
	uint32_t col = x - 1;
	for (uint32_t i = 0; i < n; i++) {
		out[i] = plane[(y + i) * stride + col];
	}
}

typedef struct {
	const int16_t* y2;
	const int16_t* y;
	const int16_t* u;
	const int16_t* v;
} ReconCoeffView;

static void reconstruct_macroblock_keyframe(Yuv420Image* pad, const Vp8DecodedFrame* decoded, const DequantFactors dqf[4],
                                            uint32_t mb, const Vp8MacroblockSyntax* syntax,
                                            const ReconCoeffView* coeffs) {
	uint32_t mb_cols = decoded->mb_cols;
	uint32_t mb_r = mb / mb_cols;
	uint32_t mb_c = mb - mb_r * mb_cols;
	uint32_t seg = decoded->segmentation_enabled ? (uint32_t)(syntax->segment_id & 3u) : 0u;
	const DequantFactors* q = &dqf[seg];

	uint32_t x = mb_c * 16u;
	uint32_t y = mb_r * 16u;

	uint8_t ymode = syntax->ymode;
	if (ymode == 4) {
		for (uint32_t sb_r = 0; sb_r < 4; sb_r++) {
			for (uint32_t sb_c = 0; sb_c < 4; sb_c++) {
				uint32_t sb = sb_r * 4u + sb_c;
				uint8_t mode = syntax->bmode ? syntax->bmode[sb] : 0;
				uint32_t sx = x + sb_c * 4u;
				uint32_t sy = y + sb_r * 4u;

				uint8_t A8[9];
				const uint8_t* A = NULL;
				uint8_t L4[4];
				if (sy != 0 && sx != 0 && sx + 7u < pad->width && (sb_c != 3 || sb_r == 0)) {
					A = pad->y + (size_t)(sy - 1u) * pad->stride_y + sx;
				} else {
					if (sy == 0) A8[0] = 127;
					else if (sx == 0) A8[0] = 129;
					else A8[0] = pad->y[(sy - 1) * pad->stride_y + (sx - 1)];

					for (uint32_t i = 0; i < 8; i++) {
						if (sy == 0) {
							A8[1 + i] = 127;
							continue;
						}
						uint32_t row = sy - 1;
						uint32_t col;
						if (sb_c == 3 && i >= 4) {
							if (y == 0) {
								A8[1 + i] = 127;
								continue;
							}
							row = y - 1;
							col = x + 16u + (i - 4u);
						} else {
							col = sx + i;
						}
						if (col >= pad->width) col = pad->width - 1;
						A8[1 + i] = pad->y[row * pad->stride_y + col];
					}
					A = &A8[1];
				}

				if (sx == 0) {
					for (uint32_t i = 0; i < 4; i++) L4[i] = 129;
				} else {
					for (uint32_t i = 0; i < 4; i++) {
						L4[i] = pad->y[(sy + i) * pad->stride_y + (sx - 1)];
					}
				}

				uint8_t* dst = pad->y + (size_t)sy * pad->stride_y + sx;
				if (!syntax->has_coeff) {
					vp8_bpred4x4(dst, pad->stride_y, A, L4, mode);
				} else {
					uint8_t B[4][4];
					vp8_bpred4x4(&B[0][0], 4, A, L4, mode);

					const int16_t* cq = coeffs->y + (size_t)sb * 16u;
					reconstruct_block4(dst, pad->stride_y, &B[0][0], 4, cq, q->factor[TOKEN_BLOCK_Y1][0],
					                   q->factor[TOKEN_BLOCK_Y1][1]);
				}
			}
		}
	} else {
		uint8_t pred_y[16 * 16];
		uint8_t* pred_y_dst = syntax->has_coeff ? pred_y : (pad->y + (size_t)y * pad->stride_y + x);
		uint32_t pred_y_stride = syntax->has_coeff ? 16u : pad->stride_y;
		uint8_t A16[20];
		uint8_t L16[16];
		get_above_row(pad->y, pad->stride_y, pad->width, x, y, 16, 127, A16);
		get_left_col(pad->y, pad->stride_y, pad->height, x, y, 16, 129, L16);
		A16[16] = A16[15];
		A16[17] = A16[15];
		A16[18] = A16[15];
		A16[19] = A16[15];
		int have_above = (y != 0);
		int have_left = (x != 0);

		switch (ymode) {
			case 0: pred_dc(pred_y_dst, pred_y_stride, A16, L16, 16, have_above, have_left, 127, 129); break;
			case 1: pred_v(pred_y_dst, pred_y_stride, A16, 16, have_above, 127); break;
			case 2: pred_h(pred_y_dst, pred_y_stride, L16, 16, have_left, 129); break;
			case 3: {
				uint8_t Ap[17];
				Ap[0] = have_above && have_left ? pad->y[(y - 1) * pad->stride_y + (x - 1)] : (have_above ? 129 : 127);
				for (uint32_t i = 0; i < 16; i++) Ap[1 + i] = A16[i];
				pred_tm(pred_y_dst, pred_y_stride, &Ap[1], L16, 16, have_above, have_left, 127, 129);
				break;
			}
			default: pred_dc(pred_y_dst, pred_y_stride, A16, L16, 16, have_above, have_left, 127, 129); break;
		}

		if (syntax->has_coeff) {
			int16_t y2_dc[16];
			const int16_t* y2q = coeffs->y2;
			CoeffClass y2_class = coeffs_classify(y2q, 1);
			if (y2_class == COEFFS_ZERO && y2q[0] == 0) {
				memset(y2_dc, 0, sizeof(y2_dc));
			} else if (y2_class != COEFFS_HAS_AC) {
				int16_t dc = (int16_t)(y2q[0] * q->factor[TOKEN_BLOCK_Y2][0]);
				inv_wht4x4_dc_only(dc, y2_dc);
			} else {
				int16_t y2_deq[16];
				for (int i = 0; i < 16; i++) {
					int fct = (i == 0) ? q->factor[TOKEN_BLOCK_Y2][0] : q->factor[TOKEN_BLOCK_Y2][1];
					y2_deq[i] = (int16_t)(y2q[i] * fct);
				}
				inv_wht4x4(y2_deq, y2_dc);
			}

			for (uint32_t sb_r = 0; sb_r < 4; sb_r++) {
				for (uint32_t sb_c = 0; sb_c < 4; sb_c++) {
					uint32_t sb = sb_r * 4u + sb_c;
					const int16_t* cq = coeffs->y + (size_t)sb * 16u;
					uint32_t bx = sb_c * 4u;
					uint32_t by = sb_r * 4u;
					reconstruct_block4_with_dc(pad->y + (size_t)(y + by) * pad->stride_y + x + bx, pad->stride_y,
					                           pred_y + (size_t)by * 16u + bx, 16, y2_dc[(int)sb_r * 4 + (int)sb_c], cq,
					                           q->factor[TOKEN_BLOCK_Y1][1]);
				}
			}
		}
	}

	uint32_t cx = mb_c * 8u;
	uint32_t cy = mb_r * 8u;
	uint32_t cw = (pad->width + 1u) / 2u;
	uint32_t ch = (pad->height + 1u) / 2u;

	uint8_t pred_u[8 * 8];
	uint8_t pred_vp[8 * 8];
	uint8_t* pred_u_dst = syntax->has_coeff ? pred_u : (pad->u + (size_t)cy * pad->stride_uv + cx);
	uint8_t* pred_v_dst = syntax->has_coeff ? pred_vp : (pad->v + (size_t)cy * pad->stride_uv + cx);
	uint32_t pred_uv_stride = syntax->has_coeff ? 8u : pad->stride_uv;
	uint8_t A8u[8];
	uint8_t L8u[8];
	uint8_t A8v[8];
	uint8_t L8v[8];
	get_above_row(pad->u, pad->stride_uv, cw, cx, cy, 8, 127, A8u);
	get_left_col(pad->u, pad->stride_uv, ch, cx, cy, 8, 129, L8u);
	get_above_row(pad->v, pad->stride_uv, cw, cx, cy, 8, 127, A8v);
	get_left_col(pad->v, pad->stride_uv, ch, cx, cy, 8, 129, L8v);
	int have_above_c = (cy != 0);
	int have_left_c = (cx != 0);
	switch (syntax->uv_mode) {
		case 0:
			pred_dc(pred_u_dst, pred_uv_stride, A8u, L8u, 8, have_above_c, have_left_c, 127, 129);
			pred_dc(pred_v_dst, pred_uv_stride, A8v, L8v, 8, have_above_c, have_left_c, 127, 129);
			break;
		case 1:
			pred_v(pred_u_dst, pred_uv_stride, A8u, 8, have_above_c, 127);
			pred_v(pred_v_dst, pred_uv_stride, A8v, 8, have_above_c, 127);
			break;
		case 2:
			pred_h(pred_u_dst, pred_uv_stride, L8u, 8, have_left_c, 129);
			pred_h(pred_v_dst, pred_uv_stride, L8v, 8, have_left_c, 129);
			break;
		case 3: {
			uint8_t Apu[9];
			uint8_t Apv[9];
			Apu[0] = have_above_c && have_left_c ? pad->u[(cy - 1) * pad->stride_uv + (cx - 1)] : (have_above_c ? 129 : 127);
			Apv[0] = have_above_c && have_left_c ? pad->v[(cy - 1) * pad->stride_uv + (cx - 1)] : (have_above_c ? 129 : 127);
			for (uint32_t i = 0; i < 8; i++) {
				Apu[1 + i] = A8u[i];
				Apv[1 + i] = A8v[i];
			}
			pred_tm(pred_u_dst, pred_uv_stride, &Apu[1], L8u, 8, have_above_c, have_left_c, 127, 129);
			pred_tm(pred_v_dst, pred_uv_stride, &Apv[1], L8v, 8, have_above_c, have_left_c, 127, 129);
			break;
		}
		default:
			pred_dc(pred_u_dst, pred_uv_stride, A8u, L8u, 8, have_above_c, have_left_c, 127, 129);
			pred_dc(pred_v_dst, pred_uv_stride, A8v, L8v, 8, have_above_c, have_left_c, 127, 129);
			break;
	}

	if (!syntax->has_coeff) return;

	for (uint32_t b = 0; b < 4; b++) {
		uint32_t br = b / 2u;
		uint32_t bc = b % 2u;
		const int16_t* cuq = coeffs->u + (size_t)b * 16u;
		const int16_t* cvq = coeffs->v + (size_t)b * 16u;
		uint32_t bx = bc * 4u;
		uint32_t by = br * 4u;
		reconstruct_block4(pad->u + (size_t)(cy + by) * pad->stride_uv + cx + bx, pad->stride_uv,
		                   pred_u + (size_t)by * 8u + bx, 8, cuq, q->factor[TOKEN_BLOCK_UV][0],
		                   q->factor[TOKEN_BLOCK_UV][1]);
		reconstruct_block4(pad->v + (size_t)(cy + by) * pad->stride_uv + cx + bx, pad->stride_uv,
		                   pred_vp + (size_t)by * 8u + bx, 8, cvq, q->factor[TOKEN_BLOCK_UV][0],
		                   q->factor[TOKEN_BLOCK_UV][1]);
	}
}

static int finish_keyframe_yuv(const Vp8KeyFrameHeader* kf, const Vp8DecodedFrame* decoded, Yuv420Image* pad,
                               Yuv420Image* out, int apply_loopfilter) {
	if (apply_loopfilter) {
		if (vp8_loopfilter_apply_keyframe(pad, decoded) != 0) {
			yuv420_free(pad);
			return -1;
		}
	}

	if (pad->width == kf->width && pad->height == kf->height) {
		*out = *pad;
		*pad = (Yuv420Image){0};
		return 0;
	}

	Yuv420Image cropped;
	if (yuv420_alloc_internal(&cropped, kf->width, kf->height, 0) != 0) {
		yuv420_free(pad);
		return -1;
	}
	for (uint32_t yy = 0; yy < cropped.height; yy++) {
		memcpy(&cropped.y[yy * cropped.stride_y], &pad->y[yy * pad->stride_y], cropped.width);
	}
	uint32_t cw_out = (cropped.width + 1u) / 2u;
	uint32_t ch_out = (cropped.height + 1u) / 2u;
	for (uint32_t yy = 0; yy < ch_out; yy++) {
		memcpy(&cropped.u[yy * cropped.stride_uv], &pad->u[yy * pad->stride_uv], cw_out);
		memcpy(&cropped.v[yy * cropped.stride_uv], &pad->v[yy * pad->stride_uv], cw_out);
	}

	yuv420_free(pad);
	*out = cropped;
	return 0;
}

static int vp8_reconstruct_keyframe_yuv_internal(const Vp8KeyFrameHeader* kf, const Vp8DecodedFrame* decoded, Yuv420Image* out,
								  int apply_loopfilter) {
	if (!kf || !decoded || !out) {
		errno = EINVAL;
		return -1;
	}

	if (!decoded->coeff_y2 || !decoded->coeff_y || !decoded->coeff_u || !decoded->coeff_v) {
		errno = EINVAL;
		return -1;
	}

	// Reconstruct into a macroblock-aligned padded buffer first.
	// This matches reference decoders that reconstruct full macroblocks even when the
	// visible frame dimensions are not multiples of 16 (or chroma not multiples of 8).
	uint32_t padded_w = decoded->mb_cols * 16u;
	uint32_t padded_h = decoded->mb_rows * 16u;
	Yuv420Image pad;
	if (yuv420_alloc_internal(&pad, padded_w, padded_h, 0) != 0) return -1;

	DequantFactors dqf[4];
	memset(dqf, 0, sizeof(dqf));
	dequant_init(dqf, decoded);

	uint32_t mb_cols = decoded->mb_cols;
	uint32_t mb_rows = decoded->mb_rows;
	for (uint32_t mb_r = 0; mb_r < mb_rows; mb_r++) {
		for (uint32_t mb_c = 0; mb_c < mb_cols; mb_c++) {
			uint32_t mb = mb_r * mb_cols + mb_c;
			ReconCoeffView coeffs = {
				.y2 = decoded->coeff_y2 + (size_t)mb * 16u,
				.y = decoded->coeff_y + (size_t)mb * 16u * 16u,
				.u = decoded->coeff_u + (size_t)mb * 4u * 16u,
				.v = decoded->coeff_v + (size_t)mb * 4u * 16u,
			};
			Vp8MacroblockSyntax syntax = {
				.segment_id = decoded->segment_id ? decoded->segment_id[mb] : 0,
				.has_coeff = decoded->has_coeff ? decoded->has_coeff[mb] : 1,
				.has_y2 = (uint8_t)(decoded->ymode[mb] != 4),
				.ymode = decoded->ymode[mb],
				.uv_mode = decoded->uv_mode[mb],
				.bmode = decoded->bmode ? (decoded->bmode + (size_t)mb * 16u) : NULL,
			};
			reconstruct_macroblock_keyframe(&pad, decoded, dqf, mb, &syntax, &coeffs);
		}
	}

	return finish_keyframe_yuv(kf, decoded, &pad, out, apply_loopfilter);
}

typedef struct {
	Vp8DecodedFrame* decoded;
	Yuv420Image pad;
	DequantFactors dqf[4];
	int initialized;
	int apply_loopfilter;
} FusedReconState;

static uint8_t* alloc_mb_flags(uint32_t mb_total) {
	size_t bytes = 0;
	if (checked_mul_size((size_t)mb_total, sizeof(uint8_t), &bytes) != 0) {
		errno = ENOMEM;
		return NULL;
	}
	uint8_t* p = (uint8_t*)calloc(1, bytes);
	if (!p) errno = ENOMEM;
	return p;
}

static int fused_recon_visit(void* user, uint32_t mb_index, const Vp8MacroblockSyntax* syntax,
                             const Vp8MacroblockCoeffs* coeffs) {
	FusedReconState* state = (FusedReconState*)user;
	if (!state->initialized) {
		uint32_t padded_w = state->decoded->mb_cols * 16u;
		uint32_t padded_h = state->decoded->mb_rows * 16u;
		if (yuv420_alloc_internal(&state->pad, padded_w, padded_h, 0) != 0) return -1;
		memset(state->dqf, 0, sizeof(state->dqf));
		dequant_init(state->dqf, state->decoded);
		if (state->apply_loopfilter) {
			uint32_t mb_total = state->decoded->mb_total;
			if (state->decoded->segmentation_enabled) {
				state->decoded->segment_id = alloc_mb_flags(mb_total);
				if (!state->decoded->segment_id) return -1;
			}
			state->decoded->has_coeff = alloc_mb_flags(mb_total);
			state->decoded->ymode = alloc_mb_flags(mb_total);
			if (!state->decoded->has_coeff || !state->decoded->ymode) return -1;
		}
		state->initialized = 1;
	}
	if (state->apply_loopfilter) {
		if (state->decoded->segment_id) state->decoded->segment_id[mb_index] = syntax->segment_id;
		state->decoded->has_coeff[mb_index] = syntax->has_coeff;
		state->decoded->ymode[mb_index] = syntax->ymode;
	}

	ReconCoeffView view = {
		.y2 = coeffs->y2,
		.y = &coeffs->y[0][0],
		.u = &coeffs->u[0][0],
		.v = &coeffs->v[0][0],
	};
	reconstruct_macroblock_keyframe(&state->pad, state->decoded, state->dqf, mb_index, syntax, &view);
	return 0;
}

static int vp8_decode_reconstruct_keyframe_yuv_internal(ByteSpan vp8_payload, Yuv420Image* out, int apply_loopfilter) {
	if (!out) {
		errno = EINVAL;
		return -1;
	}
	Vp8KeyFrameHeader kf;
	if (vp8_parse_keyframe_header(vp8_payload, &kf) != 0 || !kf.is_key_frame) {
		errno = EINVAL;
		return -1;
	}

	Vp8DecodedFrame decoded;
	FusedReconState state;
	memset(&state, 0, sizeof(state));
	state.decoded = &decoded;
	state.apply_loopfilter = apply_loopfilter;
	if (vp8_decode_decoded_frame_visit_macroblocks(vp8_payload, &decoded, fused_recon_visit, &state) != 0) {
		yuv420_free(&state.pad);
		vp8_decoded_frame_free(&decoded);
		return -1;
	}
	if (!state.initialized) {
		vp8_decoded_frame_free(&decoded);
		errno = EINVAL;
		return -1;
	}

	int rc = finish_keyframe_yuv(&kf, &decoded, &state.pad, out, apply_loopfilter);
	vp8_decoded_frame_free(&decoded);
	return rc;
}

int vp8_reconstruct_keyframe_yuv(const Vp8KeyFrameHeader* kf, const Vp8DecodedFrame* decoded, Yuv420Image* out) {
	return vp8_reconstruct_keyframe_yuv_internal(kf, decoded, out, 0);
}

int vp8_reconstruct_keyframe_yuv_filtered(const Vp8KeyFrameHeader* kf, const Vp8DecodedFrame* decoded, Yuv420Image* out) {
	return vp8_reconstruct_keyframe_yuv_internal(kf, decoded, out, 1);
}

int vp8_decode_reconstruct_keyframe_yuv(ByteSpan vp8_payload, Yuv420Image* out) {
	return vp8_decode_reconstruct_keyframe_yuv_internal(vp8_payload, out, 0);
}

int vp8_decode_reconstruct_keyframe_yuv_filtered(ByteSpan vp8_payload, Yuv420Image* out) {
	return vp8_decode_reconstruct_keyframe_yuv_internal(vp8_payload, out, 1);
}
