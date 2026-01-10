#include "enc_recon.h"

#include "../enc-m05_intra/enc_transform.h"
#include "../enc-m06_quant/enc_quant.h"
#include "../enc-m07_tokens/enc_vp8_tokens.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

static inline uint8_t clamp255_i32(int32_t v) {
	if (v < 0) return 0;
	if (v > 255) return 255;
	return (uint8_t)v;
}
static int rdo_scale_step_pct(int step, uint32_t pct) {
	if (pct == 0) pct = 100;
	uint64_t v = (uint64_t)(uint32_t)step * (uint64_t)pct;
	v = (v + 50u) / 100u;
	if (v < 1u) v = 1u;
	if (v > 2047u) v = 2047u;
	return (int)v;
}

static uint8_t dc_value(const uint8_t* left, const uint8_t* top, int size, int round, int shift);
static uint8_t load_clamped(const uint8_t* plane, uint32_t stride, uint32_t w, uint32_t h, uint32_t x, uint32_t y);

typedef enum {
	VP8_I16_DC_PRED = 0,
	VP8_I16_V_PRED = 1,
	VP8_I16_H_PRED = 2,
	VP8_I16_TM_PRED = 3,
} Vp8I16Mode;

typedef enum {
	B_DC_PRED = 0,
	B_TM_PRED = 1,
	B_VE_PRED = 2,
	B_HE_PRED = 3,
	B_LD_PRED = 4,
	B_RD_PRED = 5,
	B_VR_PRED = 6,
	B_VL_PRED = 7,
	B_HD_PRED = 8,
	B_HU_PRED = 9,
} Vp8BMode;

static inline uint8_t avg3_u8(uint8_t x, uint8_t y, uint8_t z) { return (uint8_t)((x + y + y + z + 2u) >> 2); }
static inline uint8_t avg2_u8(uint8_t x, uint8_t y) { return (uint8_t)((x + y + 1u) >> 1); }

static inline int16_t rdo_quant_one(int16_t c, int step) {
	// Match enc_vp8_quantize4x4_inplace() rounding.
	if (step <= 0) return 0;
	int v = (int)c;
	int sign = 1;
	if (v < 0) {
		sign = -1;
		v = -v;
	}
	const int q = (v + (step >> 1)) / step;
	const int r = sign * q;
	if (r < -32768) return (int16_t)-32768;
	if (r > 32767) return (int16_t)32767;
	return (int16_t)r;
}

static inline int16_t rdo_quant_one_ac_deadzone_pct(int16_t c, int step, uint32_t deadzone_pct) {
	// Experimental AC quant: enlarge the zero bin to (deadzone_pct/100)*step.
	// This is intentionally used only in bpred-rdo (tuning knob) to avoid
	// perturbing baseline encoders and tests.
	if (step <= 0) return 0;
	if (deadzone_pct == 0) deadzone_pct = 60;
	if (deadzone_pct > 99) deadzone_pct = 99;
	int v = (int)c;
	int sign = 1;
	if (v < 0) {
		sign = -1;
		v = -v;
	}
	// If |v| < deadzone_pct% of step => quantize to 0.
	if ((int64_t)v * 100 < (int64_t)step * (int64_t)deadzone_pct) return 0;
	const int q = (v + (step >> 1)) / step;
	const int r = sign * q;
	if (r < -32768) return (int16_t)-32768;
	if (r > 32767) return (int16_t)32767;
	return (int16_t)r;
}

static inline void rdo_quantize4x4_inplace(int16_t coeffs[16], int dc_step, int ac_step, int quant_mode, uint32_t ac_deadzone_pct) {
	if (!coeffs) return;
	coeffs[0] = rdo_quant_one(coeffs[0], dc_step);
	if (quant_mode == 1) {
		for (int i = 1; i < 16; ++i) coeffs[i] = rdo_quant_one_ac_deadzone_pct(coeffs[i], ac_step, ac_deadzone_pct);
	} else {
		for (int i = 1; i < 16; ++i) coeffs[i] = rdo_quant_one(coeffs[i], ac_step);
	}
}

static inline uint32_t rdo_coeff_mag_cost(int16_t c) {
	// Very cheap magnitude proxy: 1 + floor(log2(|c|)) for |c|>0, capped.
	// This tends to correlate better with actual token cost than nnz alone.
	int32_t v = (int32_t)c;
	uint32_t a = (v < 0) ? (uint32_t)(-v) : (uint32_t)v;
	if (a == 0) return 0;
	uint32_t cost = 1;
	if (a >= 2) cost += 1;
	if (a >= 4) cost += 1;
	if (a >= 8) cost += 1;
	if (a >= 16) cost += 1;
	if (a >= 32) cost += 1;
	return cost;
}

static inline uint32_t rdo_rate_proxy4x4(const int16_t coeff[16]) {
	uint32_t rate = 0;
	// DC tends to be more expensive/important.
	if (coeff[0] != 0) rate += 1;
	rate += rdo_coeff_mag_cost(coeff[0]);
	for (int i = 1; i < 16; ++i) {
		rate += rdo_coeff_mag_cost(coeff[i]);
	}
	return rate;
}

static inline uint32_t rdo_rate_from_token_bits_q8(uint32_t bits_q8) {
	// Convert Q8 bits to a small integer proxy.
	// Downscale by 8 to keep lambda tuning in-range.
	return (bits_q8 >> 11);
}

static inline uint32_t rdo_rate_from_mode_bits_q8(uint32_t bits_q8) {
	// Mode signaling costs are only a handful of bits. If we downscale them the same
	// way as coefficient token bits, they frequently quantize to zero and effectively
	// disappear from the rate term. Use a ceil(bits/8) mapping so they stay non-zero
	// but remain comparable in scale.
	const uint32_t bits = (bits_q8 >> 8);
	return (bits + 7u) >> 3;
}

static inline uint32_t rdo_bmode_signal_cost(Vp8BMode mode) {
	// Tiny fixed cost for signaling the 4x4 intra mode.
	// This is a proxy for the entropy-coded mode tree cost.
	// Kept deliberately small to avoid overpowering coeff magnitude costs.
	switch (mode) {
		case B_DC_PRED: return 0;
		case B_TM_PRED: return 1;
		case B_VE_PRED: return 1;
		case B_HE_PRED: return 1;
		default: return 2;
	}
}

static inline uint32_t rdo_uv_mode_signal_cost(Vp8I16Mode mode) {
	// UV intra mode (same enum as I16 modes). Also entropy-coded in VP8.
	switch (mode) {
		case VP8_I16_DC_PRED: return 0;
		case VP8_I16_V_PRED: return 1;
		case VP8_I16_H_PRED: return 1;
		case VP8_I16_TM_PRED: return 1;
		default: return 1;
	}
}

static inline uint32_t rdo_ymode_signal_cost(uint8_t ymode) {
	// Tiny fixed cost for signaling the macroblock luma mode (I16 vs B_PRED).
	// 0..3 map to VP8_I16_{DC,V,H,TM}_PRED, and 4 is B_PRED in our y_modes array.
	// This is a cheap proxy for the entropy-coded y-mode tree cost.
	switch (ymode) {
		case 0: return 0; // I16 DC
		case 1: return 1; // I16 V
		case 2: return 1; // I16 H
		case 3: return 1; // I16 TM
		case 4: return 2; // B_PRED is typically more expensive than DC.
		default: return 2;
	}
}

static inline int rdo_mbymode_to_bmode(int ymode) {
	// Match the mapping used by the VP8 keyframe decoder/bitstream:
	// when a neighbor macroblock is not B_PRED, its subblock modes are implied
	// by the macroblock mode for purposes of bmode contexts.
	switch (ymode) {
		case VP8_I16_DC_PRED: return B_DC_PRED;
		case VP8_I16_V_PRED: return B_VE_PRED;
		case VP8_I16_H_PRED: return B_HE_PRED;
		case VP8_I16_TM_PRED: return B_TM_PRED;
		default: return B_DC_PRED;
	}
}

static inline uint32_t rdo_lambda_from_qindex(uint8_t qindex, uint32_t mul, uint32_t div) {
	// Conservative lambda schedule. Grows with quantization strength.
	const uint32_t q = (uint32_t)qindex;
	uint32_t base = (q * q) / 24u + 4u;
	if (div == 0) div = 1;
	uint64_t scaled = (uint64_t)base * (uint64_t)mul;
	scaled = (scaled + (uint64_t)(div / 2u)) / (uint64_t)div;
	if (scaled > 0xFFFFFFFFull) return 0xFFFFFFFFu;
	return (uint32_t)scaled;
}

static inline uint32_t rdo_rate_weight_y2(uint32_t rate) {
	// In VP8, the I16 DC (Y2) block tends to have a disproportionate impact on
	// bit-cost. Our magnitude proxy underestimates this, so weight it slightly.
	return rate * 2u;
}

// 4x4 subblock predictor for B_PRED (keyframe), matching RFC 6386 reference code.
// A points to the above row with A[-1] valid (top-left), and A[0..7] valid.
static void bpred4x4(uint8_t out[16], const uint8_t* A, const uint8_t* L, Vp8BMode mode) {
	uint8_t E[9];
	E[0] = L[3];
	E[1] = L[2];
	E[2] = L[1];
	E[3] = L[0];
	E[4] = A[-1];
	E[5] = A[0];
	E[6] = A[1];
	E[7] = A[2];
	E[8] = A[3];

	uint8_t B[4][4];
	switch (mode) {
		case B_DC_PRED: {
			int v = 4;
			for (int i = 0; i < 4; i++) v += (int)A[i] + (int)L[i];
			v >>= 3;
			for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++) B[r][c] = (uint8_t)v;
			break;
		}
		case B_TM_PRED: {
			for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++) B[r][c] = clamp255_i32((int32_t)L[r] + (int32_t)A[c] - (int32_t)A[-1]);
			break;
		}
		case B_VE_PRED: {
			for (int c = 0; c < 4; c++) {
				uint8_t v = avg3_u8(A[c - 1], A[c], A[c + 1]);
				B[0][c] = B[1][c] = B[2][c] = B[3][c] = v;
			}
			break;
		}
		case B_HE_PRED: {
			uint8_t v = avg3_u8(L[2], L[3], L[3]);
			B[3][0] = B[3][1] = B[3][2] = B[3][3] = v;
			v = avg3_u8(L[1], L[2], L[3]);
			B[2][0] = B[2][1] = B[2][2] = B[2][3] = v;
			v = avg3_u8(L[0], L[1], L[2]);
			B[1][0] = B[1][1] = B[1][2] = B[1][3] = v;
			v = avg3_u8(A[-1], L[0], L[1]);
			B[0][0] = B[0][1] = B[0][2] = B[0][3] = v;
			break;
		}
		case B_LD_PRED: {
			B[0][0] = avg3_u8(A[0], A[1], A[2]);
			B[0][1] = B[1][0] = avg3_u8(A[1], A[2], A[3]);
			B[0][2] = B[1][1] = B[2][0] = avg3_u8(A[2], A[3], A[4]);
			B[0][3] = B[1][2] = B[2][1] = B[3][0] = avg3_u8(A[3], A[4], A[5]);
			B[1][3] = B[2][2] = B[3][1] = avg3_u8(A[4], A[5], A[6]);
			B[2][3] = B[3][2] = avg3_u8(A[5], A[6], A[7]);
			B[3][3] = avg3_u8(A[6], A[7], A[7]);
			break;
		}
		case B_RD_PRED: {
			B[3][0] = avg3_u8(E[0], E[1], E[2]);
			B[3][1] = B[2][0] = avg3_u8(E[1], E[2], E[3]);
			B[3][2] = B[2][1] = B[1][0] = avg3_u8(E[2], E[3], E[4]);
			B[3][3] = B[2][2] = B[1][1] = B[0][0] = avg3_u8(E[3], E[4], E[5]);
			B[2][3] = B[1][2] = B[0][1] = avg3_u8(E[4], E[5], E[6]);
			B[1][3] = B[0][2] = avg3_u8(E[5], E[6], E[7]);
			B[0][3] = avg3_u8(E[6], E[7], E[8]);
			break;
		}
		case B_VR_PRED: {
			uint8_t avg3p_2 = avg3_u8(E[1], E[2], E[3]);
			uint8_t avg3p_3 = avg3_u8(E[2], E[3], E[4]);
			uint8_t avg3p_4 = avg3_u8(E[3], E[4], E[5]);
			uint8_t avg3p_5 = avg3_u8(E[4], E[5], E[6]);
			uint8_t avg3p_6 = avg3_u8(E[5], E[6], E[7]);
			uint8_t avg3p_7 = avg3_u8(E[6], E[7], E[8]);
			uint8_t avg2p_4 = avg2_u8(E[4], E[5]);
			uint8_t avg2p_5 = avg2_u8(E[5], E[6]);
			uint8_t avg2p_6 = avg2_u8(E[6], E[7]);
			uint8_t avg2p_7 = avg2_u8(E[7], E[8]);

			B[3][0] = avg3p_2;
			B[2][0] = avg3p_3;
			B[3][1] = B[1][0] = avg3p_4;
			B[2][1] = B[0][0] = avg2p_4;
			B[3][2] = B[1][1] = avg3p_5;
			B[2][2] = B[0][1] = avg2p_5;
			B[3][3] = B[1][2] = avg3p_6;
			B[2][3] = B[0][2] = avg2p_6;
			B[1][3] = avg3p_7;
			B[0][3] = avg2p_7;
			break;
		}
		case B_VL_PRED: {
			B[0][0] = avg2_u8(A[0], A[1]);
			B[1][0] = avg3_u8(A[0], A[1], A[2]);
			B[2][0] = B[0][1] = avg2_u8(A[1], A[2]);
			B[1][1] = B[3][0] = avg3_u8(A[1], A[2], A[3]);
			B[2][1] = B[0][2] = avg2_u8(A[2], A[3]);
			B[3][1] = B[1][2] = avg3_u8(A[2], A[3], A[4]);
			B[2][2] = B[0][3] = avg2_u8(A[3], A[4]);
			B[3][2] = B[1][3] = avg3_u8(A[3], A[4], A[5]);
			B[2][3] = avg3_u8(A[4], A[5], A[6]);
			B[3][3] = avg3_u8(A[5], A[6], A[7]);
			break;
		}
		case B_HD_PRED: {
			B[3][0] = avg2_u8(E[0], E[1]);
			B[3][1] = avg3_u8(E[0], E[1], E[2]);
			B[2][0] = B[3][2] = avg2_u8(E[1], E[2]);
			B[2][1] = B[3][3] = avg3_u8(E[1], E[2], E[3]);
			B[2][2] = B[1][0] = avg2_u8(E[2], E[3]);
			B[2][3] = B[1][1] = avg3_u8(E[2], E[3], E[4]);
			B[1][2] = B[0][0] = avg2_u8(E[3], E[4]);
			B[1][3] = B[0][1] = avg3_u8(E[3], E[4], E[5]);
			B[0][2] = avg3_u8(E[4], E[5], E[6]);
			B[0][3] = avg3_u8(E[5], E[6], E[7]);
			break;
		}
		case B_HU_PRED: {
			B[0][0] = avg2_u8(E[0], E[1]);
			B[0][1] = avg3_u8(E[0], E[1], E[2]);
			B[0][2] = avg2_u8(E[1], E[2]);
			B[0][3] = avg3_u8(E[1], E[2], E[3]);
			B[1][0] = avg3_u8(E[1], E[2], E[3]);
			B[1][1] = avg2_u8(E[2], E[3]);
			B[1][2] = avg3_u8(E[2], E[3], E[4]);
			B[1][3] = avg2_u8(E[3], E[4]);
			B[2][0] = avg2_u8(E[3], E[4]);
			B[2][1] = avg3_u8(E[3], E[4], E[5]);
			B[2][2] = avg2_u8(E[4], E[5]);
			B[2][3] = avg3_u8(E[4], E[5], E[6]);
			B[3][0] = avg3_u8(E[4], E[5], E[6]);
			B[3][1] = avg2_u8(E[5], E[6]);
			B[3][2] = avg3_u8(E[5], E[6], E[7]);
			B[3][3] = avg2_u8(E[6], E[7]);
			break;
		}
		default: {
			for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++) B[r][c] = 0x80;
			break;
		}
	}

	for (int r = 0; r < 4; r++) {
		for (int c = 0; c < 4; c++) {
			out[r * 4 + c] = B[r][c];
		}
	}
}

