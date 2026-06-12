#include "enc_quant.h"

#include "enc_quality_table.h"

#include <stddef.h>

static inline int clamp_i32(int v, int lo, int hi) {
	if (v < lo) return lo;
	if (v > hi) return hi;
	return v;
}

int enc_vp8_qindex_from_quality_libwebp(int quality) {
	quality = clamp_i32(quality, 0, 100);
	return (int)enc_qindex_from_quality[quality];
}

void enc_vp8_quant_factors_from_qindex(int qindex,
                                      int y1_dc_delta,
                                      int y2_dc_delta,
                                      int y2_ac_delta,
                                      int uv_dc_delta,
                                      int uv_ac_delta,
                                      EncVp8QuantFactors* out) {
	vp8_quant_factors_from_qindex(qindex, y1_dc_delta, y2_dc_delta, y2_ac_delta, uv_dc_delta, uv_ac_delta, out);
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
