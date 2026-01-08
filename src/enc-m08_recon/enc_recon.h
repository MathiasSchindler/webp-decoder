#pragma once

#include <stddef.h>
#include <stdint.h>

#include "../enc-m04_yuv/enc_rgb_to_yuv.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
	uint32_t width;
	uint32_t height;
	uint32_t mb_cols;
	uint32_t mb_rows;
	uint32_t y_stride;
	uint32_t uv_stride;
	uint8_t* y;
	uint8_t* u;
	uint8_t* v;
} EncVp8ReconPlanes;

int enc_vp8_recon_alloc(uint32_t width, uint32_t height, EncVp8ReconPlanes* out);
void enc_vp8_recon_free(EncVp8ReconPlanes* p);

// Encode a VP8 keyframe (payload bytes, not RIFF) using DC_PRED for luma and chroma,
// generating quantized coefficients and reconstructing pixels in-loop so that
// subsequent predictions use reconstructed neighbors.
//
// - quality: [0..100] mapped to qindex via libwebp mapping.
// - coeffs_out: int16 coefficients in natural order, per macroblock layout:
//   Y2(16) + Y(16*16) + U(4*16) + V(4*16).
//
// Returns 0 on success, allocates *coeffs_out (caller frees).
int enc_vp8_encode_dc_pred_inloop(const EncYuv420Image* yuv,
                                 int quality,
                                 int16_t** coeffs_out,
                                 size_t* coeffs_count_out,
                                 uint8_t* qindex_out);

// Like enc_vp8_encode_dc_pred_inloop(), but chooses the luma macroblock intra mode
// (I16) per macroblock among {DC_PRED,V_PRED,H_PRED,TM_PRED} using SAD against
// predictors built from reconstructed neighbors.
//
// Outputs:
// - y_modes_out: array of length mb_total (bytes), values 0..3 mapping to VP8
//   intra_mbmode: DC=0, V=1, H=2, TM=3.
// - coeffs_out: same layout as enc_vp8_encode_dc_pred_inloop().
int enc_vp8_encode_i16x16_sad_inloop(const EncYuv420Image* yuv,
									int quality,
									uint8_t** y_modes_out,
									size_t* y_modes_count_out,
									int16_t** coeffs_out,
									size_t* coeffs_count_out,
									uint8_t* qindex_out);

// Like enc_vp8_encode_i16x16_sad_inloop(), but also chooses UV (8x8) intra mode
// per macroblock among {DC_PRED,V_PRED,H_PRED,TM_PRED} using SAD against U+V.
//
// Outputs:
// - y_modes_out: length mb_total, values 0..3
// - uv_modes_out: length mb_total, values 0..3
int enc_vp8_encode_i16x16_uv_sad_inloop(const EncYuv420Image* yuv,
									   int quality,
									   uint8_t** y_modes_out,
									   size_t* y_modes_count_out,
									   uint8_t** uv_modes_out,
									   size_t* uv_modes_count_out,
									   int16_t** coeffs_out,
									   size_t* coeffs_count_out,
									   uint8_t* qindex_out);

// Encode a VP8 keyframe (in-loop) using B_PRED (4x4 luma intra) for every
// macroblock.
//
// - Luma: per-subblock b_modes chosen by SAD among the 10 VP8 4x4 intra modes.
// - Chroma: per-macroblock UV mode chosen by SAD among {DC,V,H,TM}.
//
// Output modes:
// - y_modes_out: length mb_total, always 4 (B_PRED)
// - b_modes_out: length mb_total*16, values 0..9 (VP8 intra_bmode)
// - uv_modes_out: length mb_total, values 0..3
//
// Coeff layout matches other encoders: Y2(16) + Y(16*16) + U(4*16) + V(4*16).
// For B_PRED, Y2 is not coded; this function writes Y2 coeffs as 0.
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
								 uint8_t* qindex_out);

#ifdef __cplusplus
}
#endif