static void pred16x16_dc(uint8_t dst[16 * 16], const uint8_t* A16, const uint8_t* L16, int have_above, int have_left) {
	uint8_t top[16];
	uint8_t left[16];
	const uint8_t* top_ptr = NULL;
	const uint8_t* left_ptr = NULL;
	if (have_above && A16) {
		memcpy(top, A16, 16);
		top_ptr = top;
	}
	if (have_left && L16) {
		memcpy(left, L16, 16);
		left_ptr = left;
	}
	uint8_t v = dc_value(left_ptr, top_ptr, 16, 16, 5);
	for (int i = 0; i < 16 * 16; i++) dst[i] = v;
}

static void pred16x16_v(uint8_t dst[16 * 16], const uint8_t* A16, int have_above, uint8_t above_oob) {
	for (int r = 0; r < 16; r++) {
		for (int c = 0; c < 16; c++) dst[r * 16 + c] = have_above ? A16[c] : above_oob;
	}
}

static void pred16x16_h(uint8_t dst[16 * 16], const uint8_t* L16, int have_left, uint8_t left_oob) {
	for (int r = 0; r < 16; r++) {
		uint8_t v = have_left ? L16[r] : left_oob;
		for (int c = 0; c < 16; c++) dst[r * 16 + c] = v;
	}
}

static void pred16x16_tm(uint8_t dst[16 * 16],
	                     const uint8_t* A16,
	                     const uint8_t* L16,
	                     int have_above,
	                     int have_left,
	                     uint8_t above_oob,
	                     uint8_t left_oob,
	                     uint8_t above_left) {
	uint8_t P;
	if (have_above && have_left) {
		P = above_left;
	} else if (!have_above && have_left) {
		P = above_oob;
	} else if (have_above && !have_left) {
		P = left_oob;
	} else {
		P = above_oob;
	}
	for (int r = 0; r < 16; r++) {
		uint8_t Lv = have_left ? L16[r] : left_oob;
		for (int c = 0; c < 16; c++) {
			uint8_t Av = have_above ? A16[c] : above_oob;
			dst[r * 16 + c] = clamp255_i32((int32_t)Lv + (int32_t)Av - (int32_t)P);
		}
	}
}

static void pred16x16_build(uint8_t dst[16 * 16],
	                     Vp8I16Mode mode,
	                     const uint8_t* A16,
	                     const uint8_t* L16,
	                     int have_above,
	                     int have_left,
	                     uint8_t above_oob,
	                     uint8_t left_oob,
	                     uint8_t above_left) {
	switch (mode) {
		case VP8_I16_DC_PRED: pred16x16_dc(dst, A16, L16, have_above, have_left); return;
		case VP8_I16_V_PRED: pred16x16_v(dst, A16, have_above, above_oob); return;
		case VP8_I16_H_PRED: pred16x16_h(dst, L16, have_left, left_oob); return;
		case VP8_I16_TM_PRED: pred16x16_tm(dst, A16, L16, have_above, have_left, above_oob, left_oob, above_left); return;
		default: pred16x16_dc(dst, A16, L16, have_above, have_left); return;
	}
}

static uint32_t sse16x16_src_vs_pred(const EncYuv420Image* yuv,
	                                uint32_t w,
	                                uint32_t h,
	                                uint32_t x0,
	                                uint32_t y0,
	                                const uint8_t pred[16 * 16]) {
	uint32_t sse = 0;
	for (uint32_t dy = 0; dy < 16; dy++) {
		for (uint32_t dx = 0; dx < 16; dx++) {
			uint8_t s = load_clamped(yuv->y, yuv->y_stride, w, h, x0 + dx, y0 + dy);
			uint8_t p = pred[(dy * 16u) + dx];
			int d = (int)s - (int)p;
			sse += (uint32_t)(d * d);
		}
	}
	return sse;
}

static void pred16_fill4x4(uint8_t out4x4[16], const uint8_t pred16[16 * 16], uint32_t bx, uint32_t by) {
	for (uint32_t dy = 0; dy < 4; dy++) {
		for (uint32_t dx = 0; dx < 4; dx++) {
			out4x4[dy * 4 + dx] = pred16[(by + dy) * 16u + (bx + dx)];
		}
	}
}

static void pred8_fill4x4(uint8_t out4x4[16], const uint8_t pred8[8 * 8], uint32_t bx, uint32_t by) {
	for (uint32_t dy = 0; dy < 4; dy++) {
		for (uint32_t dx = 0; dx < 4; dx++) {
			out4x4[dy * 4 + dx] = pred8[(by + dy) * 8u + (bx + dx)];
		}
	}
}

static void pred8x8_dc(uint8_t dst[8 * 8], const uint8_t* A8, const uint8_t* L8, int have_above, int have_left) {
	uint8_t top[8];
	uint8_t left[8];
	const uint8_t* top_ptr = NULL;
	const uint8_t* left_ptr = NULL;
	if (have_above && A8) {
		memcpy(top, A8, 8);
		top_ptr = top;
	}
	if (have_left && L8) {
		memcpy(left, L8, 8);
		left_ptr = left;
	}
	uint8_t v = dc_value(left_ptr, top_ptr, 8, 8, 4);
	for (int i = 0; i < 8 * 8; i++) dst[i] = v;
}

static void pred8x8_v(uint8_t dst[8 * 8], const uint8_t* A8, int have_above, uint8_t above_oob) {
	for (int r = 0; r < 8; r++) {
		for (int c = 0; c < 8; c++) dst[r * 8 + c] = have_above ? A8[c] : above_oob;
	}
}

static void pred8x8_h(uint8_t dst[8 * 8], const uint8_t* L8, int have_left, uint8_t left_oob) {
	for (int r = 0; r < 8; r++) {
		uint8_t v = have_left ? L8[r] : left_oob;
		for (int c = 0; c < 8; c++) dst[r * 8 + c] = v;
	}
}

static void pred8x8_tm(uint8_t dst[8 * 8],
	                  const uint8_t* A8,
	                  const uint8_t* L8,
	                  int have_above,
	                  int have_left,
	                  uint8_t above_oob,
	                  uint8_t left_oob,
	                  uint8_t above_left) {
	uint8_t P;
	if (have_above && have_left) {
		P = above_left;
	} else if (!have_above && have_left) {
		P = above_oob;
	} else if (have_above && !have_left) {
		P = left_oob;
	} else {
		P = above_oob;
	}
	for (int r = 0; r < 8; r++) {
		uint8_t Lv = have_left ? L8[r] : left_oob;
		for (int c = 0; c < 8; c++) {
			uint8_t Av = have_above ? A8[c] : above_oob;
			dst[r * 8 + c] = clamp255_i32((int32_t)Lv + (int32_t)Av - (int32_t)P);
		}
	}
}

static void pred8x8_build(uint8_t dst[8 * 8],
	                   Vp8I16Mode mode,
	                   const uint8_t* A8,
	                   const uint8_t* L8,
	                   int have_above,
	                   int have_left,
	                   uint8_t above_oob,
	                   uint8_t left_oob,
	                   uint8_t above_left) {
	switch (mode) {
		case VP8_I16_DC_PRED: pred8x8_dc(dst, A8, L8, have_above, have_left); return;
		case VP8_I16_V_PRED: pred8x8_v(dst, A8, have_above, above_oob); return;
		case VP8_I16_H_PRED: pred8x8_h(dst, L8, have_left, left_oob); return;
		case VP8_I16_TM_PRED: pred8x8_tm(dst, A8, L8, have_above, have_left, above_oob, left_oob, above_left); return;
		default: pred8x8_dc(dst, A8, L8, have_above, have_left); return;
	}
}

static uint32_t sse8x8_plane_src_vs_pred(const uint8_t* src,
	                                   uint32_t src_stride,
	                                   uint32_t w,
	                                   uint32_t h,
	                                   uint32_t x0,
	                                   uint32_t y0,
	                                   const uint8_t pred[8 * 8]) {
	uint32_t sse = 0;
	for (uint32_t dy = 0; dy < 8; dy++) {
		for (uint32_t dx = 0; dx < 8; dx++) {
			uint8_t s = load_clamped(src, src_stride, w, h, x0 + dx, y0 + dy);
			uint8_t p = pred[(dy * 8u) + dx];
			int d = (int)s - (int)p;
			sse += (uint32_t)(d * d);
		}
	}
	return sse;
}

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

static void dequant4x4_inplace(int16_t coeffs[16], int dc_step, int ac_step);
static void inv_wht4x4(const int16_t* input, int16_t* output);
static void inv_dct4x4(const int16_t* input, int16_t* output);

static uint32_t sad4x4_u8(const uint8_t a[16], const uint8_t b[16]) {
	uint32_t sad = 0;
	for (int i = 0; i < 16; i++) {
		int d = (int)a[i] - (int)b[i];
		sad += (uint32_t)(d < 0 ? -d : d);
	}
	return sad;
}

static uint32_t sse4x4_src_vs_recon(const uint8_t src4[16], const uint8_t pred4[16], const int16_t res[16]) {
	uint32_t sse = 0;
	for (int i = 0; i < 16; i++) {
		int32_t v = (int32_t)pred4[i] + (int32_t)res[i];
		uint8_t r = clamp255_i32(v);
		int d = (int)src4[i] - (int)r;
		sse += (uint32_t)(d * d);
	}
	return sse;
}

static uint32_t sse4x4_boundary_src_vs_recon(const uint8_t src4[16], const uint8_t pred4[16], const int16_t res[16]) {
	// Weight pixels that feed future predictors (right column, bottom row).
	uint32_t sse = 0;
	for (int y = 0; y < 4; y++) {
		for (int x = 0; x < 4; x++) {
			const int i = y * 4 + x;
			int32_t v = (int32_t)pred4[i] + (int32_t)res[i];
			uint8_t r = clamp255_i32(v);
			int d = (int)src4[i] - (int)r;
			uint32_t w = 1;
			if (x == 3) w += 1;
			if (y == 3) w += 1;
			sse += w * (uint32_t)(d * d);
		}
	}
	return sse;
}

static uint32_t satd4x4_src_vs_pred(const uint8_t src4[16], const uint8_t pred4[16]) {
	// 4x4 Hadamard SATD on the residual (src - pred). Used only for candidate ranking.
	int16_t r[16];
	for (int i = 0; i < 16; i++) r[i] = (int16_t)((int)src4[i] - (int)pred4[i]);

	int32_t tmp[16];
	for (int y = 0; y < 4; y++) {
		const int o = y * 4;
		const int32_t a0 = (int32_t)r[o + 0] + (int32_t)r[o + 3];
		const int32_t a1 = (int32_t)r[o + 1] + (int32_t)r[o + 2];
		const int32_t a2 = (int32_t)r[o + 1] - (int32_t)r[o + 2];
		const int32_t a3 = (int32_t)r[o + 0] - (int32_t)r[o + 3];
		tmp[o + 0] = a0 + a1;
		tmp[o + 1] = a3 + a2;
		tmp[o + 2] = a0 - a1;
		tmp[o + 3] = a3 - a2;
	}

	uint32_t sum = 0;
	for (int x = 0; x < 4; x++) {
		const int32_t b0 = tmp[0 * 4 + x] + tmp[3 * 4 + x];
		const int32_t b1 = tmp[1 * 4 + x] + tmp[2 * 4 + x];
		const int32_t b2 = tmp[1 * 4 + x] - tmp[2 * 4 + x];
		const int32_t b3 = tmp[0 * 4 + x] - tmp[3 * 4 + x];
		const int32_t t0 = b0 + b1;
		const int32_t t1 = b3 + b2;
		const int32_t t2 = b0 - b1;
		const int32_t t3 = b3 - b2;
		const int32_t v[4] = {t0, t1, t2, t3};
		for (int k = 0; k < 4; k++) {
			int32_t a = v[k];
			if (a < 0) a = -a;
			sum += (uint32_t)a;
		}
	}

	// Scaling isn't important for ranking; keep it small-ish.
	return (sum + 1u) >> 1;
}

// Small trellis: tweak quantized DC by a couple of steps to reduce SSE.
// This helps reduce systematic luma bias (visible as banding) in heavily
// quantized textured areas.
static void refine_dc_quant4x4(int16_t coeff[16], int dc_step, int ac_step, const uint8_t src4[16], const uint8_t pred4[16]) {
	if (!coeff) return;
	const int16_t base_dc = coeff[0];
	uint32_t best_sse = 0xFFFFFFFFu;
	uint32_t base_sse = 0xFFFFFFFFu;
	int16_t best_dc = base_dc;

	for (int delta = -1; delta <= 1; delta++) {
		int dc_i = (int)base_dc + delta;
		if (dc_i < -32768) dc_i = -32768;
		if (dc_i > 32767) dc_i = 32767;
		int16_t cand[16];
		for (int i = 0; i < 16; i++) cand[i] = coeff[i];
		cand[0] = (int16_t)dc_i;
		dequant4x4_inplace(cand, dc_step, ac_step);
		int16_t res[16];
		inv_dct4x4(cand, res);
		uint32_t sse = sse4x4_boundary_src_vs_recon(src4, pred4, res);
		if (delta == 0) base_sse = sse;
		if (sse < best_sse) {
			best_sse = sse;
			best_dc = (int16_t)dc_i;
		}
	}

	// Only apply if it provides a clear improvement on predictor-relevant edges.
	if (best_dc != base_dc && best_sse + 64u < base_sse) {
		coeff[0] = best_dc;
	}
}

static uint32_t sad8x8_plane_src_vs_pred(const uint8_t* src,
	                                     uint32_t src_stride,
	                                     uint32_t w,
	                                     uint32_t h,
	                                     uint32_t x0,
	                                     uint32_t y0,
	                                     const uint8_t pred[8 * 8]) {
	uint32_t sad = 0;
	for (uint32_t dy = 0; dy < 8; dy++) {
		for (uint32_t dx = 0; dx < 8; dx++) {
			uint8_t s = load_clamped(src, src_stride, w, h, x0 + dx, y0 + dy);
			uint8_t p = pred[(dy * 8u) + dx];
			int d = (int)s - (int)p;
			sad += (uint32_t)(d < 0 ? -d : d);
		}
	}
	return sad;
}

