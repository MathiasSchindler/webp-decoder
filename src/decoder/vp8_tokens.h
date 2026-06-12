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

	// Debug: location of first token-partition overread (if any).
	// Filled with 0xFFFFFFFF when not applicable/unknown.
	uint32_t token_overread_mb_index;
	uint32_t token_overread_plane;       // 0=Y, 1=Y2, 2=U, 3=V
	uint32_t token_overread_block_index; // within the plane (Y:0..15, U/V:0..3, Y2:0)
	uint32_t token_overread_coeff_i;     // coefficient index in scan order (0..15)
	uint32_t token_overread_stage;       // 0=token, 1=extra, 2=sign

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
	uint32_t coeff_zero_tokens;
	uint32_t coeff_one_tokens;
	uint32_t coeff_abs_max;
	uint32_t coeff_token_counts[12]; // DCT_0..DCT_4, CAT1..CAT6, EOB
	uint32_t coeff_extra_category_counts[6];
	uint32_t coeff_sign_bits;
	uint32_t coeff_context_updates;
	uint64_t coeff_bool_calls;
	uint64_t coeff_token_bool_calls;
	uint64_t coeff_token_reads;
	uint64_t coeff_token_path_bits;
	uint64_t coeff_extra_bits;
	uint64_t coeff_hash_fnv1a64;
} Vp8CoeffStats;

typedef struct {
	uint64_t part0_header_ns;
	uint64_t part0_mb_syntax_ns;
	uint64_t part0_segment_read_ns;
	uint64_t part0_skip_read_ns;
	uint64_t part0_ymode_read_ns;
	uint64_t part0_bmode_read_ns;
	uint64_t part0_uvmode_read_ns;
	uint64_t token_decode_ns;
	uint64_t token_tree_ns;
	uint64_t token_extra_bits_ns;
	uint64_t token_sign_bits_ns;
	uint64_t token_context_update_ns;
	uint64_t token_plane_ns[4];       // 0=Y, 1=Y2, 2=U, 3=V
	uint64_t token_block_class[4][3]; // per plane: zero, DC-only, has AC
	uint64_t bool_refill_events;
	uint64_t bool_refill_ns;
} Vp8EntropyProfile;

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

typedef struct {
	int16_t y2[16];
	int16_t y[16][16];
	int16_t u[4][16];
	int16_t v[4][16];
} Vp8MacroblockCoeffs;

typedef struct {
	uint8_t segment_id;
	uint8_t skip_coeff;
	uint8_t has_coeff;
	uint8_t has_y2;
	uint8_t ymode;
	uint8_t uv_mode;
	const uint8_t* bmode; // 16 entries when ymode==4, otherwise NULL.
} Vp8MacroblockSyntax;

typedef int (*Vp8MacroblockCoeffVisitor)(void* user, uint32_t mb_index, const Vp8MacroblockCoeffs* coeffs);
typedef int (*Vp8MacroblockVisitor)(void* user, uint32_t mb_index, const Vp8MacroblockSyntax* syntax,
                                    const Vp8MacroblockCoeffs* coeffs);

// Parses macroblock prediction data + coefficient partitions (key frames only)
// and computes a deterministic hash over decoded coefficient values.
//
// Returns 0 on success.
int vp8_decode_coeff_stats(ByteSpan vp8_payload, Vp8CoeffStats* out);

// Decodes keyframe macroblock syntax + coefficient tokens and stores the results
// in heap-allocated arrays in `out`. Call vp8_decoded_frame_free() when done.
int vp8_decode_decoded_frame(ByteSpan vp8_payload, Vp8DecodedFrame* out);

// Opt-in profiling variant. Fills `profile` with deeper entropy/syntax timers
// while decoding; normal decode entry points do not collect these timings.
int vp8_decode_decoded_frame_profiled(ByteSpan vp8_payload, Vp8DecodedFrame* out, Vp8EntropyProfile* profile);

// Decodes keyframe macroblock syntax, but streams coefficients one macroblock at
// a time to `visitor` instead of storing frame-sized coefficient arrays.
// Syntax arrays in `out` are allocated and must be released with
// vp8_decoded_frame_free(). The callback is invoked in raster macroblock order.
int vp8_decode_decoded_frame_visit_coeffs(ByteSpan vp8_payload, Vp8DecodedFrame* out,
                                          Vp8MacroblockCoeffVisitor visitor, void* user);

// Decodes keyframe macroblock syntax + coefficients and streams both records to
// `visitor`. This macroblock-local path fills frame header/loopfilter metadata in
// `out` but does not allocate frame-sized syntax or coefficient arrays.
int vp8_decode_decoded_frame_visit_macroblocks(ByteSpan vp8_payload, Vp8DecodedFrame* out,
                                              Vp8MacroblockVisitor visitor, void* user);

void vp8_decoded_frame_free(Vp8DecodedFrame* f);
