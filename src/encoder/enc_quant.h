#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
	int qindex;  // [0..127]
	int y1_dc;
	int y1_ac;
	int y2_dc;
	int y2_ac;
	int uv_dc;
	int uv_ac;
} EncVp8QuantFactors;

// Mirrors libwebp's base quantizer selection for a single segment.
// Input: quality in [0..100]. Output: qindex in [0..127].
int enc_vp8_qindex_from_quality_libwebp(int quality);

// Computes VP8 dequant factors (used as quant step sizes) for the given qindex
// and deltas, mirroring RFC 6386 reference behavior (and libwebp's scalar path).
void enc_vp8_quant_factors_from_qindex(int qindex,
                                      int y1_dc_delta,
                                      int y2_dc_delta,
                                      int y2_ac_delta,
                                      int uv_dc_delta,
                                      int uv_ac_delta,
                                      EncVp8QuantFactors* out);

// Quantizes a 4x4 block in-place with separate DC/AC step sizes.
void enc_vp8_quantize4x4_inplace(int16_t coeffs[16], int dc_step, int ac_step);

#ifdef __cplusplus
}
#endif