static void inv_wht4x4(const int16_t* input, int16_t* output) {
	int16_t tmp[16];
	for (int i = 0; i < 4; i++) {
		int a1 = input[0 + i] + input[12 + i];
		int b1 = input[4 + i] + input[8 + i];
		int c1 = input[4 + i] - input[8 + i];
		int d1 = input[0 + i] - input[12 + i];

		tmp[0 + i] = (int16_t)(a1 + b1);
		tmp[4 + i] = (int16_t)(c1 + d1);
		tmp[8 + i] = (int16_t)(a1 - b1);
		tmp[12 + i] = (int16_t)(d1 - c1);
	}
	for (int i = 0; i < 4; i++) {
		int a1 = tmp[4 * i + 0] + tmp[4 * i + 3];
		int b1 = tmp[4 * i + 1] + tmp[4 * i + 2];
		int c1 = tmp[4 * i + 1] - tmp[4 * i + 2];
		int d1 = tmp[4 * i + 0] - tmp[4 * i + 3];

		output[4 * i + 0] = (int16_t)((a1 + b1 + 3) >> 3);
		output[4 * i + 1] = (int16_t)((c1 + d1 + 3) >> 3);
		output[4 * i + 2] = (int16_t)((a1 - b1 + 3) >> 3);
		output[4 * i + 3] = (int16_t)((d1 - c1 + 3) >> 3);
	}
}

static void inv_dct4x4(const int16_t* input, int16_t* output) {
	static const int cospi8sqrt2minus1 = 20091;
	static const int sinpi8sqrt2 = 35468;

	int16_t tmp[16];
	for (int i = 0; i < 4; i++) {
		int32_t a1 = (int32_t)input[i + 0] + (int32_t)input[i + 8];
		int32_t b1 = (int32_t)input[i + 0] - (int32_t)input[i + 8];

		int32_t temp1 = ((int32_t)input[i + 4] * sinpi8sqrt2) >> 16;
		int32_t temp2 = (int32_t)input[i + 12] + (((int32_t)input[i + 12] * cospi8sqrt2minus1) >> 16);
		int32_t c1 = temp1 - temp2;

		temp1 = (int32_t)input[i + 4] + (((int32_t)input[i + 4] * cospi8sqrt2minus1) >> 16);
		temp2 = ((int32_t)input[i + 12] * sinpi8sqrt2) >> 16;
		int32_t d1 = temp1 + temp2;

		tmp[0 + i] = (int16_t)(a1 + d1);
		tmp[12 + i] = (int16_t)(a1 - d1);
		tmp[4 + i] = (int16_t)(b1 + c1);
		tmp[8 + i] = (int16_t)(b1 - c1);
	}
	for (int i = 0; i < 4; i++) {
		int32_t a1 = (int32_t)tmp[4 * i + 0] + (int32_t)tmp[4 * i + 2];
		int32_t b1 = (int32_t)tmp[4 * i + 0] - (int32_t)tmp[4 * i + 2];

		int32_t temp1 = ((int32_t)tmp[4 * i + 1] * sinpi8sqrt2) >> 16;
		int32_t temp2 = (int32_t)tmp[4 * i + 3] + (((int32_t)tmp[4 * i + 3] * cospi8sqrt2minus1) >> 16);
		int32_t c1 = temp1 - temp2;

		temp1 = (int32_t)tmp[4 * i + 1] + (((int32_t)tmp[4 * i + 1] * cospi8sqrt2minus1) >> 16);
		temp2 = ((int32_t)tmp[4 * i + 3] * sinpi8sqrt2) >> 16;
		int32_t d1 = temp1 + temp2;

		output[4 * i + 0] = (int16_t)((a1 + d1 + 4) >> 3);
		output[4 * i + 3] = (int16_t)((a1 - d1 + 4) >> 3);
		output[4 * i + 1] = (int16_t)((b1 + c1 + 4) >> 3);
		output[4 * i + 2] = (int16_t)((b1 - c1 + 4) >> 3);
	}
}

int enc_vp8_recon_alloc(uint32_t width, uint32_t height, EncVp8ReconPlanes* out) {
	if (!out) {
		errno = EINVAL;
		return -1;
	}
	*out = (EncVp8ReconPlanes){0};

	if (width == 0 || height == 0) {
		errno = EINVAL;
		return -1;
	}
	const uint32_t mb_cols = (width + 15u) >> 4;
	const uint32_t mb_rows = (height + 15u) >> 4;
	if (mb_cols == 0 || mb_rows == 0) {
		errno = EINVAL;
		return -1;
	}
	const uint32_t y_stride = mb_cols * 16u;
	const uint32_t uv_stride = mb_cols * 8u;
	const uint32_t y_h = mb_rows * 16u;
	const uint32_t uv_h = mb_rows * 8u;

	size_t y_bytes = (size_t)y_stride * (size_t)y_h;
	size_t uv_bytes = (size_t)uv_stride * (size_t)uv_h;
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
	memset(y, 0x80, y_bytes);
	memset(u, 0x80, uv_bytes);
	memset(v, 0x80, uv_bytes);

	out->width = width;
	out->height = height;
	out->mb_cols = mb_cols;
	out->mb_rows = mb_rows;
	out->y_stride = y_stride;
	out->uv_stride = uv_stride;
	out->y = y;
	out->u = u;
	out->v = v;
	return 0;
}

void enc_vp8_recon_free(EncVp8ReconPlanes* p) {
	if (!p) return;
	free(p->y);
	free(p->u);
	free(p->v);
	*p = (EncVp8ReconPlanes){0};
}

static void dequant4x4_inplace(int16_t coeffs[16], int dc_step, int ac_step) {
	coeffs[0] = (int16_t)((int)coeffs[0] * dc_step);
	for (int i = 1; i < 16; ++i) {
		coeffs[i] = (int16_t)((int)coeffs[i] * ac_step);
	}
}

int enc_vp8_encode_dc_pred_inloop(const EncYuv420Image* yuv,
                                 int quality,
                                 int16_t** coeffs_out,
                                 size_t* coeffs_count_out,
                                 uint8_t* qindex_out) {
	if (!coeffs_out || !coeffs_count_out || !qindex_out) {
		errno = EINVAL;
		return -1;
	}
	*coeffs_out = NULL;
	*coeffs_count_out = 0;
	*qindex_out = 0;
	if (!yuv || !yuv->y || !yuv->u || !yuv->v || yuv->width == 0 || yuv->height == 0) {
		errno = EINVAL;
		return -1;
	}

	const uint32_t w = yuv->width;
	const uint32_t h = yuv->height;
	const uint32_t mb_cols = (w + 15u) >> 4;
	const uint32_t mb_rows = (h + 15u) >> 4;
	const uint32_t mb_total = mb_cols * mb_rows;
	const size_t coeffs_per_mb = 16 + (16 * 16) + (4 * 16) + (4 * 16);
	const size_t coeffs_total = (size_t)mb_total * coeffs_per_mb;
	if (mb_total == 0 || coeffs_total > (SIZE_MAX / sizeof(int16_t))) {
		errno = EOVERFLOW;
		return -1;
	}
	int16_t* out = (int16_t*)malloc(coeffs_total * sizeof(int16_t));
	if (!out) {
		errno = ENOMEM;
		return -1;
	}

	EncVp8ReconPlanes recon;
	if (enc_vp8_recon_alloc(w, h, &recon) != 0) {
		free(out);
		return -1;
	}

	const int qindex = enc_vp8_qindex_from_quality_libwebp(quality);
	EncVp8QuantFactors qf;
	enc_vp8_quant_factors_from_qindex(qindex, 0, 0, 0, 0, 0, &qf);
	*qindex_out = (uint8_t)qf.qindex;

	const uint32_t uv_w = (w + 1u) >> 1;
	const uint32_t uv_h = (h + 1u) >> 1;

	uint8_t src4[16];
	uint8_t ref4[16];
	int16_t tmp[16][16];
	int16_t y2[16];
	int16_t y2_deq[16];
	int16_t y_dc16[16];

	for (uint32_t mby = 0; mby < mb_rows; ++mby) {
		for (uint32_t mbx = 0; mbx < mb_cols; ++mbx) {
			const uint32_t x0 = mbx * 16u;
			const uint32_t y0 = mby * 16u;
			const uint32_t ux0 = mbx * 8u;
			const uint32_t uy0 = mby * 8u;

			// Predictor DC for Y from reconstructed neighbors.
			uint8_t top16[16];
			uint8_t left16[16];
			const uint8_t* top_ptr = NULL;
			const uint8_t* left_ptr = NULL;
			if (mby > 0) {
				for (uint32_t i = 0; i < 16; i++) {
					top16[i] = recon.y[(size_t)(y0 - 1) * recon.y_stride + (size_t)(x0 + i)];
				}
				top_ptr = top16;
			}
			if (mbx > 0) {
				for (uint32_t i = 0; i < 16; i++) {
					left16[i] = recon.y[(size_t)(y0 + i) * recon.y_stride + (size_t)(x0 - 1)];
				}
				left_ptr = left16;
			}
			const uint8_t dc_y = dc_value(left_ptr, top_ptr, 16, 16, 5);

			// Predictor DC for U/V from reconstructed neighbors.
			uint8_t top8_u[8], left8_u[8], top8_v[8], left8_v[8];
			const uint8_t* top_u = NULL;
			const uint8_t* left_u = NULL;
			const uint8_t* top_v = NULL;
			const uint8_t* left_v = NULL;
			if (mby > 0) {
				for (uint32_t i = 0; i < 8; i++) {
					top8_u[i] = recon.u[(size_t)(uy0 - 1) * recon.uv_stride + (size_t)(ux0 + i)];
					top8_v[i] = recon.v[(size_t)(uy0 - 1) * recon.uv_stride + (size_t)(ux0 + i)];
				}
				top_u = top8_u;
				top_v = top8_v;
			}
			if (mbx > 0) {
				for (uint32_t i = 0; i < 8; i++) {
					left8_u[i] = recon.u[(size_t)(uy0 + i) * recon.uv_stride + (size_t)(ux0 - 1)];
					left8_v[i] = recon.v[(size_t)(uy0 + i) * recon.uv_stride + (size_t)(ux0 - 1)];
				}
				left_u = left8_u;
				left_v = left8_v;
			}
			const uint8_t dc_u = dc_value(left_u, top_u, 8, 8, 4);
			const uint8_t dc_v = dc_value(left_v, top_v, 8, 8, 4);

			// Y forward transform, extract DCs into Y2.
			fill4x4_const(ref4, dc_y);
			for (uint32_t n = 0; n < 16; ++n) {
				const uint32_t bx = (n & 3u) * 4u;
				const uint32_t by = (n >> 2) * 4u;
				fill4x4_clamped(src4, yuv->y, yuv->y_stride, w, h, x0 + bx, y0 + by);
				enc_vp8_ftransform4x4(src4, 4, ref4, 4, tmp[n]);
			}
			enc_vp8_ftransform_wht(&tmp[0][0], y2);
			for (int n = 0; n < 16; ++n) tmp[n][0] = 0;

			// Quantize Y2 and Y blocks.
			{
				int16_t y2q[16];
				for (int i = 0; i < 16; ++i) y2q[i] = y2[i];
				enc_vp8_quantize4x4_inplace(y2q, qf.y2_dc, qf.y2_ac);
				for (int i = 0; i < 16; ++i) y2[i] = y2q[i];
			}
			for (int n = 0; n < 16; ++n) {
				enc_vp8_quantize4x4_inplace(tmp[n], qf.y1_dc, qf.y1_ac);
			}

			// U/V forward transforms + quant.
			int16_t ublk[4][16];
			int16_t vblk[4][16];
			fill4x4_const(ref4, dc_u);
			for (uint32_t n = 0; n < 4; ++n) {
				const uint32_t bx = (n & 1u) * 4u;
				const uint32_t by = (n >> 1) * 4u;
				fill4x4_clamped(src4, yuv->u, yuv->uv_stride, uv_w, uv_h, ux0 + bx, uy0 + by);
				enc_vp8_ftransform4x4(src4, 4, ref4, 4, ublk[n]);
				enc_vp8_quantize4x4_inplace(ublk[n], qf.uv_dc, qf.uv_ac);
			}
			fill4x4_const(ref4, dc_v);
			for (uint32_t n = 0; n < 4; ++n) {
				const uint32_t bx = (n & 1u) * 4u;
				const uint32_t by = (n >> 1) * 4u;
				fill4x4_clamped(src4, yuv->v, yuv->uv_stride, uv_w, uv_h, ux0 + bx, uy0 + by);
				enc_vp8_ftransform4x4(src4, 4, ref4, 4, vblk[n]);
				enc_vp8_quantize4x4_inplace(vblk[n], qf.uv_dc, qf.uv_ac);
			}

			// Store coeffs for this macroblock.
			const size_t mb_index = (size_t)mby * (size_t)mb_cols + (size_t)mbx;
			int16_t* dst = out + mb_index * coeffs_per_mb;
			for (int i = 0; i < 16; ++i) dst[i] = y2[i];
			dst += 16;
			for (int n = 0; n < 16; ++n) {
				for (int i = 0; i < 16; ++i) dst[i] = tmp[n][i];
				dst += 16;
			}
			for (int n = 0; n < 4; ++n) {
				for (int i = 0; i < 16; ++i) dst[i] = ublk[n][i];
				dst += 16;
			}
			for (int n = 0; n < 4; ++n) {
				for (int i = 0; i < 16; ++i) dst[i] = vblk[n][i];
				dst += 16;
			}

			// Reconstruct Y.
			for (int i = 0; i < 16; ++i) y2_deq[i] = y2[i];
			dequant4x4_inplace(y2_deq, qf.y2_dc, qf.y2_ac);
			inv_wht4x4(y2_deq, y_dc16);
			for (int n = 0; n < 16; ++n) {
				int16_t block_coeffs[16];
				for (int i = 0; i < 16; ++i) block_coeffs[i] = tmp[n][i];
				block_coeffs[0] = y_dc16[n];
				dequant4x4_inplace(block_coeffs, qf.y1_dc, qf.y1_ac);
				int16_t res[16];
				inv_dct4x4(block_coeffs, res);
				const uint32_t bx = (uint32_t)(n & 3) * 4u;
				const uint32_t by = (uint32_t)(n >> 2) * 4u;
				for (uint32_t dy = 0; dy < 4; ++dy) {
					uint8_t* row = recon.y + (size_t)(y0 + by + dy) * recon.y_stride + (size_t)(x0 + bx);
					for (uint32_t dx = 0; dx < 4; ++dx) {
						int32_t v = (int32_t)dc_y + (int32_t)res[dy * 4 + dx];
						row[dx] = clamp255_i32(v);
					}
				}
			}

			// Reconstruct U.
			for (int n = 0; n < 4; ++n) {
				int16_t block_coeffs[16];
				for (int i = 0; i < 16; ++i) block_coeffs[i] = ublk[n][i];
				dequant4x4_inplace(block_coeffs, qf.uv_dc, qf.uv_ac);
				int16_t res[16];
				inv_dct4x4(block_coeffs, res);
				const uint32_t bx = (uint32_t)(n & 1) * 4u;
				const uint32_t by = (uint32_t)(n >> 1) * 4u;
				for (uint32_t dy = 0; dy < 4; ++dy) {
					uint8_t* row = recon.u + (size_t)(uy0 + by + dy) * recon.uv_stride + (size_t)(ux0 + bx);
					for (uint32_t dx = 0; dx < 4; ++dx) {
						int32_t v = (int32_t)dc_u + (int32_t)res[dy * 4 + dx];
						row[dx] = clamp255_i32(v);
					}
				}
			}

			// Reconstruct V.
			for (int n = 0; n < 4; ++n) {
				int16_t block_coeffs[16];
				for (int i = 0; i < 16; ++i) block_coeffs[i] = vblk[n][i];
				dequant4x4_inplace(block_coeffs, qf.uv_dc, qf.uv_ac);
				int16_t res[16];
				inv_dct4x4(block_coeffs, res);
				const uint32_t bx = (uint32_t)(n & 1) * 4u;
				const uint32_t by = (uint32_t)(n >> 1) * 4u;
				for (uint32_t dy = 0; dy < 4; ++dy) {
					uint8_t* row = recon.v + (size_t)(uy0 + by + dy) * recon.uv_stride + (size_t)(ux0 + bx);
					for (uint32_t dx = 0; dx < 4; ++dx) {
						int32_t v = (int32_t)dc_v + (int32_t)res[dy * 4 + dx];
						row[dx] = clamp255_i32(v);
					}
				}
			}
		}
	}

	enc_vp8_recon_free(&recon);
	*coeffs_out = out;
	*coeffs_count_out = coeffs_total;
	return 0;
}

