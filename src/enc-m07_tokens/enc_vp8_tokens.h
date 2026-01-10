#pragma once

#include <stddef.h>
#include <stdint.h>

#include "../enc-m08_filter/enc_loopfilter.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ENC_VP8_TOKEN_PROBS_DEFAULT = 0,
    ENC_VP8_TOKEN_PROBS_ADAPTIVE = 1,
    ENC_VP8_TOKEN_PROBS_ADAPTIVE2 = 2,
} EncVp8TokenProbsMode;

// Build a VP8 keyframe payload (not RIFF/WebP wrapper) for arbitrary dimensions,
// with all macroblocks using DC_PRED (Y and UV), and coefficient tokens encoded
// from the provided quantized coefficient buffers.
//
// Layout for coeffs_per_mb, per macroblock:
// - Y2: 1 block  (16 coeffs)
// - Y:  16 blocks (16 coeffs each)
// - U:  4 blocks  (16 coeffs each)
// - V:  4 blocks  (16 coeffs each)
//
// Coefficients are in natural order (not zigzag), matching Vp8DecodedFrame.
//
// q_index and deltas are written into the frame header (purely indicative for
// coefficient decoding, but required for a coherent bitstream).
int enc_vp8_build_keyframe_dc_coeffs(uint32_t width,
                                    uint32_t height,
                                    uint8_t q_index,
                                    int8_t y1_dc_delta_q,
                                    int8_t y2_dc_delta_q,
                                    int8_t y2_ac_delta_q,
                                    int8_t uv_dc_delta_q,
                                    int8_t uv_ac_delta_q,
	                            const int16_t* coeffs,
	                            size_t coeffs_count,
                                    uint8_t** out_payload,
                                    size_t* out_size);

// Like enc_vp8_build_keyframe_dc_coeffs(), but allows setting loopfilter params.
// If lf is NULL, defaults to {use_simple=0, level=0, sharpness=0, use_lf_delta=0}.
int enc_vp8_build_keyframe_dc_coeffs_ex(uint32_t width,
                                       uint32_t height,
                                       uint8_t q_index,
                                       int8_t y1_dc_delta_q,
                                       int8_t y2_dc_delta_q,
                                       int8_t y2_ac_delta_q,
                                       int8_t uv_dc_delta_q,
                                       int8_t uv_ac_delta_q,
                                       const EncVp8LoopFilterParams* lf,
                                       const int16_t* coeffs,
                                       size_t coeffs_count,
                                       uint8_t** out_payload,
                                       size_t* out_size);

// Build a VP8 keyframe payload like enc_vp8_build_keyframe_dc_coeffs(), but with
// per-macroblock intra modes signaled in partition 0.
//
// y_modes/uv_modes are arrays of length mb_total (mb_cols*mb_rows). Values are
// VP8 intra_mbmode: DC=0, V=1, H=2, TM=3. (B_PRED not supported here.)
// Passing NULL for either array defaults that plane to DC for all macroblocks.
int enc_vp8_build_keyframe_i16_coeffs(uint32_t width,
                                     uint32_t height,
                                     uint8_t q_index,
                                     int8_t y1_dc_delta_q,
                                     int8_t y2_dc_delta_q,
                                     int8_t y2_ac_delta_q,
                                     int8_t uv_dc_delta_q,
                                     int8_t uv_ac_delta_q,
                                     const uint8_t* y_modes,
                                     const uint8_t* uv_modes,
                                     const int16_t* coeffs,
                                     size_t coeffs_count,
                                     uint8_t** out_payload,
                                     size_t* out_size);

// Like enc_vp8_build_keyframe_i16_coeffs(), but allows setting loopfilter params.
// If lf is NULL, defaults to {use_simple=0, level=0, sharpness=0, use_lf_delta=0}.
int enc_vp8_build_keyframe_i16_coeffs_ex(uint32_t width,
                                        uint32_t height,
                                        uint8_t q_index,
                                        int8_t y1_dc_delta_q,
                                        int8_t y2_dc_delta_q,
                                        int8_t y2_ac_delta_q,
                                        int8_t uv_dc_delta_q,
                                        int8_t uv_ac_delta_q,
                                        const uint8_t* y_modes,
                                        const uint8_t* uv_modes,
                                        const EncVp8LoopFilterParams* lf,
                                        const int16_t* coeffs,
                                        size_t coeffs_count,
                                        uint8_t** out_payload,
                                        size_t* out_size);

