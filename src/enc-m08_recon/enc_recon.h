#pragma once

#include <stddef.h>
#include <stdint.h>

#include "../enc-m04_yuv/enc_rgb_to_yuv.h"
#include "../enc-m07_tokens/enc_vp8_tokens.h"

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

typedef struct {
	// Scales the internal lambda(qindex) used by bpred-rdo's RDO-lite cost.
	// Effective lambda = base_lambda(qindex) * lambda_mul / lambda_div.
	uint32_t lambda_mul;
	uint32_t lambda_div;
	// 0: use the existing cheap magnitude proxy (default)
	// 1: use an entropy-style token cost estimator (experimental)
	// 2: use a dry-run bool-encoder token bitcount (experimental)
	uint32_t rate_mode;
	// Mode signaling rate term:
	// 0: use the existing tiny fixed proxy costs (default)
	// 1: add entropy-style mode signaling bit-costs (experimental)
	uint32_t signal_mode;
	// 0: default quantization (shared with other modes)
	// 1: AC deadzone quantization (experimental; bpred-rdo only)
	uint32_t quant_mode;
	// Only used when quant_mode=1.
	// AC coefficients with |v| < (ac_deadzone_pct/100)*step are quantized to 0.
	// If 0, an internal default is used.
	uint32_t ac_deadzone_pct;
	// Optional quant step scaling (percentage). If 0, treated as 100.
	// Increasing a step size generally reduces bitrate at some quality loss.
	uint32_t qscale_y_dc_pct;
	uint32_t qscale_y_ac_pct;
	uint32_t qscale_uv_dc_pct;
	uint32_t qscale_uv_ac_pct;
	// Optional 4x4 bpred candidate pruning using SATD/Hadamard pre-score.
	// 0: disabled (default; evaluates all 10 modes)
	// N>0: evaluate only the best N modes by SATD (tie-break by mode id)
	uint32_t satd_prune_k;
} EncBpredRdoTuning;

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

// Experimental: like enc_vp8_encode_bpred_uv_sad_inloop(), but chooses modes
// using a quantization-aware distortion estimate.
//
// For each candidate predictor mode, it runs ftransform -> quantize -> dequant
// -> inverse transform, reconstructs pixels, and scores SSE vs original.
// This is an RDO-lite distortion term (no explicit rate term yet).
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
				 const EncBpredRdoTuning* tuning);

#ifdef __cplusplus
}
#endif