static int enc_vp8_encode_bpred_uv_rdo_inloop_pass(const EncYuv420Image* yuv,
								  int quality,
								  const uint8_t coeff_probs_override[4][8][3][11],
								  uint8_t** y_modes_out,
								  size_t* y_modes_count_out,
								  uint8_t** b_modes_out,
								  size_t* b_modes_count_out,
								  uint8_t** uv_modes_out,
								  size_t* uv_modes_count_out,
								  int16_t** coeffs_out,
								  size_t* coeffs_count_out,
								  uint8_t* qindex_out,
								  const EncBpredRdoTuning* tuning);

int enc_vp8_encode_bpred_uv_rdo_inloop(const EncYuv420Image* yuv,
					 int quality,
					 EncVp8TokenProbsMode token_probs_mode,
					 uint8_t** y_modes_out,
					 size_t* y_modes_count_out,
					 uint8_t** b_modes_out,
					 size_t* b_modes_count_out,
					 uint8_t** uv_modes_out,
					 size_t* uv_modes_count_out,
					 int16_t** coeffs_out,
					 size_t* coeffs_count_out,
					 uint8_t* qindex_out,
					 const EncBpredRdoTuning* tuning) {
	int rate_mode = 0;
	if (tuning) rate_mode = (int)tuning->rate_mode;
	const int use_rate_estimation = (rate_mode != 0);

	// Pass 1: always run with default probability tables for entropy-rate estimation.
	uint8_t* y1 = NULL;
	size_t y1n = 0;
	uint8_t* b1 = NULL;
	size_t b1n = 0;
	uint8_t* uv1 = NULL;
	size_t uv1n = 0;
	int16_t* c1 = NULL;
	size_t c1n = 0;
	uint8_t q1 = 0;

	int rc = enc_vp8_encode_bpred_uv_rdo_inloop_pass(yuv,
	                                               quality,
	                                               /*coeff_probs_override=*/NULL,
	                                               &y1,
	                                               &y1n,
	                                               &b1,
	                                               &b1n,
	                                               &uv1,
	                                               &uv1n,
	                                               &c1,
	                                               &c1n,
	                                               &q1,
	                                               tuning);
	if (rc != 0) return rc;

	// Only refine when a rate estimator is enabled AND token probs are adaptive.
	if (!use_rate_estimation || token_probs_mode == ENC_VP8_TOKEN_PROBS_DEFAULT) {
		*y_modes_out = y1;
		*y_modes_count_out = y1n;
		*b_modes_out = b1;
		*b_modes_count_out = b1n;
		*uv_modes_out = uv1;
		*uv_modes_count_out = uv1n;
		*coeffs_out = c1;
		*coeffs_count_out = c1n;
		*qindex_out = q1;
		return 0;
	}

	// Pass 2: derive a coeff-prob table from pass1, and use it for rate estimation.
	const uint32_t mb_cols = (yuv->width + 15u) >> 4;
	const uint32_t mb_rows = (yuv->height + 15u) >> 4;
	uint8_t probs1[4][8][3][11];
	if (token_probs_mode == ENC_VP8_TOKEN_PROBS_ADAPTIVE2) {
		enc_vp8_compute_adaptive_coeff_probs2(probs1, mb_cols, mb_rows, y1, c1);
	} else {
		enc_vp8_compute_adaptive_coeff_probs(probs1, mb_cols, mb_rows, y1, c1);
	}

	free(c1);
	free(uv1);
	free(b1);
	free(y1);

	return enc_vp8_encode_bpred_uv_rdo_inloop_pass(yuv,
	                                             quality,
	                                             probs1,
	                                             y_modes_out,
	                                             y_modes_count_out,
	                                             b_modes_out,
	                                             b_modes_count_out,
	                                             uv_modes_out,
	                                             uv_modes_count_out,
	                                             coeffs_out,
	                                             coeffs_count_out,
	                                             qindex_out,
	                                             tuning);
}


int enc_vp8_encode_i16x16_uv_sad_inloop(const EncYuv420Image* yuv,
	                                    int quality,
	                                    uint8_t** y_modes_out,
	                                    size_t* y_modes_count_out,
	                                    uint8_t** uv_modes_out,
	                                    size_t* uv_modes_count_out,
	                                    int16_t** coeffs_out,
	                                    size_t* coeffs_count_out,
	                                    uint8_t* qindex_out) {
	if (!y_modes_out || !y_modes_count_out || !uv_modes_out || !uv_modes_count_out || !coeffs_out || !coeffs_count_out ||
	    !qindex_out) {
		errno = EINVAL;
		return -1;
	}
	*y_modes_out = NULL;
	*y_modes_count_out = 0;
	*uv_modes_out = NULL;
	*uv_modes_count_out = 0;
	*coeffs_out = NULL;
	*coeffs_count_out = 0;
	*qindex_out = 0;
	if (!yuv || !yuv->y || !yuv->u || !yuv->v || yuv->width == 0 || yuv->height == 0) {
		errno = EINVAL;
		return -1;
	}

	const uint32_t w = yuv->width;
	const uint32_t h = yuv->height;
	const uint32_t mb_cols = (w + 15u) >> 4;
	const uint32_t mb_rows = (h + 15u) >> 4;
	const uint32_t mb_total = mb_cols * mb_rows;
	const size_t coeffs_per_mb = 16 + (16 * 16) + (4 * 16) + (4 * 16);
	const size_t coeffs_total = (size_t)mb_total * coeffs_per_mb;
	if (mb_total == 0 || coeffs_total > (SIZE_MAX / sizeof(int16_t))) {
		errno = EOVERFLOW;
		return -1;
	}

	uint8_t* y_modes = (uint8_t*)malloc((size_t)mb_total);
	uint8_t* uv_modes = (uint8_t*)malloc((size_t)mb_total);
	if (!y_modes || !uv_modes) {
		free(y_modes);
		free(uv_modes);
		errno = ENOMEM;
		return -1;
	}

	int16_t* out = (int16_t*)malloc(coeffs_total * sizeof(int16_t));
	if (!out) {
		free(y_modes);
		free(uv_modes);
		errno = ENOMEM;
		return -1;
	}

	EncVp8ReconPlanes recon;
	if (enc_vp8_recon_alloc(w, h, &recon) != 0) {
		free(out);
		free(y_modes);
		free(uv_modes);
		return -1;
	}

	const int qindex = enc_vp8_qindex_from_quality_libwebp(quality);
	EncVp8QuantFactors qf;
	enc_vp8_quant_factors_from_qindex(qindex, 0, 0, 0, 0, 0, &qf);
	*qindex_out = (uint8_t)qf.qindex;

	const uint32_t uv_w = (w + 1u) >> 1;
	const uint32_t uv_h = (h + 1u) >> 1;

	uint8_t src4[16];
	uint8_t ref4[16];
	int16_t tmp[16][16];
	int16_t y2[16];
	int16_t y2_deq[16];
	int16_t y_dc16[16];

	uint8_t pred_y16[16 * 16];
	uint8_t pred_tmp[16 * 16];
	uint8_t pred_u8[8 * 8];
	uint8_t pred_v8[8 * 8];
	uint8_t pred_u_tmp[8 * 8];
	uint8_t pred_v_tmp[8 * 8];

	for (uint32_t mby = 0; mby < mb_rows; ++mby) {
		for (uint32_t mbx = 0; mbx < mb_cols; ++mbx) {
			const uint32_t x0 = mbx * 16u;
			const uint32_t y0 = mby * 16u;
			const uint32_t ux0 = mbx * 8u;
			const uint32_t uy0 = mby * 8u;

			// Build A/L neighbor vectors from reconstructed luma.
			uint8_t A16[16];
			uint8_t L16[16];
			int have_above = (mby > 0);
			int have_left = (mbx > 0);
			for (uint32_t i = 0; i < 16; i++) {
				A16[i] = have_above ? recon.y[(size_t)(y0 - 1) * recon.y_stride + (size_t)(x0 + i)] : 127;
				L16[i] = have_left ? recon.y[(size_t)(y0 + i) * recon.y_stride + (size_t)(x0 - 1)] : 129;
			}
			uint8_t above_left = 127;
			if (have_above && have_left) {
				above_left = recon.y[(size_t)(y0 - 1) * recon.y_stride + (size_t)(x0 - 1)];
			} else {
				above_left = have_above ? 129 : 127;
			}

			// Choose I16 mode by SSE (distortion).
			uint32_t best_sse = 0xFFFFFFFFu;
			Vp8I16Mode best_mode = VP8_I16_DC_PRED;
			for (Vp8I16Mode mode = VP8_I16_DC_PRED; mode <= VP8_I16_TM_PRED; mode++) {
				pred16x16_build(pred_tmp, mode, A16, L16, have_above, have_left, 127, 129, above_left);
				uint32_t sse = sse16x16_src_vs_pred(yuv, w, h, x0, y0, pred_tmp);
				if (sse < best_sse) {
					best_sse = sse;
					best_mode = mode;
				}
			}
			pred16x16_build(pred_y16, best_mode, A16, L16, have_above, have_left, 127, 129, above_left);
			const size_t mb_index = (size_t)mby * (size_t)mb_cols + (size_t)mbx;
			y_modes[mb_index] = (uint8_t)best_mode;

			// Choose UV (8x8) mode by SSE against U+V.
			int have_above_c = (mby > 0);
			int have_left_c = (mbx > 0);
			uint8_t A8u[8];
			uint8_t L8u[8];
			uint8_t A8v[8];
			uint8_t L8v[8];
			for (uint32_t i = 0; i < 8; i++) {
				A8u[i] = have_above_c ? recon.u[(size_t)(uy0 - 1) * recon.uv_stride + (size_t)(ux0 + i)] : 127;
				A8v[i] = have_above_c ? recon.v[(size_t)(uy0 - 1) * recon.uv_stride + (size_t)(ux0 + i)] : 127;
				L8u[i] = have_left_c ? recon.u[(size_t)(uy0 + i) * recon.uv_stride + (size_t)(ux0 - 1)] : 129;
				L8v[i] = have_left_c ? recon.v[(size_t)(uy0 + i) * recon.uv_stride + (size_t)(ux0 - 1)] : 129;
			}
			uint8_t above_left_u = 127;
			uint8_t above_left_v = 127;
			if (have_above_c && have_left_c) {
				above_left_u = recon.u[(size_t)(uy0 - 1) * recon.uv_stride + (size_t)(ux0 - 1)];
				above_left_v = recon.v[(size_t)(uy0 - 1) * recon.uv_stride + (size_t)(ux0 - 1)];
			} else {
				uint8_t al = have_above_c ? 129 : 127;
				above_left_u = al;
				above_left_v = al;
			}

			uint32_t best_uv_sse = 0xFFFFFFFFu;
			Vp8I16Mode best_uv_mode = VP8_I16_DC_PRED;
			for (Vp8I16Mode mode = VP8_I16_DC_PRED; mode <= VP8_I16_TM_PRED; mode++) {
				pred8x8_build(pred_u_tmp, mode, A8u, L8u, have_above_c, have_left_c, 127, 129, above_left_u);
				pred8x8_build(pred_v_tmp, mode, A8v, L8v, have_above_c, have_left_c, 127, 129, above_left_v);
				uint32_t sse_u = sse8x8_plane_src_vs_pred(yuv->u, yuv->uv_stride, uv_w, uv_h, ux0, uy0, pred_u_tmp);
				uint32_t sse_v = sse8x8_plane_src_vs_pred(yuv->v, yuv->uv_stride, uv_w, uv_h, ux0, uy0, pred_v_tmp);
				uint32_t sse = sse_u + sse_v;
				if (sse < best_uv_sse) {
					best_uv_sse = sse;
					best_uv_mode = mode;
				}
			}
			pred8x8_build(pred_u8, best_uv_mode, A8u, L8u, have_above_c, have_left_c, 127, 129, above_left_u);
			pred8x8_build(pred_v8, best_uv_mode, A8v, L8v, have_above_c, have_left_c, 127, 129, above_left_v);
			uv_modes[mb_index] = (uint8_t)best_uv_mode;

			// Y forward transform (mode-aware predictor), extract DCs into Y2.
			for (uint32_t n = 0; n < 16; ++n) {
				const uint32_t bx = (n & 3u) * 4u;
				const uint32_t by = (n >> 2) * 4u;
				fill4x4_clamped(src4, yuv->y, yuv->y_stride, w, h, x0 + bx, y0 + by);
				pred16_fill4x4(ref4, pred_y16, bx, by);
				enc_vp8_ftransform4x4(src4, 4, ref4, 4, tmp[n]);
			}
			enc_vp8_ftransform_wht(&tmp[0][0], y2);
			for (int n = 0; n < 16; ++n) tmp[n][0] = 0;

			// Quantize Y2 and Y blocks.
			{
				int16_t y2q[16];
				for (int i = 0; i < 16; ++i) y2q[i] = y2[i];
				enc_vp8_quantize4x4_inplace(y2q, qf.y2_dc, qf.y2_ac);
				for (int i = 0; i < 16; ++i) y2[i] = y2q[i];
			}
			for (int n = 0; n < 16; ++n) {
				enc_vp8_quantize4x4_inplace(tmp[n], qf.y1_dc, qf.y1_ac);
			}

			// U/V forward transforms + quant (mode-aware predictors).
			int16_t ublk[4][16];
			int16_t vblk[4][16];
			for (uint32_t n = 0; n < 4; ++n) {
				const uint32_t bx = (n & 1u) * 4u;
				const uint32_t by = (n >> 1) * 4u;
				fill4x4_clamped(src4, yuv->u, yuv->uv_stride, uv_w, uv_h, ux0 + bx, uy0 + by);
				pred8_fill4x4(ref4, pred_u8, bx, by);
				enc_vp8_ftransform4x4(src4, 4, ref4, 4, ublk[n]);
				enc_vp8_quantize4x4_inplace(ublk[n], qf.uv_dc, qf.uv_ac);
			}
			for (uint32_t n = 0; n < 4; ++n) {
				const uint32_t bx = (n & 1u) * 4u;
				const uint32_t by = (n >> 1) * 4u;
				fill4x4_clamped(src4, yuv->v, yuv->uv_stride, uv_w, uv_h, ux0 + bx, uy0 + by);
				pred8_fill4x4(ref4, pred_v8, bx, by);
				enc_vp8_ftransform4x4(src4, 4, ref4, 4, vblk[n]);
				enc_vp8_quantize4x4_inplace(vblk[n], qf.uv_dc, qf.uv_ac);
			}

			// Store coeffs for this macroblock.
			int16_t* dst = out + mb_index * coeffs_per_mb;
			for (int i = 0; i < 16; ++i) dst[i] = y2[i];
			dst += 16;
			for (int n = 0; n < 16; ++n) {
				for (int i = 0; i < 16; ++i) dst[i] = tmp[n][i];
				dst += 16;
			}
			for (int n = 0; n < 4; ++n) {
				for (int i = 0; i < 16; ++i) dst[i] = ublk[n][i];
				dst += 16;
			}
			for (int n = 0; n < 4; ++n) {
				for (int i = 0; i < 16; ++i) dst[i] = vblk[n][i];
				dst += 16;
			}

			// Reconstruct Y (mode-aware predictor).
			for (int i = 0; i < 16; ++i) y2_deq[i] = y2[i];
			dequant4x4_inplace(y2_deq, qf.y2_dc, qf.y2_ac);
			inv_wht4x4(y2_deq, y_dc16);
			for (int n = 0; n < 16; ++n) {
				int16_t block_coeffs[16];
				for (int i = 0; i < 16; ++i) block_coeffs[i] = tmp[n][i];
				block_coeffs[0] = y_dc16[n];
				dequant4x4_inplace(block_coeffs, qf.y1_dc, qf.y1_ac);
				int16_t res[16];
				inv_dct4x4(block_coeffs, res);
				const uint32_t bx = (uint32_t)(n & 3) * 4u;
				const uint32_t by = (uint32_t)(n >> 2) * 4u;
				for (uint32_t dy = 0; dy < 4; ++dy) {
					uint8_t* row = recon.y + (size_t)(y0 + by + dy) * recon.y_stride + (size_t)(x0 + bx);
					for (uint32_t dx = 0; dx < 4; ++dx) {
						uint8_t p = pred_y16[(by + dy) * 16u + (bx + dx)];
						int32_t v = (int32_t)p + (int32_t)res[dy * 4 + dx];
						row[dx] = clamp255_i32(v);
					}
				}
			}

			// Reconstruct U.
			for (int n = 0; n < 4; ++n) {
				int16_t block_coeffs[16];
				for (int i = 0; i < 16; ++i) block_coeffs[i] = ublk[n][i];
				dequant4x4_inplace(block_coeffs, qf.uv_dc, qf.uv_ac);
				int16_t res[16];
				inv_dct4x4(block_coeffs, res);
				const uint32_t bx = (uint32_t)(n & 1) * 4u;
				const uint32_t by = (uint32_t)(n >> 1) * 4u;
				for (uint32_t dy = 0; dy < 4; ++dy) {
					uint8_t* row = recon.u + (size_t)(uy0 + by + dy) * recon.uv_stride + (size_t)(ux0 + bx);
					for (uint32_t dx = 0; dx < 4; ++dx) {
						uint8_t p = pred_u8[(by + dy) * 8u + (bx + dx)];
						int32_t v = (int32_t)p + (int32_t)res[dy * 4 + dx];
						row[dx] = clamp255_i32(v);
					}
				}
			}

			// Reconstruct V.
			for (int n = 0; n < 4; ++n) {
				int16_t block_coeffs[16];
				for (int i = 0; i < 16; ++i) block_coeffs[i] = vblk[n][i];
				dequant4x4_inplace(block_coeffs, qf.uv_dc, qf.uv_ac);
				int16_t res[16];
				inv_dct4x4(block_coeffs, res);
				const uint32_t bx = (uint32_t)(n & 1) * 4u;
				const uint32_t by = (uint32_t)(n >> 1) * 4u;
				for (uint32_t dy = 0; dy < 4; ++dy) {
					uint8_t* row = recon.v + (size_t)(uy0 + by + dy) * recon.uv_stride + (size_t)(ux0 + bx);
					for (uint32_t dx = 0; dx < 4; ++dx) {
						uint8_t p = pred_v8[(by + dy) * 8u + (bx + dx)];
						int32_t v = (int32_t)p + (int32_t)res[dy * 4 + dx];
						row[dx] = clamp255_i32(v);
					}
				}
			}
		}
	}

	enc_vp8_recon_free(&recon);
	*y_modes_out = y_modes;
	*y_modes_count_out = (size_t)mb_total;
	*uv_modes_out = uv_modes;
	*uv_modes_count_out = (size_t)mb_total;
	*coeffs_out = out;
	*coeffs_count_out = coeffs_total;
	return 0;
}

