#pragma once

#include <stdint.h>

#include "../common/os.h"

typedef struct {
	uint32_t mb_cols;
	uint32_t mb_rows;
	uint32_t mb_total;
	// Partition sizes/consumption (for RFC 6386 sanity checks).
	uint32_t part0_size_bytes;
	uint32_t part0_bytes_used;
	uint8_t part0_overread;
	uint32_t part0_overread_bytes;
	uint32_t token_part_size_bytes;
	uint32_t token_part_bytes_used;
	uint8_t token_overread;
	uint32_t token_overread_bytes;

	uint32_t mb_skip_coeff;
	uint32_t mb_b_pred;

	// Mode histograms (key frames).
	uint32_t ymode_counts[5];  // DC, V, H, TM, B_PRED
	uint32_t uv_mode_counts[4]; // DC, V, H, TM
	uint32_t bmode_counts[10]; // B_DC, B_TM, B_VE, B_HE, B_LD, B_RD, B_VR, B_VL, B_HD, B_HU

	// Coefficient/block statistics.
	uint32_t blocks_total_y2;
	uint32_t blocks_total_y;
	uint32_t blocks_total_u;
	uint32_t blocks_total_v;
	uint32_t blocks_nonzero_y2;
	uint32_t blocks_nonzero_y;
	uint32_t blocks_nonzero_u;
	uint32_t blocks_nonzero_v;
	uint32_t coeff_nonzero_total;
	uint32_t coeff_eob_tokens;
	uint32_t coeff_abs_max;
	uint64_t coeff_hash_fnv1a64;
} Vp8CoeffStats;

typedef struct {
	uint32_t mb_cols;
	uint32_t mb_rows;
	uint32_t mb_total;

	// Quantization parameters (from the frame header).
	uint8_t q_index;
	int8_t y1_dc_delta_q;
	int8_t y2_dc_delta_q;
	int8_t y2_ac_delta_q;
	int8_t uv_dc_delta_q;
	int8_t uv_ac_delta_q;

	// Segmentation parameters (key frames only; loopfilter deltas ignored for now).
	uint8_t segmentation_enabled;
	uint8_t segmentation_abs;
	int8_t seg_quant_idx[4];
	int8_t seg_lf_level[4];

	// Loop filter parameters (RFC 6386 9.4 / 15).
	uint8_t lf_use_simple;
	uint8_t lf_level;        // 0..63
	uint8_t lf_sharpness;    // 0..7
	uint8_t lf_delta_enabled;
	int8_t lf_ref_delta[4];
	int8_t lf_mode_delta[4];

	// Per-macroblock syntax.
	uint8_t* segment_id; // [mb_total] values 0..3
	uint8_t* skip_coeff; // [mb_total] 0/1
	uint8_t* has_coeff;  // [mb_total] 0/1 (computed from decoded coeffs; used by loopfilter skip logic)
	uint8_t* ymode;      // [mb_total] 0..4 (DC,V,H,TM,B_PRED)
	uint8_t* uv_mode;    // [mb_total] 0..3 (DC,V,H,TM)
	uint8_t* bmode;      // [mb_total*16] (only meaningful for ymode==B_PRED)

	// Residual coefficient blocks, zigzag-reordered into natural coefficient order.
	// Layout per macroblock:
	// - Y2: 1 block  (16 coeffs)
	// - Y:  16 blocks (16 coeffs each)
	// - U:  4 blocks  (16 coeffs each)
	// - V:  4 blocks  (16 coeffs each)
	int16_t* coeff_y2; // [mb_total*16]
	int16_t* coeff_y;  // [mb_total*16*16]
	int16_t* coeff_u;  // [mb_total*4*16]
	int16_t* coeff_v;  // [mb_total*4*16]

	Vp8CoeffStats stats;
} Vp8DecodedFrame;

// Parses macroblock prediction data + coefficient partitions (key frames only)
// and computes a deterministic hash over decoded coefficient values.
//
// Returns 0 on success.
int vp8_decode_coeff_stats(ByteSpan vp8_payload, Vp8CoeffStats* out);

// Decodes keyframe macroblock syntax + coefficient tokens and stores the results
// in heap-allocated arrays in `out`. Call vp8_decoded_frame_free() when done.
int vp8_decode_decoded_frame(ByteSpan vp8_payload, Vp8DecodedFrame* out);

void vp8_decoded_frame_free(Vp8DecodedFrame* f);
