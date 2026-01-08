#include "enc_quant.h"

#include <math.h>
#include <stddef.h>

static inline int clamp_i32(int v, int lo, int hi) {
	if (v < lo) return lo;
	if (v > hi) return hi;
	return v;
}

// From RFC 6386 (dequant_data.h). Duplicated here to keep encoder-side logic
// self-contained.
#define QINDEX_RANGE 128
static const int dc_qlookup[QINDEX_RANGE] = {
	4, 5, 6, 7, 8, 9, 10, 10, 11, 12, 13, 14, 15, 16, 17, 17, 18, 19, 20, 20, 21, 21, 22, 22, 23, 23,
	24, 25, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 37, 38, 39, 40, 41, 42, 43, 44, 45,
	46, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63, 64, 65, 66, 67, 68,
	69, 70, 71, 72, 73, 74, 75, 76, 76, 77, 78, 79, 80, 81, 82, 83, 84, 85, 86, 87, 88, 89, 91, 93,
	95, 96, 98, 100, 101, 102, 104, 106, 108, 110, 112, 114, 116, 118, 122, 124, 126, 128, 130, 132, 134,
	136, 138, 140, 143, 145, 148, 151, 154, 157,
};

static const int ac_qlookup[QINDEX_RANGE] = {
	4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29,
	30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53,
	54, 55, 56, 57, 58, 60, 62, 64, 66, 68, 70, 72, 74, 76, 78, 80, 82, 84, 86, 88, 90, 92, 94, 96,
	98, 100, 102, 104, 106, 108, 110, 112, 114, 116, 119, 122, 125, 128, 131, 134, 137, 140, 143, 146, 149,
	152, 155, 158, 161, 164, 167, 170, 173, 177, 181, 185, 189, 193, 197, 201, 205, 209, 213, 217, 221, 225,
	229, 234, 239, 245, 249, 254, 259, 264, 269, 274, 279, 284,
};

static inline int dc_q(int q) { return dc_qlookup[clamp_i32(q, 0, 127)]; }
static inline int ac_q(int q) { return ac_qlookup[clamp_i32(q, 0, 127)]; }

// libwebp: QualityToCompression (quant_enc.c)
static double quality_to_compression(double q01) {
	const double linear_c = (q01 < 0.75) ? q01 * (2. / 3.) : 2. * q01 - 1.;
	return pow(linear_c, 1.0 / 3.0);
}

int enc_vp8_qindex_from_quality_libwebp(int quality) {
	quality = clamp_i32(quality, 0, 100);
	const double q01 = (double)quality / 100.0;
	const double c = quality_to_compression(q01);
	const int qindex = (int)(127.0 * (1.0 - c));
	return clamp_i32(qindex, 0, 127);
}

void enc_vp8_quant_factors_from_qindex(int qindex,
                                      int y1_dc_delta,
                                      int y2_dc_delta,
                                      int y2_ac_delta,
                                      int uv_dc_delta,
                                      int uv_ac_delta,
                                      EncVp8QuantFactors* out) {
	if (!out) return;
	qindex = clamp_i32(qindex, 0, 127);
	out->qindex = qindex;
	out->y1_dc = dc_q(qindex + y1_dc_delta);
	out->y1_ac = ac_q(qindex);
	out->uv_dc = dc_q(qindex + uv_dc_delta);
	out->uv_ac = ac_q(qindex + uv_ac_delta);
	out->y2_dc = dc_q(qindex + y2_dc_delta) * 2;
	out->y2_ac = ac_q(qindex + y2_ac_delta) * 155 / 100;
	if (out->y2_ac < 8) out->y2_ac = 8;
	if (out->uv_dc > 132) out->uv_dc = 132;
}

static inline int16_t quant_one(int16_t c, int step) {
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

void enc_vp8_quantize4x4_inplace(int16_t coeffs[16], int dc_step, int ac_step) {
	if (!coeffs) return;
	coeffs[0] = quant_one(coeffs[0], dc_step);
	for (int i = 1; i < 16; ++i) {
		coeffs[i] = quant_one(coeffs[i], ac_step);
	}
}