int enc_vp8_encode_i16x16_sad_inloop(const EncYuv420Image* yuv,
	                                int quality,
	                                uint8_t** y_modes_out,
	                                size_t* y_modes_count_out,
	                                int16_t** coeffs_out,
	                                size_t* coeffs_count_out,
	                                uint8_t* qindex_out) {
	uint8_t* uv_modes = NULL;
	size_t uv_modes_count = 0;
	int rc = enc_vp8_encode_i16x16_uv_sad_inloop(yuv,
	                                           quality,
	                                           y_modes_out,
	                                           y_modes_count_out,
	                                           &uv_modes,
	                                           &uv_modes_count,
	                                           coeffs_out,
	                                           coeffs_count_out,
	                                           qindex_out);
	free(uv_modes);
	return rc;
}

int enc_vp8_encode_bpred_uv_sad_inloop(const EncYuv420Image* yuv,
								 int quality,
								 uint8_t** y_modes_out,
								 size_t* y_modes_count_out,
								 uint8_t** b_modes_out,
								 size_t* b_modes_count_out,
								 uint8_t** uv_modes_out,
								 size_t* uv_modes_count_out,
								 int16_t** coeffs_out,
								 size_t* coeffs_count_out,
								 uint8_t* qindex_out) {
	if (!y_modes_out || !y_modes_count_out || !b_modes_out || !b_modes_count_out || !uv_modes_out || !uv_modes_count_out ||
	    !coeffs_out || !coeffs_count_out || !qindex_out) {
		errno = EINVAL;
		return -1;
	}
	*y_modes_out = NULL;
	*y_modes_count_out = 0;
	*b_modes_out = NULL;
	*b_modes_count_out = 0;
	*uv_modes_out = NULL;
	*uv_modes_count_out = 0;
	*coeffs_out = NULL;
	*coeffs_count_out = 0;
	*qindex_out = 0;
	if (!yuv || !yuv->y || !yuv->u || !yuv->v || yuv->width == 0 || yuv->height == 0) {
		errno = EINVAL;
		return -1;
	}

	const uint32_t w = yuv->width;
	const uint32_t h = yuv->height;
	const uint32_t mb_cols = (w + 15u) >> 4;
	const uint32_t mb_rows = (h + 15u) >> 4;
	const uint32_t mb_total = mb_cols * mb_rows;
	const size_t coeffs_per_mb = 16 + (16 * 16) + (4 * 16) + (4 * 16);
	const size_t coeffs_total = (size_t)mb_total * coeffs_per_mb;
	if (mb_total == 0 || coeffs_total > (SIZE_MAX / sizeof(int16_t))) {
		errno = EOVERFLOW;
		return -1;
	}

	uint8_t* y_modes = (uint8_t*)malloc((size_t)mb_total);
	uint8_t* uv_modes = (uint8_t*)malloc((size_t)mb_total);
	uint8_t* b_modes = (uint8_t*)malloc((size_t)mb_total * 16u);
	if (!y_modes || !uv_modes || !b_modes) {
		free(y_modes);
		free(uv_modes);
		free(b_modes);
		errno = ENOMEM;
		return -1;
	}

	int16_t* out = (int16_t*)malloc(coeffs_total * sizeof(int16_t));
	if (!out) {
		free(y_modes);
		free(uv_modes);
		free(b_modes);
		errno = ENOMEM;
		return -1;
	}
	memset(out, 0, coeffs_total * sizeof(int16_t));

	EncVp8ReconPlanes recon;
	if (enc_vp8_recon_alloc(w, h, &recon) != 0) {
		free(out);
		free(y_modes);
		free(uv_modes);
		free(b_modes);
		return -1;
	}

	const int qindex = enc_vp8_qindex_from_quality_libwebp(quality);
	EncVp8QuantFactors qf;
	enc_vp8_quant_factors_from_qindex(qindex, 0, 0, 0, 0, 0, &qf);
	*qindex_out = (uint8_t)qf.qindex;

	const uint32_t uv_w = (w + 1u) >> 1;
	const uint32_t uv_h = (h + 1u) >> 1;

	uint8_t src4[16];
	uint8_t pred4[16];
	uint8_t pred_u8[8 * 8];
	uint8_t pred_v8[8 * 8];
	uint8_t pred_u_tmp[8 * 8];
	uint8_t pred_v_tmp[8 * 8];

	for (uint32_t mby = 0; mby < mb_rows; ++mby) {
		for (uint32_t mbx = 0; mbx < mb_cols; ++mbx) {
			const uint32_t x0 = mbx * 16u;
			const uint32_t y0 = mby * 16u;
			const uint32_t ux0 = mbx * 8u;
			const uint32_t uy0 = mby * 8u;
			const size_t mb_index = (size_t)mby * (size_t)mb_cols + (size_t)mbx;
			y_modes[mb_index] = 4; // default to B_PRED; may be overridden by I16 below

			// Choose UV (8x8) mode by SAD against U+V (uses reconstructed chroma neighbors).
			int have_above_c = (mby > 0);
			int have_left_c = (mbx > 0);
			uint8_t A8u[8];
			uint8_t L8u[8];
			uint8_t A8v[8];
			uint8_t L8v[8];
			for (uint32_t i = 0; i < 8; i++) {
				A8u[i] = have_above_c ? recon.u[(size_t)(uy0 - 1) * recon.uv_stride + (size_t)(ux0 + i)] : 127;
				A8v[i] = have_above_c ? recon.v[(size_t)(uy0 - 1) * recon.uv_stride + (size_t)(ux0 + i)] : 127;
				L8u[i] = have_left_c ? recon.u[(size_t)(uy0 + i) * recon.uv_stride + (size_t)(ux0 - 1)] : 129;
				L8v[i] = have_left_c ? recon.v[(size_t)(uy0 + i) * recon.uv_stride + (size_t)(ux0 - 1)] : 129;
			}
			uint8_t above_left_u = 127;
			uint8_t above_left_v = 127;
			if (have_above_c && have_left_c) {
				above_left_u = recon.u[(size_t)(uy0 - 1) * recon.uv_stride + (size_t)(ux0 - 1)];
				above_left_v = recon.v[(size_t)(uy0 - 1) * recon.uv_stride + (size_t)(ux0 - 1)];
			} else {
				uint8_t al = have_above_c ? 129 : 127;
				above_left_u = al;
				above_left_v = al;
			}
			uint32_t best_uv_sad = 0xFFFFFFFFu;
			Vp8I16Mode best_uv_mode = VP8_I16_DC_PRED;
			for (Vp8I16Mode mode = VP8_I16_DC_PRED; mode <= VP8_I16_TM_PRED; mode++) {
				pred8x8_build(pred_u_tmp, mode, A8u, L8u, have_above_c, have_left_c, 127, 129, above_left_u);
				pred8x8_build(pred_v_tmp, mode, A8v, L8v, have_above_c, have_left_c, 127, 129, above_left_v);
				uint32_t sad_u = sad8x8_plane_src_vs_pred(yuv->u, yuv->uv_stride, uv_w, uv_h, ux0, uy0, pred_u_tmp);
				uint32_t sad_v = sad8x8_plane_src_vs_pred(yuv->v, yuv->uv_stride, uv_w, uv_h, ux0, uy0, pred_v_tmp);
				uint32_t sad = sad_u + sad_v;
				if (sad < best_uv_sad) {
					best_uv_sad = sad;
					best_uv_mode = mode;
				}
			}
			pred8x8_build(pred_u8, best_uv_mode, A8u, L8u, have_above_c, have_left_c, 127, 129, above_left_u);
			pred8x8_build(pred_v8, best_uv_mode, A8v, L8v, have_above_c, have_left_c, 127, 129, above_left_v);
			uv_modes[mb_index] = (uint8_t)best_uv_mode;

			// Luma: per subblock choose B mode by SAD; forward transform/quant; reconstruct in scan order.
			for (uint32_t sb_r = 0; sb_r < 4; sb_r++) {
				for (uint32_t sb_c = 0; sb_c < 4; sb_c++) {
					const uint32_t sx = x0 + sb_c * 4u;
					const uint32_t sy = y0 + sb_r * 4u;

					uint8_t A8[9];
					uint8_t L4[4];
					// Top-left (P)
					if (sy == 0) A8[0] = 127;
					else if (sx == 0) A8[0] = 129;
					else A8[0] = recon.y[(size_t)(sy - 1) * recon.y_stride + (size_t)(sx - 1)];

					// Above row A[0..7] in A8[1..8].
					for (uint32_t i = 0; i < 8; i++) {
						if (sy == 0) {
							A8[1 + i] = 127;
							continue;
						}
						uint32_t row = sy - 1;
						uint32_t col;
						if (sb_c == 3 && i >= 4) {
							// RFC 6386 11.4: right-edge special case.
							if (y0 == 0) {
								A8[1 + i] = 127;
								continue;
							}
							row = y0 - 1;
							col = x0 + 16u + (i - 4u);
						} else {
							col = sx + i;
						}
						// recon is padded to macroblock size; still clamp for safety.
						uint32_t max_row = recon.mb_rows * 16u;
						uint32_t max_col = recon.mb_cols * 16u;
						if (max_row) {
							if (row >= max_row) row = max_row - 1u;
						}
						if (max_col) {
							if (col >= max_col) col = max_col - 1u;
						}
						A8[1 + i] = recon.y[(size_t)row * recon.y_stride + (size_t)col];
					}

					// Left column.
					if (sx == 0) {
						for (uint32_t i = 0; i < 4; i++) L4[i] = 129;
					} else {
						for (uint32_t i = 0; i < 4; i++) {
							uint32_t row = sy + i;
							uint32_t max_row = recon.mb_rows * 16u;
							if (max_row && row >= max_row) row = max_row - 1u;
							L4[i] = recon.y[(size_t)row * recon.y_stride + (size_t)(sx - 1)];
						}
					}

					fill4x4_clamped(src4, yuv->y, yuv->y_stride, w, h, sx, sy);

					uint32_t best_sad = 0xFFFFFFFFu;
					Vp8BMode best_mode = B_DC_PRED;
					fill4x4_clamped(src4, yuv->y, yuv->y_stride, w, h, sx, sy);
					for (Vp8BMode mode = B_DC_PRED; mode <= B_HU_PRED; mode++) {
						bpred4x4(pred4, &A8[1], L4, mode);
						uint32_t sad = sad4x4_u8(src4, pred4);
						if (sad < best_sad) {
							best_sad = sad;
							best_mode = mode;
						}
					}
					b_modes[mb_index * 16u + (size_t)(sb_r * 4u + sb_c)] = (uint8_t)best_mode;
					bpred4x4(pred4, &A8[1], L4, best_mode);
					int16_t coeff[16];
					enc_vp8_ftransform4x4(src4, 4, pred4, 4, coeff);
					enc_vp8_quantize4x4_inplace(coeff, qf.y1_dc, qf.y1_ac);

					// Store coeffs: Y2 is not coded; keep it 0. Y blocks start at +16.
					int16_t* mbdst = out + mb_index * coeffs_per_mb;
					int16_t* ydst = mbdst + 16;
					const size_t blk = (size_t)(sb_r * 4u + sb_c);
					for (int i = 0; i < 16; i++) ydst[blk * 16u + (size_t)i] = coeff[i];

					// Reconstruct into recon.y
					int16_t deq[16];
					for (int i = 0; i < 16; i++) deq[i] = coeff[i];
					dequant4x4_inplace(deq, qf.y1_dc, qf.y1_ac);
					int16_t res[16];
					inv_dct4x4(deq, res);
					for (uint32_t dy = 0; dy < 4; dy++) {
						uint8_t* row = recon.y + (size_t)(sy + dy) * recon.y_stride + (size_t)sx;
						for (uint32_t dx = 0; dx < 4; dx++) {
							int32_t v = (int32_t)pred4[dy * 4u + dx] + (int32_t)res[(int)(dy * 4u + dx)];
							row[dx] = clamp255_i32(v);
						}
					}
				}
			}

			// U/V forward transforms + quant (mode-aware predictors).
			int16_t ublk[4][16];
			int16_t vblk[4][16];
			for (uint32_t n = 0; n < 4; ++n) {
				const uint32_t bx = (n & 1u) * 4u;
				const uint32_t by = (n >> 1) * 4u;
				fill4x4_clamped(src4, yuv->u, yuv->uv_stride, uv_w, uv_h, ux0 + bx, uy0 + by);
				pred8_fill4x4(pred4, pred_u8, bx, by);
				enc_vp8_ftransform4x4(src4, 4, pred4, 4, ublk[n]);
				enc_vp8_quantize4x4_inplace(ublk[n], qf.uv_dc, qf.uv_ac);
			}
			for (uint32_t n = 0; n < 4; ++n) {
				const uint32_t bx = (n & 1u) * 4u;
				const uint32_t by = (n >> 1) * 4u;
				fill4x4_clamped(src4, yuv->v, yuv->uv_stride, uv_w, uv_h, ux0 + bx, uy0 + by);
				pred8_fill4x4(pred4, pred_v8, bx, by);
				enc_vp8_ftransform4x4(src4, 4, pred4, 4, vblk[n]);
				enc_vp8_quantize4x4_inplace(vblk[n], qf.uv_dc, qf.uv_ac);
			}

			// Store U/V coeffs after Y.
			int16_t* dst = out + mb_index * coeffs_per_mb + 16 + (16 * 16);
			for (int n = 0; n < 4; ++n) {
				for (int i = 0; i < 16; ++i) dst[i] = ublk[n][i];
				dst += 16;
			}
			for (int n = 0; n < 4; ++n) {
				for (int i = 0; i < 16; ++i) dst[i] = vblk[n][i];
				dst += 16;
			}

			// Reconstruct U/V.
			for (int n = 0; n < 4; ++n) {
				int16_t block_coeffs[16];
				for (int i = 0; i < 16; ++i) block_coeffs[i] = ublk[n][i];
				dequant4x4_inplace(block_coeffs, qf.uv_dc, qf.uv_ac);
				int16_t res[16];
				inv_dct4x4(block_coeffs, res);
				const uint32_t bx = (uint32_t)(n & 1) * 4u;
				const uint32_t by = (uint32_t)(n >> 1) * 4u;
				for (uint32_t dy = 0; dy < 4; ++dy) {
					uint8_t* row = recon.u + (size_t)(uy0 + by + dy) * recon.uv_stride + (size_t)(ux0 + bx);
					for (uint32_t dx = 0; dx < 4; ++dx) {
						uint8_t p = pred_u8[(by + dy) * 8u + (bx + dx)];
						int32_t v = (int32_t)p + (int32_t)res[(int)(dy * 4u + dx)];
						row[dx] = clamp255_i32(v);
					}
				}
			}
			for (int n = 0; n < 4; ++n) {
				int16_t block_coeffs[16];
				for (int i = 0; i < 16; ++i) block_coeffs[i] = vblk[n][i];
				dequant4x4_inplace(block_coeffs, qf.uv_dc, qf.uv_ac);
				int16_t res[16];
				inv_dct4x4(block_coeffs, res);
				const uint32_t bx = (uint32_t)(n & 1) * 4u;
				const uint32_t by = (uint32_t)(n >> 1) * 4u;
				for (uint32_t dy = 0; dy < 4; ++dy) {
					uint8_t* row = recon.v + (size_t)(uy0 + by + dy) * recon.uv_stride + (size_t)(ux0 + bx);
					for (uint32_t dx = 0; dx < 4; ++dx) {
						uint8_t p = pred_v8[(by + dy) * 8u + (bx + dx)];
						int32_t v = (int32_t)p + (int32_t)res[(int)(dy * 4u + dx)];
						row[dx] = clamp255_i32(v);
					}
				}
			}
		}
	}

	enc_vp8_recon_free(&recon);
	*y_modes_out = y_modes;
	*y_modes_count_out = (size_t)mb_total;
	*b_modes_out = b_modes;
	*b_modes_count_out = (size_t)mb_total * 16u;
	*uv_modes_out = uv_modes;
	*uv_modes_count_out = (size_t)mb_total;
	*coeffs_out = out;
	*coeffs_count_out = coeffs_total;
	return 0;
}