// Build a VP8 keyframe payload like enc_vp8_build_keyframe_i16_coeffs(), but
// also supports B_PRED (4x4 luma intra) and its per-subblock b_modes.
//
// y_modes values are VP8 intra_mbmode: DC=0, V=1, H=2, TM=3, B_PRED=4.
//
// If ymode==B_PRED for a macroblock, then:
// - has_y2 is false (no Y2 tokens are coded)
// - b_modes must provide 16 subblock intra modes for that macroblock.
//
// b_modes layout: mb_total*16 bytes, subblock order rr-major (rr 0..3, cc 0..3).
// Values are VP8 intra_bmode: 0..9.
// Passing NULL for b_modes defaults all subblocks to B_DC_PRED.
int enc_vp8_build_keyframe_intra_coeffs(uint32_t width,
                                       uint32_t height,
                                       uint8_t q_index,
                                       int8_t y1_dc_delta_q,
                                       int8_t y2_dc_delta_q,
                                       int8_t y2_ac_delta_q,
                                       int8_t uv_dc_delta_q,
                                       int8_t uv_ac_delta_q,
                                       const uint8_t* y_modes,
                                       const uint8_t* uv_modes,
                                       const uint8_t* b_modes,
                                       const int16_t* coeffs,
                                       size_t coeffs_count,
                                       uint8_t** out_payload,
                                       size_t* out_size);

// Like enc_vp8_build_keyframe_intra_coeffs(), but allows setting loopfilter params.
// If lf is NULL, defaults to {use_simple=0, level=0, sharpness=0, use_lf_delta=0}.
int enc_vp8_build_keyframe_intra_coeffs_ex(uint32_t width,
                                          uint32_t height,
                                          uint8_t q_index,
                                          int8_t y1_dc_delta_q,
                                          int8_t y2_dc_delta_q,
                                          int8_t y2_ac_delta_q,
                                          int8_t uv_dc_delta_q,
                                          int8_t uv_ac_delta_q,
					  int enable_mb_skip,
                                          const uint8_t* y_modes,
                                          const uint8_t* uv_modes,
                                          const uint8_t* b_modes,
                                          const EncVp8LoopFilterParams* lf,
                                          const int16_t* coeffs,
                                          size_t coeffs_count,
                                          uint8_t** out_payload,
                                          size_t* out_size);

// Like enc_vp8_build_keyframe_intra_coeffs_ex(), but allows optional coefficient
// token probability updates (keyframes only).
//
// When probs_mode==ENC_VP8_TOKEN_PROBS_DEFAULT, the output is identical to
// enc_vp8_build_keyframe_intra_coeffs_ex() (no probability updates emitted).
int enc_vp8_build_keyframe_intra_coeffs_ex_probs(uint32_t width,
                                                 uint32_t height,
                                                 uint8_t q_index,
                                                 int8_t y1_dc_delta_q,
                                                 int8_t y2_dc_delta_q,
                                                 int8_t y2_ac_delta_q,
                                                 int8_t uv_dc_delta_q,
                                                 int8_t uv_ac_delta_q,
						   int enable_mb_skip,
                                                 const uint8_t* y_modes,
                                                 const uint8_t* uv_modes,
                                                 const uint8_t* b_modes,
                                                 const EncVp8LoopFilterParams* lf,
                                                 EncVp8TokenProbsMode probs_mode,
                                                 const int16_t* coeffs,
                                                 size_t coeffs_count,
                                                 uint8_t** out_payload,
                                                 size_t* out_size);