static int enc_vp8_encode_bpred_uv_rdo_inloop_pass(const EncYuv420Image* yuv,
								  int quality,
								  const uint8_t coeff_probs_override[4][8][3][11],
							 uint8_t** y_modes_out,
							 size_t* y_modes_count_out,
							 uint8_t** b_modes_out,
							 size_t* b_modes_count_out,
							 uint8_t** uv_modes_out,
							 size_t* uv_modes_count_out,
							 int16_t** coeffs_out,
							 size_t* coeffs_count_out,
							 uint8_t* qindex_out,
							 const EncBpredRdoTuning* tuning) {
	// This largely mirrors enc_vp8_encode_bpred_uv_sad_inloop(), but chooses
	// predictor modes using a quantization-aware distortion estimate:
	//   ftransform -> quantize -> dequant -> inverse transform -> reconstruct
	// and then score SSE vs original.
	if (!y_modes_out || !y_modes_count_out || !b_modes_out || !b_modes_count_out || !uv_modes_out || !uv_modes_count_out ||
	    !coeffs_out || !coeffs_count_out || !qindex_out) {
		errno = EINVAL;
		return -1;
	}
	*y_modes_out = NULL;
	*y_modes_count_out = 0;
	*b_modes_out = NULL;
	*b_modes_count_out = 0;
	*uv_modes_out = NULL;
	*uv_modes_count_out = 0;
	*coeffs_out = NULL;
	*coeffs_count_out = 0;
	*qindex_out = 0;
	if (!yuv || !yuv->y || !yuv->u || !yuv->v || yuv->width == 0 || yuv->height == 0) {
		errno = EINVAL;
		return -1;
	}

	const uint32_t w = yuv->width;
	const uint32_t h = yuv->height;
	const uint32_t mb_cols = (w + 15u) >> 4;
	const uint32_t mb_rows = (h + 15u) >> 4;
	const uint32_t mb_total = mb_cols * mb_rows;
	const size_t coeffs_per_mb = 16 + (16 * 16) + (4 * 16) + (4 * 16);
	const size_t coeffs_total = (size_t)mb_total * coeffs_per_mb;
	if (mb_total == 0 || coeffs_total > (SIZE_MAX / sizeof(int16_t))) {
		errno = EOVERFLOW;
		return -1;
	}

	uint8_t* y_modes = (uint8_t*)malloc((size_t)mb_total);
	uint8_t* uv_modes = (uint8_t*)malloc((size_t)mb_total);
	uint8_t* b_modes = (uint8_t*)malloc((size_t)mb_total * 16u);
	if (!y_modes || !uv_modes || !b_modes) {
		free(y_modes);
		free(uv_modes);
		free(b_modes);
		errno = ENOMEM;
		return -1;
	}

	int16_t* out = (int16_t*)malloc(coeffs_total * sizeof(int16_t));
	if (!out) {
		free(y_modes);
		free(uv_modes);
		free(b_modes);
		errno = ENOMEM;
		return -1;
	}
	memset(out, 0, coeffs_total * sizeof(int16_t));

	EncVp8ReconPlanes recon;
	if (enc_vp8_recon_alloc(w, h, &recon) != 0) {
		free(out);
		free(y_modes);
		free(uv_modes);
		free(b_modes);
		return -1;
	}

	const int qindex = enc_vp8_qindex_from_quality_libwebp(quality);
	EncVp8QuantFactors qf;
	enc_vp8_quant_factors_from_qindex(qindex, 0, 0, 0, 0, 0, &qf);
	*qindex_out = (uint8_t)qf.qindex;

	uint32_t lambda_mul = 1;
	uint32_t lambda_div = 1;
	int rate_mode = 0;
	int use_entropy_rate = 0;
	int use_dry_run_rate = 0;
	int use_entropy_signal = 0;
	int quant_mode = 0;
	uint32_t ac_deadzone_pct = 0;
	uint32_t qscale_y_dc_pct = 100;
	uint32_t qscale_y_ac_pct = 100;
	uint32_t qscale_uv_dc_pct = 100;
	uint32_t qscale_uv_ac_pct = 100;
	uint32_t satd_prune_k = 0;
	const uint32_t satd_prune_min_qindex = 12;
	if (tuning) {
		lambda_mul = tuning->lambda_mul ? tuning->lambda_mul : 1;
		lambda_div = tuning->lambda_div ? tuning->lambda_div : 1;
		rate_mode = (int)tuning->rate_mode;
		use_entropy_rate = (rate_mode != 0);
		use_dry_run_rate = (rate_mode == 2);
		use_entropy_signal = (tuning->signal_mode == 1);
		quant_mode = (tuning->quant_mode == 1) ? 1 : 0;
		ac_deadzone_pct = tuning->ac_deadzone_pct;
		qscale_y_dc_pct = tuning->qscale_y_dc_pct ? tuning->qscale_y_dc_pct : 100;
		qscale_y_ac_pct = tuning->qscale_y_ac_pct ? tuning->qscale_y_ac_pct : 100;
		qscale_uv_dc_pct = tuning->qscale_uv_dc_pct ? tuning->qscale_uv_dc_pct : 100;
		qscale_uv_ac_pct = tuning->qscale_uv_ac_pct ? tuning->qscale_uv_ac_pct : 100;
		satd_prune_k = tuning->satd_prune_k;
		// Apply optional quant step scaling (Experiment 2).
		qf.y1_dc = rdo_scale_step_pct(qf.y1_dc, qscale_y_dc_pct);
		qf.y2_dc = rdo_scale_step_pct(qf.y2_dc, qscale_y_dc_pct);
		qf.y1_ac = rdo_scale_step_pct(qf.y1_ac, qscale_y_ac_pct);
		qf.y2_ac = rdo_scale_step_pct(qf.y2_ac, qscale_y_ac_pct);
		qf.uv_dc = rdo_scale_step_pct(qf.uv_dc, qscale_uv_dc_pct);
		qf.uv_ac = rdo_scale_step_pct(qf.uv_ac, qscale_uv_ac_pct);
	}

	const uint32_t uv_w = (w + 1u) >> 1;
	const uint32_t uv_h = (h + 1u) >> 1;

	uint8_t src4[16];
	uint8_t pred4[16];
	uint8_t pred_u8[8 * 8];
	uint8_t pred_v8[8 * 8];
	uint8_t pred_u_tmp[8 * 8];
	uint8_t pred_v_tmp[8 * 8];

	for (uint32_t mby = 0; mby < mb_rows; ++mby) {
		for (uint32_t mbx = 0; mbx < mb_cols; ++mbx) {
			const uint32_t x0 = mbx * 16u;
			const uint32_t y0 = mby * 16u;
			const uint32_t ux0 = mbx * 8u;
			const uint32_t uy0 = mby * 8u;
			const size_t mb_index = (size_t)mby * (size_t)mb_cols + (size_t)mbx;
			y_modes[mb_index] = 4; // B_PRED

			// --- UV (8x8) mode selection (quantization-aware SSE) ---
			int have_above_c = (mby > 0);
			int have_left_c = (mbx > 0);
			uint8_t A8u[8];
			uint8_t L8u[8];
			uint8_t A8v[8];
			uint8_t L8v[8];
			for (uint32_t i = 0; i < 8; i++) {
				A8u[i] = have_above_c ? recon.u[(size_t)(uy0 - 1) * recon.uv_stride + (size_t)(ux0 + i)] : 127;
				A8v[i] = have_above_c ? recon.v[(size_t)(uy0 - 1) * recon.uv_stride + (size_t)(ux0 + i)] : 127;
				L8u[i] = have_left_c ? recon.u[(size_t)(uy0 + i) * recon.uv_stride + (size_t)(ux0 - 1)] : 129;
				L8v[i] = have_left_c ? recon.v[(size_t)(uy0 + i) * recon.uv_stride + (size_t)(ux0 - 1)] : 129;
			}
			uint8_t above_left_u = 127;
			uint8_t above_left_v = 127;
			if (have_above_c && have_left_c) {
				above_left_u = recon.u[(size_t)(uy0 - 1) * recon.uv_stride + (size_t)(ux0 - 1)];
				above_left_v = recon.v[(size_t)(uy0 - 1) * recon.uv_stride + (size_t)(ux0 - 1)];
			} else {
				uint8_t al = have_above_c ? 129 : 127;
				above_left_u = al;
				above_left_v = al;
			}

			uint32_t best_uv_sse = 0xFFFFFFFFu;
			Vp8I16Mode best_uv_mode = VP8_I16_DC_PRED;
			int16_t best_ublk[4][16];
			int16_t best_vblk[4][16];
			uint8_t best_pred_u8[8 * 8];
			uint8_t best_pred_v8[8 * 8];
			const uint32_t lambda_uv = rdo_lambda_from_qindex(qf.qindex, lambda_mul, lambda_div);

			for (Vp8I16Mode mode = VP8_I16_DC_PRED; mode <= VP8_I16_TM_PRED; mode++) {
				pred8x8_build(pred_u_tmp, mode, A8u, L8u, have_above_c, have_left_c, 127, 129, above_left_u);
				pred8x8_build(pred_v_tmp, mode, A8v, L8v, have_above_c, have_left_c, 127, 129, above_left_v);

				uint32_t sse = 0;
				uint32_t rate = 0;
				rate += rdo_uv_mode_signal_cost(mode);
				if (use_entropy_rate && use_entropy_signal) {
					rate += rdo_rate_from_mode_bits_q8(enc_vp8_estimate_keyframe_uv_mode_bits_q8((int)mode));
				}
				uint8_t u_has[2][2] = {{0, 0}, {0, 0}};
				uint8_t v_has[2][2] = {{0, 0}, {0, 0}};
				int16_t ublk_tmp[4][16];
				int16_t vblk_tmp[4][16];

				for (uint32_t n = 0; n < 4; ++n) {
					const uint32_t bx = (n & 1u) * 4u;
					const uint32_t by = (n >> 1) * 4u;

					fill4x4_clamped(src4, yuv->u, yuv->uv_stride, uv_w, uv_h, ux0 + bx, uy0 + by);
					pred8_fill4x4(pred4, pred_u_tmp, bx, by);
					enc_vp8_ftransform4x4(src4, 4, pred4, 4, ublk_tmp[n]);
					rdo_quantize4x4_inplace(ublk_tmp[n], qf.uv_dc, qf.uv_ac, quant_mode, ac_deadzone_pct);
					refine_dc_quant4x4(ublk_tmp[n], qf.uv_dc, qf.uv_ac, src4, pred4);
					if (use_entropy_rate && !use_dry_run_rate) {
						uint8_t has = 0;
						uint8_t left_has = (bx == 0) ? 0 : u_has[by >> 2][(bx >> 2) - 1];
						uint8_t above_has = (by == 0) ? 0 : u_has[(by >> 2) - 1][bx >> 2];
						rate += rdo_rate_from_token_bits_q8(
							enc_vp8_estimate_keyframe_block_token_bits_q8_probs(
								2,
								0,
								left_has,
								above_has,
								ublk_tmp[n],
								&has,
								coeff_probs_override));
						u_has[by >> 2][bx >> 2] = has;
					} else if (!use_entropy_rate) {
						rate += rdo_rate_proxy4x4(ublk_tmp[n]);
					}
					int16_t deq[16];
					for (int i = 0; i < 16; i++) deq[i] = ublk_tmp[n][i];
					dequant4x4_inplace(deq, qf.uv_dc, qf.uv_ac);
					int16_t res[16];
					inv_dct4x4(deq, res);
					for (int i = 0; i < 16; i++) {
						int32_t v = (int32_t)pred4[i] + (int32_t)res[i];
						uint8_t r = clamp255_i32(v);
						int d = (int)src4[i] - (int)r;
						sse += (uint32_t)(d * d);
					}
				}
				for (uint32_t n = 0; n < 4; ++n) {
					const uint32_t bx = (n & 1u) * 4u;
					const uint32_t by = (n >> 1) * 4u;

					fill4x4_clamped(src4, yuv->v, yuv->uv_stride, uv_w, uv_h, ux0 + bx, uy0 + by);
					pred8_fill4x4(pred4, pred_v_tmp, bx, by);
					enc_vp8_ftransform4x4(src4, 4, pred4, 4, vblk_tmp[n]);
					rdo_quantize4x4_inplace(vblk_tmp[n], qf.uv_dc, qf.uv_ac, quant_mode, ac_deadzone_pct);
					refine_dc_quant4x4(vblk_tmp[n], qf.uv_dc, qf.uv_ac, src4, pred4);
					if (use_entropy_rate && !use_dry_run_rate) {
						uint8_t has = 0;
						uint8_t left_has = (bx == 0) ? 0 : v_has[by >> 2][(bx >> 2) - 1];
						uint8_t above_has = (by == 0) ? 0 : v_has[(by >> 2) - 1][bx >> 2];
						rate += rdo_rate_from_token_bits_q8(
							enc_vp8_estimate_keyframe_block_token_bits_q8_probs(
								2,
								0,
								left_has,
								above_has,
								vblk_tmp[n],
								&has,
								coeff_probs_override));
						v_has[by >> 2][bx >> 2] = has;
					} else if (!use_entropy_rate) {
						rate += rdo_rate_proxy4x4(vblk_tmp[n]);
					}
					int16_t deq[16];
					for (int i = 0; i < 16; i++) deq[i] = vblk_tmp[n][i];
					dequant4x4_inplace(deq, qf.uv_dc, qf.uv_ac);
					int16_t res[16];
					inv_dct4x4(deq, res);
					for (int i = 0; i < 16; i++) {
						int32_t v = (int32_t)pred4[i] + (int32_t)res[i];
						uint8_t r = clamp255_i32(v);
						int d = (int)src4[i] - (int)r;
						sse += (uint32_t)(d * d);
					}
				}

				// If using dry-run rate, add an actual bool-encoder bitcount for just the UV tokens.
				if (use_dry_run_rate) {
					int16_t mb_coeffs_uv[16 + (16 * 16) + (4 * 16) + (4 * 16)];
					memset(mb_coeffs_uv, 0, sizeof(mb_coeffs_uv));
					int16_t* dst_uv = mb_coeffs_uv + 16 + (16 * 16);
					for (int n = 0; n < 4; n++) {
						for (int i = 0; i < 16; i++) dst_uv[i] = ublk_tmp[n][i];
						dst_uv += 16;
					}
					for (int n = 0; n < 4; n++) {
						for (int i = 0; i < 16; i++) dst_uv[i] = vblk_tmp[n][i];
						dst_uv += 16;
					}
					const uint32_t bits_q8 = enc_vp8_dry_run_keyframe_mb_token_bits_q8_probs(/*ymode=*/4, mb_coeffs_uv, coeff_probs_override);
					rate += rdo_rate_from_token_bits_q8(bits_q8);
				}

				uint32_t cost = sse + (uint32_t)((uint64_t)lambda_uv * (uint64_t)rate);
				if (cost < best_uv_sse) {
					best_uv_sse = cost;
					best_uv_mode = mode;
					memcpy(best_pred_u8, pred_u_tmp, sizeof(best_pred_u8));
					memcpy(best_pred_v8, pred_v_tmp, sizeof(best_pred_v8));
					for (int n = 0; n < 4; n++) {
						for (int i = 0; i < 16; i++) best_ublk[n][i] = ublk_tmp[n][i];
						for (int i = 0; i < 16; i++) best_vblk[n][i] = vblk_tmp[n][i];
					}
				}
			}

			memcpy(pred_u8, best_pred_u8, sizeof(pred_u8));
			memcpy(pred_v8, best_pred_v8, sizeof(pred_v8));
			uv_modes[mb_index] = (uint8_t)best_uv_mode;

			const uint32_t lambda_y = rdo_lambda_from_qindex(qf.qindex, lambda_mul, lambda_div);

			// Snapshot the current 16x16 recon luma so we can evaluate candidates without
			// permanently affecting future macroblocks.
			uint8_t saved_y[16 * 16];
			for (uint32_t dy = 0; dy < 16; ++dy) {
				memcpy(saved_y + dy * 16u,
				       recon.y + (size_t)(y0 + dy) * recon.y_stride + (size_t)x0,
				       16);
			}

			// --- Candidate A: B_PRED (4x4) with quant-aware RDO ---
			uint32_t cost_bpred = 0;
			uint8_t y_has_sel[4][4];
			for (int rr = 0; rr < 4; rr++) for (int cc = 0; cc < 4; cc++) y_has_sel[rr][cc] = 0;
			uint8_t cand_b_modes[16];
			int16_t cand_y_coeffs[16][16];
			uint8_t cand_recon_y[16 * 16];
			for (uint32_t sb_r = 0; sb_r < 4; sb_r++) {
				for (uint32_t sb_c = 0; sb_c < 4; sb_c++) {
					const uint32_t sx = x0 + sb_c * 4u;
					const uint32_t sy = y0 + sb_r * 4u;

					uint8_t A8[9];
					uint8_t L4[4];
					if (sy == 0) A8[0] = 127;
					else if (sx == 0) A8[0] = 129;
					else A8[0] = recon.y[(size_t)(sy - 1) * recon.y_stride + (size_t)(sx - 1)];

					for (uint32_t i = 0; i < 8; i++) {
						if (sy == 0) {
							A8[1 + i] = 127;
							continue;
						}
						uint32_t row = sy - 1;
						uint32_t col;
						if (sb_c == 3 && i >= 4) {
							if (y0 == 0) {
								A8[1 + i] = 127;
								continue;
							}
							row = y0 - 1;
							col = x0 + 16u + (i - 4u);
						} else {
							col = sx + i;
						}
						uint32_t max_row = recon.mb_rows * 16u;
						uint32_t max_col = recon.mb_cols * 16u;
						if (max_row && row >= max_row) row = max_row - 1u;
						if (max_col && col >= max_col) col = max_col - 1u;
						A8[1 + i] = recon.y[(size_t)row * recon.y_stride + (size_t)col];
					}

					if (sx == 0) {
						for (uint32_t i = 0; i < 4; i++) L4[i] = 129;
					} else {
						for (uint32_t i = 0; i < 4; i++) {
							uint32_t row = sy + i;
							uint32_t max_row = recon.mb_rows * 16u;
							if (max_row && row >= max_row) row = max_row - 1u;
							L4[i] = recon.y[(size_t)row * recon.y_stride + (size_t)(sx - 1)];
						}
					}

					fill4x4_clamped(src4, yuv->y, yuv->y_stride, w, h, sx, sy);
					const uint32_t blk = sb_r * 4u + sb_c;

					uint32_t best_cost = 0xFFFFFFFFu;
					Vp8BMode best_mode = B_DC_PRED;
					int16_t best_coeff[16];
					uint8_t best_pred4[16];
					uint8_t best_has = 0;
					const uint8_t left_has_ctx = (sb_c == 0) ? 0 : y_has_sel[sb_r][sb_c - 1];
					const uint8_t above_has_ctx = (sb_r == 0) ? 0 : y_has_sel[sb_r - 1][sb_c];

					int cand_modes_sorted[10];
					int cand_modes_count = 0;
					if (satd_prune_k > 0 && satd_prune_k < 10 && (uint32_t)qf.qindex >= satd_prune_min_qindex) {
						uint32_t satd_by_mode[10];
						for (Vp8BMode m = B_DC_PRED; m <= B_HU_PRED; m++) {
							bpred4x4(pred4, &A8[1], L4, m);
							satd_by_mode[(int)m] = satd4x4_src_vs_pred(src4, pred4);
						}
						for (Vp8BMode m = B_DC_PRED; m <= B_HU_PRED; m++) {
							int pos = cand_modes_count;
							while (pos > 0) {
								const int prev = cand_modes_sorted[pos - 1];
								const uint32_t s_new = satd_by_mode[(int)m];
								const uint32_t s_prev = satd_by_mode[prev];
								if (s_new > s_prev) break;
								if (s_new == s_prev && (int)m > prev) break;
								cand_modes_sorted[pos] = cand_modes_sorted[pos - 1];
								pos--;
							}
							cand_modes_sorted[pos] = (int)m;
							cand_modes_count++;
						}
						cand_modes_count = (int)satd_prune_k;
					} else {
						for (Vp8BMode m = B_DC_PRED; m <= B_HU_PRED; m++) cand_modes_sorted[cand_modes_count++] = (int)m;
					}

					for (int mi = 0; mi < cand_modes_count; mi++) {
						const Vp8BMode mode = (Vp8BMode)cand_modes_sorted[mi];
						bpred4x4(pred4, &A8[1], L4, mode);
						int16_t coeff[16];
						enc_vp8_ftransform4x4(src4, 4, pred4, 4, coeff);
						rdo_quantize4x4_inplace(coeff, qf.y1_dc, qf.y1_ac, quant_mode, ac_deadzone_pct);
						refine_dc_quant4x4(coeff, qf.y1_dc, qf.y1_ac, src4, pred4);
						uint32_t rate = 0;
						uint8_t has = 0;
						if (use_entropy_rate) {
							int left_bmode = 0;
							int above_bmode = 0;
							if (sb_c > 0) {
								left_bmode = (int)cand_b_modes[blk - 1u];
							} else if (mbx > 0) {
								const uint32_t mb_left_index = mb_index - 1u;
								const int left_ymode = (int)y_modes[mb_left_index];
								if (left_ymode == 4) {
									left_bmode = (int)b_modes[mb_left_index * 16u + (sb_r * 4u + 3u)];
								} else {
									left_bmode = rdo_mbymode_to_bmode(left_ymode);
								}
							}
							if (sb_r > 0) {
								above_bmode = (int)cand_b_modes[blk - 4u];
							} else if (mby > 0) {
								const uint32_t mb_above_index = mb_index - mb_cols;
								const int above_ymode = (int)y_modes[mb_above_index];
								if (above_ymode == 4) {
									above_bmode = (int)b_modes[mb_above_index * 16u + (3u * 4u + sb_c)];
								} else {
									above_bmode = rdo_mbymode_to_bmode(above_ymode);
								}
							}
							rate += rdo_bmode_signal_cost(mode);
							if (use_entropy_signal) {
								rate += rdo_rate_from_mode_bits_q8(
									enc_vp8_estimate_keyframe_bmode_bits_q8(above_bmode, left_bmode, (int)mode));
							}
							rate += rdo_rate_from_token_bits_q8(
								enc_vp8_estimate_keyframe_block_token_bits_q8_probs(
									3,
									0,
									left_has_ctx,
									above_has_ctx,
									coeff,
									&has,
									coeff_probs_override));
						} else {
							rate += rdo_bmode_signal_cost(mode);
							rate += rdo_rate_proxy4x4(coeff);
							for (int i = 0; i < 16; i++) has |= (uint8_t)(coeff[i] != 0);
						}

						int16_t deq[16];
						for (int i = 0; i < 16; i++) deq[i] = coeff[i];
						dequant4x4_inplace(deq, qf.y1_dc, qf.y1_ac);
						int16_t res[16];
						inv_dct4x4(deq, res);
						uint32_t sse = sse4x4_src_vs_recon(src4, pred4, res);
						uint32_t cost = sse + (uint32_t)((uint64_t)lambda_y * (uint64_t)rate);
						if (cost < best_cost) {
							best_cost = cost;
							best_mode = mode;
							for (int i = 0; i < 16; i++) best_coeff[i] = coeff[i];
							for (int i = 0; i < 16; i++) best_pred4[i] = pred4[i];
							best_has = has;
						}
					}

					cand_b_modes[blk] = (uint8_t)best_mode;
					for (int i = 0; i < 16; i++) cand_y_coeffs[blk][i] = best_coeff[i];
					y_has_sel[sb_r][sb_c] = best_has;
					cost_bpred += best_cost;

					// Reconstruct into recon.y for correct intra context within this macroblock.
					int16_t deq[16];
					for (int i = 0; i < 16; i++) deq[i] = best_coeff[i];
					dequant4x4_inplace(deq, qf.y1_dc, qf.y1_ac);
					int16_t res[16];
					inv_dct4x4(deq, res);
					for (uint32_t dy = 0; dy < 4; dy++) {
						uint8_t* row = recon.y + (size_t)(sy + dy) * recon.y_stride + (size_t)sx;
						for (uint32_t dx = 0; dx < 4; dx++) {
							int32_t v = (int32_t)best_pred4[dy * 4u + dx] + (int32_t)res[(int)(dy * 4u + dx)];
							row[dx] = clamp255_i32(v);
						}
					}
				}
			}
			cost_bpred += (uint32_t)((uint64_t)lambda_y * (uint64_t)rdo_ymode_signal_cost(4));
			if (use_entropy_rate && use_entropy_signal) {
				cost_bpred += (uint32_t)((uint64_t)lambda_y * (uint64_t)rdo_rate_from_mode_bits_q8(enc_vp8_estimate_keyframe_ymode_bits_q8(4)));
			}
			for (uint32_t dy = 0; dy < 16; ++dy) {
				memcpy(cand_recon_y + dy * 16u,
				       recon.y + (size_t)(y0 + dy) * recon.y_stride + (size_t)x0,
				       16);
				memcpy(recon.y + (size_t)(y0 + dy) * recon.y_stride + (size_t)x0,
				       saved_y + dy * 16u,
				       16);
			}

			// --- Candidate B: I16 (16x16) with quant-aware RDO ---
			uint32_t best_cost_i16 = 0xFFFFFFFFu;
			Vp8I16Mode best_i16_mode = VP8_I16_DC_PRED;
			int16_t best_i16_y2[16];
			int16_t best_i16_yblk[16][16];
			uint8_t best_i16_recon_y[16 * 16];

			// Build neighbor vectors from reconstructed luma.
			uint8_t A16[16];
			uint8_t L16[16];
			int have_above = (mby > 0);
			int have_left = (mbx > 0);
			for (uint32_t i = 0; i < 16; i++) {
				A16[i] = have_above ? recon.y[(size_t)(y0 - 1) * recon.y_stride + (size_t)(x0 + i)] : 127;
				L16[i] = have_left ? recon.y[(size_t)(y0 + i) * recon.y_stride + (size_t)(x0 - 1)] : 129;
			}
			uint8_t above_left = 127;
			if (have_above && have_left) {
				above_left = recon.y[(size_t)(y0 - 1) * recon.y_stride + (size_t)(x0 - 1)];
			} else {
				above_left = have_above ? 129 : 127;
			}

			uint8_t pred_tmp[16 * 16];
			uint8_t ref4[16];
			int16_t tmp[16][16];
			int16_t y2[16];
			int16_t y2_deq[16];
			int16_t y_dc16[16];
			uint8_t recon_y_tmp[16 * 16];

			for (Vp8I16Mode mode = VP8_I16_DC_PRED; mode <= VP8_I16_TM_PRED; mode++) {
				pred16x16_build(pred_tmp, mode, A16, L16, have_above, have_left, 127, 129, above_left);

				// Forward transforms, collecting DCs into Y2.
				for (uint32_t n = 0; n < 16; ++n) {
					const uint32_t bx = (n & 3u) * 4u;
					const uint32_t by = (n >> 2) * 4u;
					fill4x4_clamped(src4, yuv->y, yuv->y_stride, w, h, x0 + bx, y0 + by);
					pred16_fill4x4(ref4, pred_tmp, bx, by);
					enc_vp8_ftransform4x4(src4, 4, ref4, 4, tmp[n]);
				}
				enc_vp8_ftransform_wht(&tmp[0][0], y2);
				for (int n = 0; n < 16; ++n) tmp[n][0] = 0;

				// Quantize Y2 and Y blocks.
				int16_t y2q[16];
				for (int i = 0; i < 16; ++i) y2q[i] = y2[i];
				rdo_quantize4x4_inplace(y2q, qf.y2_dc, qf.y2_ac, 0, 0);
				for (int i = 0; i < 16; ++i) y2[i] = y2q[i];
				for (int n = 0; n < 16; ++n) {
					rdo_quantize4x4_inplace(tmp[n], qf.y1_dc, qf.y1_ac, quant_mode, ac_deadzone_pct);
				}

				// Rate term.
				uint32_t rate = 0;
				rate += rdo_ymode_signal_cost((uint8_t)mode);
				if (use_entropy_rate && use_entropy_signal) {
					rate += rdo_rate_from_mode_bits_q8(enc_vp8_estimate_keyframe_ymode_bits_q8((int)mode));
				}
				if (use_entropy_rate) {
					if (!use_dry_run_rate) {
						uint32_t bits_q8 = 0;
						uint8_t y2_has = 0;
						bits_q8 += enc_vp8_estimate_keyframe_block_token_bits_q8(1, 0, 0, 0, y2, &y2_has);
						uint8_t above_y[4] = {0, 0, 0, 0};
						uint8_t left_y[4] = {0, 0, 0, 0};
						uint8_t y_has[4][4];
						for (int rr = 0; rr < 4; rr++) for (int cc = 0; cc < 4; cc++) y_has[rr][cc] = 0;
						for (int rr = 0; rr < 4; rr++) {
							for (int cc = 0; cc < 4; cc++) {
								uint8_t left_has = (cc == 0) ? left_y[rr] : y_has[rr][cc - 1];
								uint8_t above_has = (rr == 0) ? above_y[cc] : y_has[rr - 1][cc];
								uint8_t has = 0;
								bits_q8 += enc_vp8_estimate_keyframe_block_token_bits_q8(0, 1, left_has, above_has, tmp[rr * 4 + cc], &has);
								y_has[rr][cc] = has;
							}
						}
						rate += rdo_rate_from_token_bits_q8(bits_q8);
					} else {
						int16_t mb_coeffs_i16[16 + (16 * 16) + (4 * 16) + (4 * 16)];
						int16_t* dst = mb_coeffs_i16;
						for (int i = 0; i < 16; i++) dst[i] = y2[i];
						dst += 16;
						for (int n = 0; n < 16; n++) {
							for (int i = 0; i < 16; i++) dst[i] = tmp[n][i];
							dst += 16;
						}
						for (int n = 0; n < 4; n++) {
							for (int i = 0; i < 16; i++) dst[i] = best_ublk[n][i];
							dst += 16;
						}
						for (int n = 0; n < 4; n++) {
							for (int i = 0; i < 16; i++) dst[i] = best_vblk[n][i];
							dst += 16;
						}
						const uint32_t bits_q8 = enc_vp8_dry_run_keyframe_mb_token_bits_q8_probs((int)mode, mb_coeffs_i16, coeff_probs_override);
						rate += rdo_rate_from_token_bits_q8(bits_q8);
					}
				} else {
					rate += rdo_rate_weight_y2(rdo_rate_proxy4x4(y2));
					for (int n = 0; n < 16; ++n) rate += rdo_rate_proxy4x4(tmp[n]);
				}

				// Distortion: reconstruct and SSE vs source.
				for (int i = 0; i < 16; ++i) y2_deq[i] = y2[i];
				dequant4x4_inplace(y2_deq, qf.y2_dc, qf.y2_ac);
				inv_wht4x4(y2_deq, y_dc16);

				uint32_t sse_mb = 0;
				for (uint32_t n = 0; n < 16; ++n) {
					int16_t block_coeffs[16];
					for (int i = 0; i < 16; ++i) block_coeffs[i] = tmp[n][i];
					block_coeffs[0] = y_dc16[n];
					dequant4x4_inplace(block_coeffs, qf.y1_dc, qf.y1_ac);
					int16_t res[16];
					inv_dct4x4(block_coeffs, res);
					const uint32_t bx = (uint32_t)(n & 3) * 4u;
					const uint32_t by = (uint32_t)(n >> 2) * 4u;
					fill4x4_clamped(src4, yuv->y, yuv->y_stride, w, h, x0 + bx, y0 + by);
					pred16_fill4x4(ref4, pred_tmp, bx, by);
					for (uint32_t dy = 0; dy < 4; ++dy) {
						for (uint32_t dx = 0; dx < 4; ++dx) {
							const int idx = (int)(dy * 4u + dx);
							int32_t v = (int32_t)ref4[idx] + (int32_t)res[idx];
							uint8_t r = clamp255_i32(v);
							recon_y_tmp[(by + dy) * 16u + (bx + dx)] = r;
							int d = (int)src4[idx] - (int)r;
							sse_mb += (uint32_t)(d * d);
						}
					}
				}

				uint32_t cost = sse_mb + (uint32_t)((uint64_t)lambda_y * (uint64_t)rate);
				if (cost < best_cost_i16) {
					best_cost_i16 = cost;
					best_i16_mode = mode;
					for (int i = 0; i < 16; ++i) best_i16_y2[i] = y2[i];
					for (int n = 0; n < 16; ++n) {
						for (int i = 0; i < 16; ++i) best_i16_yblk[n][i] = tmp[n][i];
					}
					memcpy(best_i16_recon_y, recon_y_tmp, sizeof(best_i16_recon_y));
				}
			}

			// Decide macroblock luma mode and commit coeffs + recon.
			uint32_t cost_bpred_cmp = cost_bpred;
			if (use_dry_run_rate) {
				// Recompute B_PRED macroblock cost using a dry-run token bitcount (mode selection inside
				// the macroblock was still driven by the usual estimator; this improves the I16 vs B_PRED decision).
				uint32_t sse_y = 0;
				for (uint32_t dy = 0; dy < 16u; ++dy) {
					uint32_t sy = y0 + dy;
					if (sy >= h) sy = h ? (h - 1u) : 0u;
					const uint8_t* src_row = yuv->y + (size_t)sy * yuv->y_stride;
					for (uint32_t dx = 0; dx < 16u; ++dx) {
						uint32_t sx = x0 + dx;
						if (sx >= w) sx = w ? (w - 1u) : 0u;
						int d = (int)src_row[sx] - (int)cand_recon_y[dy * 16u + dx];
						sse_y += (uint32_t)(d * d);
					}
				}
				uint32_t rate = 0;
				rate += rdo_ymode_signal_cost(4);
				if (use_entropy_signal) {
					rate += rdo_rate_from_mode_bits_q8(enc_vp8_estimate_keyframe_ymode_bits_q8(4));
				}
				for (uint32_t rr = 0; rr < 4; rr++) {
					for (uint32_t cc = 0; cc < 4; cc++) {
						const uint32_t blk = rr * 4u + cc;
						const int mode = (int)cand_b_modes[blk];
						rate += rdo_bmode_signal_cost(mode);
						if (use_entropy_signal) {
							int left_bmode = 0;
							int above_bmode = 0;
							if (cc > 0) {
								left_bmode = (int)cand_b_modes[blk - 1u];
							} else if (mbx > 0) {
								const uint32_t mb_left_index = (uint32_t)mb_index - 1u;
								const int left_ymode = (int)y_modes[mb_left_index];
								if (left_ymode == 4) left_bmode = (int)b_modes[mb_left_index * 16u + (rr * 4u + 3u)];
								else left_bmode = rdo_mbymode_to_bmode(left_ymode);
							}
							if (rr > 0) {
								above_bmode = (int)cand_b_modes[blk - 4u];
							} else if (mby > 0) {
								const uint32_t mb_above_index = (uint32_t)mb_index - mb_cols;
								const int above_ymode = (int)y_modes[mb_above_index];
								if (above_ymode == 4) above_bmode = (int)b_modes[mb_above_index * 16u + (3u * 4u + cc)];
								else above_bmode = rdo_mbymode_to_bmode(above_ymode);
							}
							rate += rdo_rate_from_mode_bits_q8(enc_vp8_estimate_keyframe_bmode_bits_q8(above_bmode, left_bmode, mode));
						}
					}
				}
				int16_t mb_coeffs_bp[16 + (16 * 16) + (4 * 16) + (4 * 16)];
				memset(mb_coeffs_bp, 0, 16 * sizeof(int16_t));
				int16_t* dst = mb_coeffs_bp + 16;
				for (int n = 0; n < 16; n++) {
					for (int i = 0; i < 16; i++) dst[i] = cand_y_coeffs[n][i];
					dst += 16;
				}
				for (int n = 0; n < 4; n++) {
					for (int i = 0; i < 16; i++) dst[i] = best_ublk[n][i];
					dst += 16;
				}
				for (int n = 0; n < 4; n++) {
					for (int i = 0; i < 16; i++) dst[i] = best_vblk[n][i];
					dst += 16;
				}
				const uint32_t bits_q8 = enc_vp8_dry_run_keyframe_mb_token_bits_q8_probs(/*ymode=*/4, mb_coeffs_bp, coeff_probs_override);
				rate += rdo_rate_from_token_bits_q8(bits_q8);
				cost_bpred_cmp = sse_y + (uint32_t)((uint64_t)lambda_y * (uint64_t)rate);
			}

			int choose_i16 = (best_cost_i16 < cost_bpred_cmp);
			int16_t* mbdst = out + mb_index * coeffs_per_mb;
			int16_t* ydst = mbdst + 16;
			if (choose_i16) {
				y_modes[mb_index] = (uint8_t)best_i16_mode;
				for (int i = 0; i < 16; ++i) mbdst[i] = best_i16_y2[i];
				for (int n = 0; n < 16; ++n) {
					for (int i = 0; i < 16; ++i) ydst[(size_t)n * 16u + (size_t)i] = best_i16_yblk[n][i];
					b_modes[mb_index * 16u + (size_t)n] = 0;
				}
				for (uint32_t dy = 0; dy < 16; ++dy) {
					memcpy(recon.y + (size_t)(y0 + dy) * recon.y_stride + (size_t)x0,
					       best_i16_recon_y + dy * 16u,
					       16);
				}
			} else {
				y_modes[mb_index] = 4; // B_PRED
				// Y2 remains 0 for B_PRED; out is zero-initialized.
				for (int n = 0; n < 16; ++n) {
					for (int i = 0; i < 16; ++i) ydst[(size_t)n * 16u + (size_t)i] = cand_y_coeffs[n][i];
					b_modes[mb_index * 16u + (size_t)n] = cand_b_modes[n];
				}
				for (uint32_t dy = 0; dy < 16; ++dy) {
					memcpy(recon.y + (size_t)(y0 + dy) * recon.y_stride + (size_t)x0,
					       cand_recon_y + dy * 16u,
					       16);
				}
			}

			// Store U/V coeffs after Y.
			int16_t* dst = out + mb_index * coeffs_per_mb + 16 + (16 * 16);
			for (int n = 0; n < 4; ++n) {
				for (int i = 0; i < 16; ++i) dst[i] = best_ublk[n][i];
				dst += 16;
			}
			for (int n = 0; n < 4; ++n) {
				for (int i = 0; i < 16; ++i) dst[i] = best_vblk[n][i];
				dst += 16;
			}

			// Reconstruct U/V.
			for (int n = 0; n < 4; ++n) {
				int16_t block_coeffs[16];
				for (int i = 0; i < 16; ++i) block_coeffs[i] = best_ublk[n][i];
				dequant4x4_inplace(block_coeffs, qf.uv_dc, qf.uv_ac);
				int16_t res[16];
				inv_dct4x4(block_coeffs, res);
				const uint32_t bx = (uint32_t)(n & 1) * 4u;
				const uint32_t by = (uint32_t)(n >> 1) * 4u;
				for (uint32_t dy = 0; dy < 4; ++dy) {
					uint8_t* row = recon.u + (size_t)(uy0 + by + dy) * recon.uv_stride + (size_t)(ux0 + bx);
					for (uint32_t dx = 0; dx < 4; ++dx) {
						uint8_t p = pred_u8[(by + dy) * 8u + (bx + dx)];
						int32_t v = (int32_t)p + (int32_t)res[(int)(dy * 4u + dx)];
						row[dx] = clamp255_i32(v);
					}
				}
			}
			for (int n = 0; n < 4; ++n) {
				int16_t block_coeffs[16];
				for (int i = 0; i < 16; ++i) block_coeffs[i] = best_vblk[n][i];
				dequant4x4_inplace(block_coeffs, qf.uv_dc, qf.uv_ac);
				int16_t res[16];
				inv_dct4x4(block_coeffs, res);
				const uint32_t bx = (uint32_t)(n & 1) * 4u;
				const uint32_t by = (uint32_t)(n >> 1) * 4u;
				for (uint32_t dy = 0; dy < 4; ++dy) {
					uint8_t* row = recon.v + (size_t)(uy0 + by + dy) * recon.uv_stride + (size_t)(ux0 + bx);
					for (uint32_t dx = 0; dx < 4; ++dx) {
						uint8_t p = pred_v8[(by + dy) * 8u + (bx + dx)];
						int32_t v = (int32_t)p + (int32_t)res[(int)(dy * 4u + dx)];
						row[dx] = clamp255_i32(v);
					}
				}
			}
		}
	}

	enc_vp8_recon_free(&recon);
	*y_modes_out = y_modes;
	*y_modes_count_out = (size_t)mb_total;
	*b_modes_out = b_modes;
	*b_modes_count_out = (size_t)mb_total * 16u;
	*uv_modes_out = uv_modes;
	*uv_modes_count_out = (size_t)mb_total;
	*coeffs_out = out;
	*coeffs_count_out = coeffs_total;
	return 0;
}