// --- Experimental helpers (encoder-side estimation) ---
//
// Estimate the VP8 coefficient token cost (entropy-style), using the default
// probability tables and coefficient token tree.
//
// Returns a cost in Q8 "bits" (i.e. 1 bit == 256).
//
// coeff_plane selects the default coefficient probability set:
//   0: Y (I16: AC-only blocks)
//   1: Y2 (I16 DC/WHT block)
//   2: UV
//   3: Y (B_PRED/DC mode: full blocks incl DC)
//
// first_coeff is 0 for full blocks, 1 for AC-only blocks.
// left_has/above_has are the usual VP8 contexts (0/1) indicating whether the
// neighbor block had any non-zero coefficients.
// out_has_coeffs (optional) is set to 1 if this block has any non-zero coeffs.
uint32_t enc_vp8_estimate_keyframe_block_token_bits_q8(int coeff_plane,
                                                      int first_coeff,
                                                      uint8_t left_has,
                                                      uint8_t above_has,
                                                      const int16_t block[16],
                                                      uint8_t* out_has_coeffs);

// Like enc_vp8_estimate_keyframe_block_token_bits_q8(), but uses an explicit
// coefficient probability table override (e.g. to better match adaptive probs).
//
// If coeff_probs_override is NULL, this falls back to the default tables.
uint32_t enc_vp8_estimate_keyframe_block_token_bits_q8_probs(int coeff_plane,
                                                            int first_coeff,
                                                            uint8_t left_has,
                                                            uint8_t above_has,
                                                            const int16_t block[16],
                                                            uint8_t* out_has_coeffs,
                                                            const uint8_t coeff_probs_override[4][8][3][11]);

// Compute the coefficient probability table we'd emit for a keyframe when using
// adaptive token probs (based on coefficient branch counts + update heuristics).
//
// This is intentionally deterministic.
void enc_vp8_compute_adaptive_coeff_probs(uint8_t out_probs[4][8][3][11],
                                         uint32_t mb_cols,
                                         uint32_t mb_rows,
                                         const uint8_t* y_modes,
                                         const int16_t* coeffs);

// Alternative deterministic adaptive prob strategy (Experiment 3).
// Uses a stronger per-band/context prior and a stricter savings-vs-overhead rule.
void enc_vp8_compute_adaptive_coeff_probs2(uint8_t out_probs[4][8][3][11],
                                          uint32_t mb_cols,
                                          uint32_t mb_rows,
                                          const uint8_t* y_modes,
                                          const int16_t* coeffs);

// Estimate the macroblock token cost (coeffs only) for keyframes, assuming
// external contexts are 0. Uses the standard VP8 per-block context propagation
// within the macroblock.
//
// ymode is VP8 intra_mbmode (0..3 for I16, 4 for B_PRED).
// mb_coeffs uses the standard layout: Y2(16) + Y(16*16) + U(4*16) + V(4*16).
uint32_t enc_vp8_estimate_keyframe_mb_token_bits_q8(int ymode, const int16_t* mb_coeffs);

// Estimate keyframe intra mode signaling cost using the RFC-aligned trees and
// default probabilities.
//
// Returns a cost in Q8 "bits" (i.e. 1 bit == 256).
uint32_t enc_vp8_estimate_keyframe_ymode_bits_q8(int ymode);
uint32_t enc_vp8_estimate_keyframe_uv_mode_bits_q8(int uv_mode);
uint32_t enc_vp8_estimate_keyframe_bmode_bits_q8(int above_bmode, int left_bmode, int bmode);

// Dry-run coefficient token bitcount using the actual VP8 bool encoder.
//
// This is intended for encoder-side RDO experiments. It encodes only coefficient
// tokens (no mode signaling) for a single macroblock, assuming external contexts
// are 0 (i.e. left/above blocks outside the macroblock are treated as all-zero).
//
// Returns a cost in Q8 "bits" (1 bit == 256).
//
// If coeff_probs_override is NULL, this falls back to the default tables.
uint32_t enc_vp8_dry_run_keyframe_mb_token_bits_q8_probs(int ymode,
                                                        const int16_t* mb_coeffs,
                                                        const uint8_t coeff_probs_override[4][8][3][11]);

#ifdef __cplusplus
}
#endif
